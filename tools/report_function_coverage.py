#!/usr/bin/env python3
"""Report standalone function-map coverage and executable control-flow gaps."""

from __future__ import annotations

import argparse
import csv
import json
import struct
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

from merge_function_config import ConfigError, elf_executable_ranges, load_game_config, sha256_file


@dataclass(frozen=True, order=True)
class Span:
    start: int
    end: int
    name: str


def load_map(path: Path) -> list[Span]:
    spans: list[Span] = []
    try:
        with path.open("r", encoding="utf-8-sig", newline="") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames != ["Name", "Start", "End", "Size"]:
                raise ConfigError(f"{path}: expected CSV header Name,Start,End,Size")
            for line, row in enumerate(reader, 2):
                try:
                    start = int(row["Start"], 0)
                    end = int(row["End"], 0)
                except (TypeError, ValueError) as exc:
                    raise ConfigError(f"{path}:{line}: invalid address") from exc
                name = (row["Name"] or "").strip()
                if not name or start % 4 or end % 4 or end <= start:
                    raise ConfigError(f"{path}:{line}: invalid function record")
                spans.append(Span(start, end, name))
    except OSError as exc:
        raise ConfigError(f"cannot read map {path}: {exc}") from exc
    return sorted(spans)


def executable_words(path: Path):
    data = path.read_bytes()
    phoff = struct.unpack_from("<I", data, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    for index in range(phnum):
        offset = phoff + index * phentsize
        kind, file_offset, vaddr, _, filesz, _, flags, _ = struct.unpack_from(
            "<IIIIIIII", data, offset
        )
        if kind != 1 or not flags & 1:
            continue
        for relative in range(0, filesz - (filesz % 4), 4):
            yield vaddr + relative, struct.unpack_from("<I", data, file_offset + relative)[0]


def merged_ranges(spans: list[Span]) -> list[tuple[int, int]]:
    merged: list[list[int]] = []
    for span in spans:
        if not merged or span.start > merged[-1][1]:
            merged.append([span.start, span.end])
        else:
            merged[-1][1] = max(merged[-1][1], span.end)
    return [(start, end) for start, end in merged]


def contains(ranges: list[tuple[int, int]], address: int) -> bool:
    low = 0
    high = len(ranges)
    while low < high:
        middle = (low + high) // 2
        start, end = ranges[middle]
        if address < start:
            high = middle
        elif address >= end:
            low = middle + 1
        else:
            return True
    return False


def load_binding_addresses(path: Path) -> dict[int, str]:
    try:
        with path.open("rb") as stream:
            config = tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise ConfigError(f"cannot read recompiler config {path}: {exc}") from exc

    general = config.get("general", {})
    result: dict[int, str] = {}
    for key in ("stubs", "untracked_stubs"):
        selectors = general.get(key, [])
        if not isinstance(selectors, list):
            raise ConfigError(f"{path}: general.{key} must be an array")
        for selector in selectors:
            if not isinstance(selector, str) or "@" not in selector:
                raise ConfigError(f"{path}: general.{key} entry lacks an address: {selector!r}")
            name, literal = selector.rsplit("@", 1)
            try:
                address = int(literal, 0)
            except ValueError as exc:
                raise ConfigError(
                    f"{path}: general.{key} entry has an invalid address: {selector!r}"
                ) from exc
            result[address] = name
    return result


def coverage_report(
    elf: Path, spans: list[Span], large_threshold: int,
    bindings: dict[int, str] | None = None,
) -> dict:
    executable = elf_executable_ranges(elf)
    overlaps = []
    for previous, current in zip(spans, spans[1:]):
        if current.start < previous.end:
            overlaps.append({
                "first": previous.name,
                "second": current.name,
                "start": f"0x{current.start:08X}",
                "end": f"0x{min(previous.end, current.end):08X}",
            })

    covered = merged_ranges(spans)
    gaps = []
    covered_bytes = 0
    executable_bytes = sum(end - start for start, end in executable)
    for exec_start, exec_end in executable:
        cursor = exec_start
        for start, end in covered:
            start, end = max(start, exec_start), min(end, exec_end)
            if end <= start:
                continue
            if start > cursor:
                gaps.append((cursor, start))
            covered_bytes += max(0, end - max(cursor, start))
            cursor = max(cursor, end)
        if cursor < exec_end:
            gaps.append((cursor, exec_end))

    missing_targets: dict[int, list[int]] = {}
    missing_call_targets: dict[int, list[int]] = {}
    indirect_sites: list[int] = []
    for address, word in executable_words(elf):
        # Unowned executable bytes frequently contain jump-table or literal
        # data. Only decode control flow from bytes the map claims are code.
        if not contains(covered, address):
            continue
        opcode = word >> 26
        target = None
        if opcode in (2, 3):
            target = ((address + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
        elif opcode in (1, 4, 5, 6, 7, 20, 21, 22, 23) or (
            opcode in (16, 17, 18) and ((word >> 21) & 0x1F) == 8
        ):
            displacement = struct.unpack("<h", struct.pack("<H", word & 0xFFFF))[0] << 2
            target = (address + 4 + displacement) & 0xFFFFFFFF
        elif opcode == 0 and (word & 0x3F) in (8, 9):
            indirect_sites.append(address)
        if target is not None and contains(executable, target) and not contains(covered, target):
            missing_targets.setdefault(target, []).append(address)
            if opcode == 3:
                missing_call_targets.setdefault(target, []).append(address)

    bindings = bindings or {}
    bound_targets = {target: sources for target, sources in missing_targets.items()
                     if target in bindings}
    unresolved_targets = {target: sources for target, sources in missing_targets.items()
                          if target not in bindings}
    unresolved_call_targets = {
        target: sources for target, sources in missing_call_targets.items()
        if target not in bindings
    }

    large = [span for span in spans if span.end - span.start >= large_threshold]
    return {
        "function_count": len(spans),
        "executable_bytes": executable_bytes,
        "covered_bytes": covered_bytes,
        "coverage_percent": round(covered_bytes * 100.0 / executable_bytes, 4),
        "gap_count": len(gaps),
        "gap_bytes": sum(end - start for start, end in gaps),
        "largest_gaps": [
            {"start": f"0x{start:08X}", "end": f"0x{end:08X}", "size": end - start}
            for start, end in sorted(gaps, key=lambda item: item[1] - item[0], reverse=True)[:25]
        ],
        "overlap_count": len(overlaps),
        "overlaps": overlaps[:25],
        "large_function_count": len(large),
        "largest_functions": [
            {"name": span.name, "start": f"0x{span.start:08X}", "size": span.end - span.start}
            for span in sorted(large, key=lambda item: item.end - item.start, reverse=True)[:25]
        ],
        "indirect_control_flow_sites": len(indirect_sites),
        "intentional_binding_target_count": len(bound_targets),
        "intentional_binding_targets": [
            {"target": f"0x{target:08X}", "binding": bindings[target],
             "sources": [f"0x{source:08X}" for source in sources[:16]]}
            for target, sources in sorted(bound_targets.items())
        ],
        "uncovered_direct_target_count": len(unresolved_targets),
        "uncovered_direct_targets": [
            {"target": f"0x{target:08X}",
             "sources": [f"0x{source:08X}" for source in sources[:16]]}
            for target, sources in sorted(unresolved_targets.items())[:100]
        ],
        "uncovered_direct_call_target_count": len(unresolved_call_targets),
        "uncovered_direct_call_targets": [
            {"target": f"0x{target:08X}",
             "sources": [f"0x{source:08X}" for source in sources[:16]]}
            for target, sources in sorted(unresolved_call_targets.items())
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--map", required=True, type=Path)
    parser.add_argument("--game", default=Path("config/game.toml"), type=Path)
    parser.add_argument("--large-threshold", type=lambda value: int(value, 0), default=0x2000)
    parser.add_argument("--config", type=Path,
                        help="effective TOML whose explicit runtime bindings satisfy targets")
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args(argv)
    try:
        game, _ = load_game_config(args.game)
        actual = sha256_file(args.elf)
        if actual != game["elf_sha256"]:
            raise ConfigError(f"ELF SHA-256 mismatch: expected {game['elf_sha256']}, found {actual}")
        bindings = load_binding_addresses(args.config) if args.config else None
        report = coverage_report(
            args.elf, load_map(args.map), args.large_threshold, bindings,
        )
    except (ConfigError, OSError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 1 if report["overlap_count"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
