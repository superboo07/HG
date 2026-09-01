#!/usr/bin/env python3
"""Build a complete code/data/padding/investigated-gap ledger for the ELF."""

from __future__ import annotations

import argparse
import json
import struct
import sys
import tomllib
from pathlib import Path

from merge_function_config import ConfigError, elf_executable_ranges, load_game_config, sha256_file
from report_function_coverage import load_map, merged_ranges


def executable_image(path: Path) -> dict[int, int]:
    data = path.read_bytes()
    phoff = struct.unpack_from("<I", data, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    image: dict[int, int] = {}
    for index in range(phnum):
        offset = phoff + index * phentsize
        kind, file_offset, vaddr, _, filesz, _, flags, _ = struct.unpack_from(
            "<IIIIIIII", data, offset
        )
        if kind != 1 or not flags & 1:
            continue
        if file_offset + filesz > len(data):
            raise ConfigError(f"{path}: truncated executable segment")
        image.update(enumerate(data[file_offset:file_offset + filesz], vaddr))
    return image


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ConfigError(f"cannot read audit {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ConfigError(f"{path}: expected a JSON object")
    return value


def require_cleared_audits(
    coverage: dict, pointers: dict, boundaries: dict, leaves: dict,
) -> None:
    checks = (
        (coverage, "overlap_count"),
        (coverage, "uncovered_direct_target_count"),
        (coverage, "uncovered_direct_call_target_count"),
        (pointers, "candidate_count"),
        (boundaries, "candidate_count"),
        (boundaries, "invalid_manual_set_count"),
        (leaves, "candidate_count"),
    )
    for report, key in checks:
        if report.get(key) != 0:
            raise ConfigError(f"audit gate {key} is not zero (found {report.get(key)!r})")


def partition(
    executable: list[tuple[int, int]], code: list[tuple[int, int]],
    image: dict[int, int], data_start: int,
) -> list[dict[str, object]]:
    entries: list[dict[str, object]] = []

    def add_gap(start: int, end: int) -> None:
        if start >= end:
            return
        if start < data_start < end:
            add_gap(start, data_start)
            add_gap(data_start, end)
            return
        values = bytes(image.get(address, 0) for address in range(start, end))
        if start >= data_start:
            classification = "data"
            evidence = "reviewed file-backed data boundary"
        elif values and not any(values):
            classification = "padding"
            evidence = "all bytes are zero/NOP alignment"
        else:
            classification = "investigated_gap"
            evidence = "direct, pointer, boundary, and leaf audit gates are clear"
        pointer_words = 0
        for offset in range(0, len(values) - (len(values) % 4), 4):
            value = struct.unpack_from("<I", values, offset)[0]
            if value % 4 == 0 and value in image:
                pointer_words += 1
        entries.append({
            "start": f"0x{start:08X}",
            "end": f"0x{end:08X}",
            "size": end - start,
            "classification": classification,
            "evidence": evidence,
            "nonzero_bytes": sum(value != 0 for value in values),
            "pointer_like_words": pointer_words,
        })

    for exec_start, exec_end in executable:
        cursor = exec_start
        for code_start, code_end in code:
            start = max(exec_start, code_start)
            end = min(exec_end, code_end)
            if end <= start:
                continue
            add_gap(cursor, start)
            entries.append({
                "start": f"0x{start:08X}",
                "end": f"0x{end:08X}",
                "size": end - start,
                "classification": "code",
                "evidence": "owned by the validated effective function map",
            })
            cursor = max(cursor, end)
        add_gap(cursor, exec_end)
    return entries


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--map", required=True, type=Path)
    parser.add_argument("--game", default=Path("config/game.toml"), type=Path)
    parser.add_argument("--coverage-audit", required=True, type=Path)
    parser.add_argument("--pointer-audit", required=True, type=Path)
    parser.add_argument("--boundary-audit", required=True, type=Path)
    parser.add_argument("--leaf-audit", required=True, type=Path)
    parser.add_argument("--json-out", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        game, _ = load_game_config(args.game)
        if sha256_file(args.elf) != game["elf_sha256"]:
            raise ConfigError("ELF SHA-256 mismatch")
        with args.game.open("rb") as stream:
            root = tomllib.load(stream)
        layout = root.get("layout", {})
        data_start = layout.get("file_backed_data_start")
        evidence = layout.get("evidence")
        if not isinstance(data_start, int) or data_start % 4 or not isinstance(evidence, str):
            raise ConfigError("game layout requires aligned file_backed_data_start and evidence")

        coverage = load_json(args.coverage_audit)
        pointers = load_json(args.pointer_audit)
        boundaries = load_json(args.boundary_audit)
        leaves = load_json(args.leaf_audit)
        require_cleared_audits(coverage, pointers, boundaries, leaves)

        executable = elf_executable_ranges(args.elf)
        code = merged_ranges(load_map(args.map))
        entries = partition(executable, code, executable_image(args.elf), data_start)
        executable_bytes = sum(end - start for start, end in executable)
        classified_bytes = sum(int(entry["size"]) for entry in entries)
        if classified_bytes != executable_bytes:
            raise ConfigError(
                f"classification is incomplete: {classified_bytes} of {executable_bytes} bytes"
            )
        counts: dict[str, int] = {}
        range_counts: dict[str, int] = {}
        for entry in entries:
            kind = str(entry["classification"])
            counts[kind] = counts.get(kind, 0) + int(entry["size"])
            range_counts[kind] = range_counts.get(kind, 0) + 1
        result = {
            "build": game["id"],
            "elf_sha256": game["elf_sha256"],
            "map": str(args.map),
            "file_backed_data_start": f"0x{data_start:08X}",
            "data_boundary_evidence": evidence,
            "executable_bytes": executable_bytes,
            "classified_bytes": classified_bytes,
            "classification_bytes": counts,
            "classification_ranges": range_counts,
            "audit_inputs": {
                "coverage": str(args.coverage_audit),
                "pointers": str(args.pointer_audit),
                "boundaries": str(args.boundary_audit),
                "leaves": str(args.leaf_audit),
            },
            "entries": entries,
        }
    except (ConfigError, OSError, struct.error, tomllib.TOMLDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    args.json_out.parent.mkdir(parents=True, exist_ok=True)
    args.json_out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in (
        "build", "executable_bytes", "classified_bytes",
        "classification_bytes", "classification_ranges",
    )}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
