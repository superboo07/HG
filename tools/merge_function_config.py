#!/usr/bin/env python3
"""Validate and merge standalone function metadata for PS2Recomp.

Ghidra can produce the optional analysis CSV, but this program has no Ghidra
dependency. It consumes only an ELF, committed TOML, and an optional standalone
CSV, then emits PS2Recomp-compatible standalone files.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import struct
import sys
import tomllib
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, Iterable


class ConfigError(ValueError):
    """Raised when build or function metadata is unsafe or inconsistent."""


@dataclass(frozen=True, order=True)
class FunctionEntry:
    start: int
    end: int
    name: str
    mode: str = "recompile"
    handler: str = ""
    confidence: str = "unknown"
    evidence: str = ""
    reason: str = ""
    source: str = "analysis"
    override_analysis: bool = False
    expected_analysis_end: int | None = None

    @property
    def size(self) -> int:
        return self.end - self.start


def _read_toml(path: Path) -> dict[str, Any]:
    try:
        with path.open("rb") as stream:
            return tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise ConfigError(f"cannot read TOML {path}: {exc}") from exc


def _required(table: dict[str, Any], key: str, expected: type, context: str) -> Any:
    if key not in table:
        raise ConfigError(f"{context}: missing required field '{key}'")
    value = table[key]
    if not isinstance(value, expected):
        raise ConfigError(f"{context}.{key}: expected {expected.__name__}")
    return value


def _parse_address(value: str | int, context: str) -> int:
    try:
        result = int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError) as exc:
        raise ConfigError(f"{context}: invalid address {value!r}") from exc
    if result < 0 or result > 0xFFFFFFFF:
        raise ConfigError(f"{context}: address is outside the 32-bit range")
    return result


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as exc:
        raise ConfigError(f"cannot hash ELF {path}: {exc}") from exc
    return digest.hexdigest()


def elf_executable_ranges(path: Path) -> list[tuple[int, int]]:
    """Return executable virtual-address ranges from an ELF32 little-endian MIPS file."""
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ConfigError(f"cannot read ELF {path}: {exc}") from exc

    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise ConfigError(f"{path}: not an ELF file")
    if data[4] != 1 or data[5] != 1:
        raise ConfigError(f"{path}: expected a 32-bit little-endian ELF")

    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    machine, phoff, shoff = header[1], header[4], header[5]
    phentsize, phnum, shentsize, shnum = header[8], header[9], header[10], header[11]
    if machine != 8:
        raise ConfigError(f"{path}: expected EM_MIPS (8), found {machine}")

    ranges: list[tuple[int, int]] = []
    if shoff and shentsize >= 40:
        for index in range(shnum):
            offset = shoff + index * shentsize
            if offset + 40 > len(data):
                raise ConfigError(f"{path}: truncated section header table")
            section = struct.unpack_from("<IIIIIIIIII", data, offset)
            flags, address, size = section[2], section[3], section[5]
            if flags & 0x4 and size:
                ranges.append((address, address + size))

    if not ranges and phoff and phentsize >= 32:
        for index in range(phnum):
            offset = phoff + index * phentsize
            if offset + 32 > len(data):
                raise ConfigError(f"{path}: truncated program header table")
            segment = struct.unpack_from("<IIIIIIII", data, offset)
            kind, address, memory_size, flags = segment[0], segment[2], segment[5], segment[6]
            if kind == 1 and flags & 0x1 and memory_size:
                ranges.append((address, address + memory_size))

    if not ranges:
        raise ConfigError(f"{path}: no executable sections or segments found")
    return _merge_ranges(ranges)


def _merge_ranges(ranges: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[list[int]] = []
    for start, end in sorted(ranges):
        if not merged or start > merged[-1][1]:
            merged.append([start, end])
        else:
            merged[-1][1] = max(merged[-1][1], end)
    return [(start, end) for start, end in merged]


def load_game_config(path: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    root = _read_toml(path)
    if root.get("schema_version") != 1:
        raise ConfigError(f"{path}: unsupported schema_version")
    game = _required(root, "game", dict, str(path))
    recomp = _required(root, "recompiler", dict, str(path))
    _required(game, "id", str, "game")
    expected_hash = _required(game, "elf_sha256", str, "game").lower()
    if len(expected_hash) != 64 or any(c not in "0123456789abcdef" for c in expected_hash):
        raise ConfigError("game.elf_sha256: expected 64 lowercase hexadecimal characters")
    excluded = recomp.get("exclude_analyzer_functions", [])
    if not isinstance(excluded, list) or any(
        not isinstance(name, str) or not name.strip() or any(char in name for char in "\r\n,")
        for name in excluded
    ):
        raise ConfigError("recompiler.exclude_analyzer_functions: expected valid function names")
    if len(set(excluded)) != len(excluded):
        raise ConfigError("recompiler.exclude_analyzer_functions: duplicate function name")
    excluded_stubs = recomp.get("exclude_base_stubs", [])
    if not isinstance(excluded_stubs, list) or any(
        not isinstance(stub, str) or not stub.strip() or any(char in stub for char in "\r\n,")
        for stub in excluded_stubs
    ):
        raise ConfigError("recompiler.exclude_base_stubs: expected valid stub specifications")
    if len(set(excluded_stubs)) != len(excluded_stubs):
        raise ConfigError("recompiler.exclude_base_stubs: duplicate stub specification")
    excluded_patches = recomp.get("exclude_base_patches", [])
    if not isinstance(excluded_patches, list) or any(
        not isinstance(address, int) or isinstance(address, bool)
        or address < 0 or address > 0xFFFFFFFF or address % 4
        for address in excluded_patches
    ):
        raise ConfigError("recompiler.exclude_base_patches: expected aligned 32-bit addresses")
    if len(set(excluded_patches)) != len(excluded_patches):
        raise ConfigError("recompiler.exclude_base_patches: duplicate patch address")
    return game, recomp


def load_analysis_map(
    path: Path | None,
    source: str = "standalone_analysis",
) -> list[FunctionEntry]:
    if path is None:
        return []
    entries: list[FunctionEntry] = []
    try:
        with path.open("r", encoding="utf-8-sig", newline="") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames != ["Name", "Start", "End", "Size"]:
                raise ConfigError(f"{path}: expected CSV header Name,Start,End,Size")
            for line, row in enumerate(reader, 2):
                name = (row["Name"] or "").strip()
                if not name or any(char in name for char in "\r\n,"):
                    raise ConfigError(f"{path}:{line}: invalid function name")
                start = _parse_address(row["Start"], f"{path}:{line} Start")
                end = _parse_address(row["End"], f"{path}:{line} End")
                size = _parse_address(row["Size"], f"{path}:{line} Size")
                # Ghidra function bodies may be discontiguous. In that case Size
                # is the body byte count while End is max(body)+1; PS2Recomp uses
                # Start/End and ignores Size.
                if end <= start or size <= 0:
                    raise ConfigError(f"{path}:{line}: inconsistent function range/size")
                # Unsupported/undefined instructions can leave Ghidra with a
                # one-byte body on this fixed-width ISA. PS2Recomp consumes
                # four-byte words, so normalize the exclusive bound upward.
                end = (end + 3) & ~3
                # PS2Recomp treats FUN_/sub_/LAB_/DAT_ as autogenerated and may
                # prefer a longer heuristic function at the same start. Give
                # standalone analysis records a non-auto provenance name so the
                # reviewed map boundary wins deterministically.
                if name.startswith(("FUN_", "sub_", "LAB_", "DAT_")):
                    name = f"ghidra_{start:08X}"
                entries.append(FunctionEntry(start, end, name, source=source))
    except OSError as exc:
        raise ConfigError(f"cannot read analysis map {path}: {exc}") from exc
    unique_entries: list[FunctionEntry] = []
    used_names: set[str] = set()
    for entry in entries:
        name = entry.name
        if name in used_names:
            name = f"{name}__at_{entry.start:08X}"
            suffix = 2
            while name in used_names:
                name = f"{entry.name}__at_{entry.start:08X}_{suffix}"
                suffix += 1
            entry = replace(entry, name=name)
        used_names.add(name)
        unique_entries.append(entry)
    ordered = sorted(unique_entries)
    contiguous: list[FunctionEntry] = []
    for index, entry in enumerate(ordered):
        if index + 1 < len(ordered):
            next_start = ordered[index + 1].start
            if entry.start < next_start < entry.end:
                entry = replace(entry, end=next_start)
        contiguous.append(entry)
    return contiguous


def _read_elf_words(path: Path, start: int, end: int) -> list[int]:
    """Read a virtual-address range from file-backed ELF32 PT_LOAD data."""
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ConfigError(f"cannot read ELF {path}: {exc}") from exc
    if end <= start or start % 4 or end % 4:
        raise ConfigError("function table range must be non-empty and four-byte aligned")
    if len(data) < 52 or data[:6] != b"\x7fELF\x01\x01":
        raise ConfigError(f"{path}: expected a 32-bit little-endian ELF")
    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    phoff, phentsize, phnum = header[4], header[8], header[9]
    for index in range(phnum):
        offset = phoff + index * phentsize
        if offset + 32 > len(data):
            raise ConfigError(f"{path}: truncated program header table")
        segment = struct.unpack_from("<IIIIIIII", data, offset)
        kind, file_offset, address, file_size = segment[0], segment[1], segment[2], segment[4]
        if kind == 1 and start >= address and end <= address + file_size:
            first = file_offset + start - address
            return list(struct.unpack_from(f"<{(end - start) // 4}I", data, first))
    raise ConfigError(
        f"function table 0x{start:08X}-0x{end:08X} is not file-backed ELF data"
    )


def load_manual_config(path: Path, build_id: str, elf_path: Path) -> list[FunctionEntry]:
    root = _read_toml(path)
    if root.get("schema_version") != 1:
        raise ConfigError(f"{path}: unsupported schema_version")
    if root.get("build") != build_id:
        raise ConfigError(f"{path}: build does not match {build_id}")

    entries: list[FunctionEntry] = []
    for index, item in enumerate(root.get("functions", []), 1):
        context = f"functions[{index}]"
        if not isinstance(item, dict):
            raise ConfigError(f"{context}: expected a table")
        name = _required(item, "name", str, context).strip()
        if not name or any(char in name for char in "\r\n,"):
            raise ConfigError(f"{context}.name: invalid standalone-map name")
        start = _parse_address(_required(item, "start", int, context), f"{context}.start")
        end = _parse_address(_required(item, "end", int, context), f"{context}.end")
        mode = _required(item, "mode", str, context)
        confidence = _required(item, "confidence", str, context)
        evidence = _required(item, "evidence", str, context).strip()
        reason = _required(item, "reason", str, context).strip()
        handler = item.get("handler", "")
        override_analysis = item.get("override_analysis", False)
        if not isinstance(handler, str):
            raise ConfigError(f"{context}.handler: expected str")
        if not isinstance(override_analysis, bool):
            raise ConfigError(f"{context}.override_analysis: expected bool")
        if mode not in {"recompile", "runtime_bind", "skip_for_triage"}:
            raise ConfigError(f"{context}.mode: unsupported mode {mode!r}")
        if confidence not in {"low", "medium", "high"}:
            raise ConfigError(f"{context}.confidence: use low, medium, or high")
        if mode == "runtime_bind" and not handler.strip():
            raise ConfigError(f"{context}.handler: required for runtime_bind")
        if mode != "runtime_bind" and handler.strip():
            raise ConfigError(f"{context}.handler: only valid for runtime_bind")
        if not evidence or not reason:
            raise ConfigError(f"{context}: evidence and reason cannot be empty")
        entries.append(FunctionEntry(start, end, name, mode, handler.strip(), confidence,
                                     evidence, reason, "manual", override_analysis))

    for index, item in enumerate(root.get("function_tables", []), 1):
        context = f"function_tables[{index}]"
        if not isinstance(item, dict):
            raise ConfigError(f"{context}: expected a table")
        prefix = _required(item, "name_prefix", str, context).strip()
        if not prefix or any(char in prefix for char in "\r\n,"):
            raise ConfigError(f"{context}.name_prefix: invalid standalone-map prefix")
        table_start = _parse_address(
            _required(item, "table_start", int, context), f"{context}.table_start"
        )
        table_end = _parse_address(
            _required(item, "table_end", int, context), f"{context}.table_end"
        )
        code_end = _parse_address(
            _required(item, "code_end", int, context), f"{context}.code_end"
        )
        confidence = _required(item, "confidence", str, context)
        evidence = _required(item, "evidence", str, context).strip()
        reason = _required(item, "reason", str, context).strip()
        override_analysis = item.get("override_analysis", False)
        if not isinstance(override_analysis, bool):
            raise ConfigError(f"{context}.override_analysis: expected bool")
        if confidence not in {"low", "medium", "high"}:
            raise ConfigError(f"{context}.confidence: use low, medium, or high")
        if not evidence or not reason:
            raise ConfigError(f"{context}: evidence and reason cannot be empty")
        starts = _read_elf_words(elf_path, table_start, table_end)
        if not starts:
            raise ConfigError(f"{context}: function table cannot be empty")
        if any(address % 4 for address in starts):
            raise ConfigError(f"{context}: table contains an unaligned function address")
        if any(current <= previous for previous, current in zip(starts, starts[1:])):
            raise ConfigError(f"{context}: function addresses must be strictly increasing")
        if code_end <= starts[-1]:
            raise ConfigError(f"{context}.code_end: must follow the final function start")
        boundaries = starts[1:] + [code_end]
        entries.extend(
            FunctionEntry(
                start, end, f"{prefix}_{start:08X}", "recompile", "", confidence,
                evidence, reason, "manual_table", override_analysis,
            )
            for start, end in zip(starts, boundaries)
        )

    for set_index, item in enumerate(root.get("function_sets", []), 1):
        context = f"function_sets[{set_index}]"
        if not isinstance(item, dict):
            raise ConfigError(f"{context}: expected a table")
        prefix = _required(item, "name_prefix", str, context).strip()
        confidence = _required(item, "confidence", str, context)
        evidence = _required(item, "evidence", str, context).strip()
        reason = _required(item, "reason", str, context).strip()
        override_analysis = item.get("override_analysis", False)
        ranges = _required(item, "ranges", list, context)
        if not prefix or any(char in prefix for char in "\r\n,"):
            raise ConfigError(f"{context}.name_prefix: invalid standalone-map prefix")
        if confidence not in {"low", "medium", "high"}:
            raise ConfigError(f"{context}.confidence: use low, medium, or high")
        if not isinstance(override_analysis, bool):
            raise ConfigError(f"{context}.override_analysis: expected bool")
        if not override_analysis:
            raise ConfigError(f"{context}.override_analysis: function sets must replace analysis")
        if not evidence or not reason or not ranges:
            raise ConfigError(f"{context}: evidence, reason, and ranges cannot be empty")
        for range_index, record in enumerate(ranges, 1):
            range_context = f"{context}.ranges[{range_index}]"
            if not isinstance(record, dict):
                raise ConfigError(f"{range_context}: expected a table")
            start = _parse_address(
                _required(record, "start", int, range_context), f"{range_context}.start"
            )
            analysis_end = _parse_address(
                _required(record, "analysis_end", int, range_context),
                f"{range_context}.analysis_end",
            )
            end = _parse_address(
                _required(record, "end", int, range_context), f"{range_context}.end"
            )
            entries.append(FunctionEntry(
                start, end, f"{prefix}_{start:08X}", "recompile", "", confidence,
                evidence, reason, "manual_set", True, analysis_end,
            ))

    for set_index, item in enumerate(root.get("missing_function_sets", []), 1):
        context = f"missing_function_sets[{set_index}]"
        if not isinstance(item, dict):
            raise ConfigError(f"{context}: expected a table")
        prefix = _required(item, "name_prefix", str, context).strip()
        confidence = _required(item, "confidence", str, context)
        evidence = _required(item, "evidence", str, context).strip()
        reason = _required(item, "reason", str, context).strip()
        ranges = _required(item, "ranges", list, context)
        if not prefix or any(char in prefix for char in "\r\n,"):
            raise ConfigError(f"{context}.name_prefix: invalid standalone-map prefix")
        if confidence not in {"low", "medium", "high"}:
            raise ConfigError(f"{context}.confidence: use low, medium, or high")
        if not evidence or not reason or not ranges:
            raise ConfigError(f"{context}: evidence, reason, and ranges cannot be empty")
        for range_index, record in enumerate(ranges, 1):
            range_context = f"{context}.ranges[{range_index}]"
            if not isinstance(record, dict):
                raise ConfigError(f"{range_context}: expected a table")
            start = _parse_address(
                _required(record, "start", int, range_context), f"{range_context}.start"
            )
            end = _parse_address(
                _required(record, "end", int, range_context), f"{range_context}.end"
            )
            entries.append(FunctionEntry(
                start, end, f"{prefix}_{start:08X}", "recompile", "", confidence,
                evidence, reason, "manual_missing_set", False, None,
            ))
    return entries


def apply_manual_overrides(
    analysis: list[FunctionEntry], manual: list[FunctionEntry]
) -> list[FunctionEntry]:
    """Replace analyzer records only when a manual entry explicitly opts in."""
    overridden_starts = {entry.start for entry in manual if entry.override_analysis}
    required_starts = {
        entry.start for entry in manual
        if entry.override_analysis and entry.source in {"manual", "manual_set"}
    }
    analysis_starts = {entry.start for entry in analysis}
    missing = sorted(required_starts - analysis_starts)
    if missing:
        formatted = ", ".join(f"0x{address:08X}" for address in missing)
        raise ConfigError(
            f"manual override_analysis has no analysis entry at: {formatted}"
        )
    analysis_by_start = {entry.start: entry for entry in analysis}
    stale_ranges = [
        entry for entry in manual
        if entry.expected_analysis_end is not None
        and analysis_by_start[entry.start].end != entry.expected_analysis_end
    ]
    if stale_ranges:
        entry = stale_ranges[0]
        actual = analysis_by_start[entry.start].end
        raise ConfigError(
            f"{entry.name}: expected analysis end 0x{entry.expected_analysis_end:08X}, "
            f"found 0x{actual:08X}"
        )
    return [entry for entry in analysis if entry.start not in overridden_starts]


def validate_entries(
    analysis: list[FunctionEntry],
    manual: list[FunctionEntry],
    executable_ranges: list[tuple[int, int]],
    release: bool,
) -> None:
    all_entries = analysis + manual
    starts: dict[int, FunctionEntry] = {}
    names: dict[str, FunctionEntry] = {}
    for entry in all_entries:
        if entry.start % 4 or entry.end % 4:
            raise ConfigError(f"{entry.name}: range must be four-byte aligned")
        if entry.end <= entry.start:
            raise ConfigError(f"{entry.name}: end must be greater than start")
        if not any(entry.start >= lo and entry.end <= hi for lo, hi in executable_ranges):
            raise ConfigError(f"{entry.name}: range is outside executable ELF sections")
        if entry.start in starts:
            raise ConfigError(
                f"duplicate start 0x{entry.start:08X}: {starts[entry.start].name} and {entry.name}"
            )
        if entry.name in names:
            raise ConfigError(f"duplicate name: {entry.name}")
        starts[entry.start] = entry
        names[entry.name] = entry
        if release and entry.mode == "skip_for_triage":
            raise ConfigError(f"{entry.name}: skip_for_triage is forbidden in release mode")

    ordered_manual = sorted(manual)
    for previous, current in zip(ordered_manual, ordered_manual[1:]):
        if current.start < previous.end:
            raise ConfigError(f"manual functions overlap: {previous.name} and {current.name}")

    for candidate in manual:
        for existing in analysis:
            if candidate.start < existing.end and existing.start < candidate.end:
                raise ConfigError(
                    f"manual function {candidate.name} overlaps analysis entry {existing.name}"
                )


def write_map(path: Path, entries: list[FunctionEntry]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(["Name", "Start", "End", "Size"])
        for entry in sorted(entries):
            writer.writerow([
                entry.name,
                f"0x{entry.start:08X}",
                f"0x{entry.end:08X}",
                entry.size,
            ])


def write_provenance_report(path: Path, entries: list[FunctionEntry]) -> None:
    """Write the human-auditable origin and review facts for every boundary."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow([
            "Name", "Start", "End", "Size", "Source", "Mode", "Confidence",
            "Handler", "Evidence", "Reason",
        ])
        for entry in sorted(entries):
            writer.writerow([
                entry.name,
                f"0x{entry.start:08X}",
                f"0x{entry.end:08X}",
                entry.size,
                entry.source,
                entry.mode,
                entry.confidence,
                entry.handler,
                entry.evidence,
                entry.reason,
            ])


def _toml_quote(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _toml_array(values: Iterable[str]) -> str:
    return "[" + ", ".join(_toml_quote(value) for value in values) + "]"


def _inline_toml(value: Any) -> str:
    if isinstance(value, bool):
        return str(value).lower()
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        return _toml_quote(value)
    if isinstance(value, list):
        return "[" + ", ".join(_inline_toml(item) for item in value) + "]"
    if isinstance(value, dict):
        fields = ", ".join(f"{key} = {_inline_toml(item)}" for key, item in value.items())
        return "{ " + fields + " }"
    raise ConfigError(f"cannot serialize TOML value of type {type(value).__name__}")


def _append_array(lines: list[str], key: str, values: Iterable[Any]) -> None:
    items = list(values)
    if not items:
        lines.append(f"{key} = []")
        return
    lines.append(f"{key} = [")
    lines.extend(f"  {_inline_toml(item)}," for item in items)
    lines.append("]")


def write_recompiler_config(
    path: Path,
    elf_path: Path,
    map_path: Path,
    output_dir: Path,
    recomp: dict[str, Any],
    manual: list[FunctionEntry],
    base: dict[str, Any] | None = None,
) -> None:
    base = base or {}
    base_general = base.get("general", {})
    if not isinstance(base_general, dict):
        raise ConfigError("base config [general] must be a table")
    excluded_base_stubs = set(recomp.get("exclude_base_stubs", []))
    stubs = [stub for stub in base_general.get("stubs", [])
             if stub not in excluded_base_stubs]
    stubs.extend(f"{entry.handler}@0x{entry.start:08X}" for entry in manual
                 if entry.mode == "runtime_bind")
    skips = list(base_general.get("skip", []))
    skips.extend(recomp.get("exclude_analyzer_functions", []))
    skips.extend(entry.name for entry in manual if entry.mode == "skip_for_triage")
    untracked_stubs = list(base_general.get("untracked_stubs", []))
    lines = [
        "# Generated by tools/merge_function_config.py; do not edit.",
        "# The standalone map may originate from any analysis tool; Ghidra is not required.",
        "",
        "[general]",
        f"input = {_toml_quote(str(elf_path.resolve()))}",
        f"output = {_toml_quote(str(output_dir.resolve()))}",
        f"ghidra_output = {_toml_quote(str(map_path.resolve()))}",
        f"single_file_output = {str(bool(recomp.get('single_file_output', False))).lower()}",
        f"low_memory_mode = {str(bool(recomp.get('low_memory_mode', True))).lower()}",
        f"output_worker_threads = {int(recomp.get('output_worker_threads', 0))}",
        f"patch_syscalls = {str(bool(recomp.get('patch_syscalls', False))).lower()}",
        f"patch_cop0 = {str(bool(recomp.get('patch_cop0', True))).lower()}",
        f"patch_cache = {str(bool(recomp.get('patch_cache', True))).lower()}",
    ]
    _append_array(lines, "stubs", stubs)
    _append_array(lines, "untracked_stubs", untracked_stubs)
    _append_array(lines, "skip", skips)

    mmio = base.get("mmio", {})
    if mmio:
        if not isinstance(mmio, dict):
            raise ConfigError("base config [mmio] must be a table")
        lines.extend(["", "[mmio]"])
        for key in sorted(mmio, key=lambda item: int(item, 0)):
            lines.append(f"{_toml_quote(key)} = {_inline_toml(mmio[key])}")

    jump_tables = base.get("jump_tables", {})
    if jump_tables:
        tables = jump_tables.get("table", []) if isinstance(jump_tables, dict) else None
        if not isinstance(tables, list):
            raise ConfigError("base config jump_tables.table must be an array")
        lines.extend(["", "[jump_tables]"])
        for table in tables:
            if not isinstance(table, dict):
                raise ConfigError("base config jump table entry must be a table")
            lines.extend(["", "[[jump_tables.table]]"])
            for key, value in table.items():
                if key == "entries":
                    _append_array(lines, key, value)
                else:
                    lines.append(f"{key} = {_inline_toml(value)}")

    patches = base.get("patches", {})
    instructions = patches.get("instructions", []) if isinstance(patches, dict) else None
    if not isinstance(instructions, list):
        raise ConfigError("base config patches.instructions must be an array")
    excluded_base_patches = set(recomp.get("exclude_base_patches", []))
    filtered_instructions = []
    for instruction in instructions:
        address = instruction.get("address") if isinstance(instruction, dict) else None
        try:
            numeric_address = int(address, 0) if isinstance(address, str) else int(address)
        except (TypeError, ValueError):
            numeric_address = None
        if numeric_address not in excluded_base_patches:
            filtered_instructions.append(instruction)
    lines.extend(["", "[patches]"])
    _append_array(lines, "instructions", filtered_instructions)

    performance = base.get("performance", {})
    if performance:
        if not isinstance(performance, dict):
            raise ConfigError("base config [performance] must be a table")
        lines.extend(["", "[performance]"])
        for key, value in performance.items():
            if isinstance(value, list):
                _append_array(lines, key, value)
            else:
                lines.append(f"{key} = {_inline_toml(value)}")
    lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def merge(
    elf_path: Path,
    game_path: Path,
    manual_path: Path,
    analysis_path: Path | None,
    output_map: Path,
    output_config: Path,
    generated_code_dir: Path,
    release: bool = False,
    base_config_path: Path | None = None,
    provenance_report_path: Path | None = None,
    analysis_source: str = "standalone_analysis",
) -> tuple[int, int]:
    game, recomp = load_game_config(game_path)
    actual_hash = sha256_file(elf_path)
    if actual_hash != game["elf_sha256"].lower():
        raise ConfigError(
            f"ELF SHA-256 mismatch for {game['id']}: expected {game['elf_sha256']}, "
            f"found {actual_hash}"
        )
    ranges = elf_executable_ranges(elf_path)
    analysis = load_analysis_map(analysis_path, analysis_source)
    manual = load_manual_config(manual_path, game["id"], elf_path)
    if manual and analysis_path is None:
        raise ConfigError(
            "manual functions require --analysis-map containing the complete standalone function map"
        )
    analysis = apply_manual_overrides(analysis, manual)
    validate_entries(analysis, manual, ranges, release)
    base = _read_toml(base_config_path) if base_config_path else None
    merged_entries = analysis + manual
    write_map(output_map, merged_entries)
    if provenance_report_path is not None:
        write_provenance_report(provenance_report_path, merged_entries)
    write_recompiler_config(output_config, elf_path, output_map, generated_code_dir,
                            recomp, manual, base)
    return len(analysis), len(manual)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--game", default=Path("config/game.toml"), type=Path)
    parser.add_argument("--manual", default=Path("config/functions.manual.toml"), type=Path)
    parser.add_argument("--analysis-map", type=Path)
    parser.add_argument("--base-config", type=Path,
                        help="optional analyzer TOML whose stubs, MMIO, jump tables, and patches are preserved")
    parser.add_argument("--output-map", default=Path("build/generated/functions.effective.csv"), type=Path)
    parser.add_argument("--output-config", default=Path("build/generated/recomp.effective.toml"), type=Path)
    parser.add_argument("--provenance-report", default=Path("build/generated/functions.provenance.csv"), type=Path)
    parser.add_argument(
        "--analysis-source",
        choices=("ghidra", "ps2recomp_analyzer", "standalone_analysis"),
        default="standalone_analysis",
        help="provenance label assigned to records in --analysis-map",
    )
    parser.add_argument("--generated-code-dir", default=Path("build/generated/recompiled"), type=Path)
    parser.add_argument("--release", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        analysis_count, manual_count = merge(
            args.elf, args.game, args.manual, args.analysis_map,
            args.output_map, args.output_config, args.generated_code_dir, args.release,
            args.base_config, args.provenance_report, args.analysis_source,
        )
    except ConfigError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(f"validated {analysis_count} analysis and {manual_count} manual functions")
    print(
        f"wrote {args.output_map}, {args.output_config}, and "
        f"{args.provenance_report}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
