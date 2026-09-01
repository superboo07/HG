#!/usr/bin/env python3
"""Find conservative ELF-backed candidates for truncated analyzed functions.

This is a triage report, not an automatic config writer. A candidate is emitted
only when a directly referenced function has a stack-frame prologue, its mapped
range lacks a terminal return/tail jump, and the ELF reaches a clean ``JR ra``
before another mapped function or suspicious nested prologue.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from bisect import bisect_right
from pathlib import Path

from merge_function_config import (
    ConfigError, load_analysis_map, load_game_config, load_manual_config, sha256_file,
)
from report_function_coverage import Span, executable_words, load_map, merged_ranges, contains


JR_RA = 0x03E00008


def is_stack_prologue(word: int) -> bool:
    return (word >> 16) in (0x27BD, 0x67BD) and (word & 0x8000) != 0


def is_stack_restore(word: int) -> bool:
    return (word >> 16) in (0x27BD, 0x67BD) and 0 < (word & 0xFFFF) < 0x8000


def is_terminal_jump(word: int) -> bool:
    opcode = word >> 26
    return word == JR_RA or opcode == 2 or (opcode == 0 and (word & 0x3F) == 8)


def noncall_direct_target(address: int, word: int) -> int | None:
    opcode = word >> 26
    if opcode == 2:
        return ((address + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
    if opcode in (1, 4, 5, 6, 7, 20, 21, 22, 23) or (
        opcode in (16, 17, 18) and ((word >> 21) & 0x1F) == 8
    ):
        displacement = struct.unpack("<h", struct.pack("<H", word & 0xFFFF))[0] << 2
        return (address + 4 + displacement) & 0xFFFFFFFF
    return None


def reachable_addresses(words: dict[int, int], start: int, end: int) -> set[int]:
    """Conservatively follow EE control flow inside a proposed function range."""
    pending = [start]
    reached: set[int] = set()
    while pending:
        address = pending.pop()
        if address in reached or address < start or address >= end or address not in words:
            continue
        reached.add(address)
        word = words[address]
        opcode = word >> 26
        # Every control transfer executes its delay slot.
        if opcode in (1, 2, 3, 4, 5, 6, 7, 20, 21, 22, 23) or (
            opcode in (16, 17, 18) and ((word >> 21) & 0x1F) == 8
        ) or (opcode == 0 and (word & 0x3F) in (8, 9)):
            delay_slot = address + 4
            if start <= delay_slot < end and delay_slot in words:
                reached.add(delay_slot)
        if opcode == 2:  # J: no local fallthrough after the delay slot.
            target = noncall_direct_target(address, word)
            if target is not None:
                pending.append(target)
        elif opcode == 3:  # JAL resumes after its delay slot.
            pending.append(address + 8)
        elif opcode in (1, 4, 5, 6, 7, 20, 21, 22, 23) or (
            opcode in (16, 17, 18) and ((word >> 21) & 0x1F) == 8
        ):
            target = noncall_direct_target(address, word)
            if target is not None:
                pending.append(target)
            pending.append(address + 8)
        elif opcode == 0 and (word & 0x3F) == 8:  # JR terminates this path.
            pass
        elif opcode == 0 and (word & 0x3F) == 9:  # JALR resumes after call.
            pending.append(address + 8)
        else:
            pending.append(address + 4)
    return reached


def direct_targets(words: dict[int, int], owned: list[tuple[int, int]]) -> dict[int, list[int]]:
    result: dict[int, list[int]] = {}
    for address, word in words.items():
        if not contains(owned, address):
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
        if target is not None:
            result.setdefault(target, []).append(address)
    return result


def audit(
    analysis: list[Span], effective: list[Span], words: dict[int, int], max_scan: int,
    require_inbound: bool = True, reject_zero_runs: bool = True,
) -> list[dict[str, object]]:
    effective_by_start = {span.start: span for span in effective}
    # Manual missing-function discoveries are independent boundaries too; an
    # analyzed function must never be extended across one merely because the
    # analyzer omitted that neighboring start.
    starts = sorted({span.start for span in analysis} | {span.start for span in effective})
    inbound = direct_targets(words, merged_ranges(effective))
    candidates: list[dict[str, object]] = []

    for span in analysis:
        replacement = effective_by_start.get(span.start)
        if replacement is not None and replacement.end > span.end:
            continue
        sources = inbound.get(span.start, [])
        if (require_inbound and not sources) or not is_stack_prologue(words.get(span.start, 0)):
            continue
        if span.end - span.start >= 8 and is_terminal_jump(words.get(span.end - 8, 0)):
            continue

        next_index = bisect_right(starts, span.start)
        next_start = starts[next_index] if next_index < len(starts) else span.end + max_scan
        scan_end = min(next_start, span.end + max_scan)
        return_address = None
        tailcall_address = None
        zero_run = 0
        rejected = False
        for address in range(span.end, scan_end, 4):
            word = words.get(address)
            if word is None:
                rejected = True
                break
            if word == 0:
                zero_run += 1
                if reject_zero_runs and zero_run >= 4:
                    rejected = True
                    break
            else:
                zero_run = 0
            if is_stack_prologue(word):
                rejected = True
                break
            if word == JR_RA:
                if address + 4 not in words:
                    rejected = True
                else:
                    return_address = address
                break
            if (word >> 26) == 2:
                target = noncall_direct_target(address, word)
                delay = words.get(address + 4, 0)
                if target is not None and not (span.start <= target < scan_end) and is_stack_restore(delay):
                    tailcall_address = address
                    break
        if rejected or (return_address is None and tailcall_address is None):
            continue
        terminator = return_address if return_address is not None else tailcall_address
        assert terminator is not None
        proposed_end = terminator + 8
        if terminator not in reachable_addresses(words, span.start, proposed_end):
            continue
        # The first linear return is not a safe bound if another path branches
        # past it, or if the omitted suffix contains an indirect non-return
        # jump. Calls (JAL/JALR) are allowed because they resume in this frame.
        for address in range(span.start, proposed_end, 4):
            word = words[address]
            target = noncall_direct_target(address, word)
            if target is not None and proposed_end <= target < next_start:
                rejected = True
                break
            if (word >> 26) == 0 and (word & 0x3F) == 8 and word != JR_RA:
                rejected = True
                break
        if rejected:
            continue
        candidates.append({
            "name": span.name,
            "start": f"0x{span.start:08X}",
            "analysis_end": f"0x{span.end:08X}",
            "proposed_end": f"0x{proposed_end:08X}",
            "added_bytes": proposed_end - span.end,
            "termination": "return" if return_address is not None else "tailcall",
            "terminator": f"0x{terminator:08X}",
            "direct_sources": [f"0x{source:08X}" for source in sources[:16]],
        })
    return sorted(candidates, key=lambda item: (-len(item["direct_sources"]), item["start"]))


def add_native_analyzer_crosscheck(
    candidates: list[dict[str, object]], generated_dir: Path
) -> None:
    address_pattern = re.compile(r"// 0x([0-9a-fA-F]+):")
    by_start: dict[int, Path] = {}
    for path in generated_dir.glob("*.cpp"):
        marker = path.stem.rsplit("_0x", 1)
        if len(marker) != 2:
            continue
        try:
            by_start[int(marker[1], 16)] = path
        except ValueError:
            continue
    for candidate in candidates:
        start = int(str(candidate["start"]), 0)
        path = by_start.get(start)
        native_end = None
        if path is not None:
            try:
                addresses = [
                    int(match.group(1), 16)
                    for match in address_pattern.finditer(path.read_text(encoding="utf-8"))
                ]
            except OSError:
                addresses = []
            if addresses:
                native_end = max(addresses) + 4
        candidate["native_analyzer_end"] = (
            f"0x{native_end:08X}" if native_end is not None else None
        )
        candidate["native_covers_proposed"] = (
            native_end is not None and native_end >= int(str(candidate["proposed_end"]), 0)
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--analysis-map", required=True, type=Path)
    parser.add_argument("--effective-map", required=True, type=Path)
    parser.add_argument("--game", default=Path("config/game.toml"), type=Path)
    parser.add_argument("--manual", default=Path("config/functions.manual.toml"), type=Path)
    parser.add_argument("--max-scan", type=lambda value: int(value, 0), default=0x800)
    parser.add_argument("--native-generated-dir", type=Path,
                        help="optional ignored native-analyzer C++ output for boundary cross-checking")
    parser.add_argument(
        "--all-prologues", action="store_true",
        help="audit every analyzed stack-frame function, including indirect-only functions",
    )
    parser.add_argument(
        "--allow-zero-runs", action="store_true",
        help="permit reachable NOP runs used by polling/retry loops during suffix scanning",
    )
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args(argv)
    try:
        game, _ = load_game_config(args.game)
        actual = sha256_file(args.elf)
        if actual != game["elf_sha256"]:
            raise ConfigError(
                f"ELF SHA-256 mismatch: expected {game['elf_sha256']}, found {actual}"
            )
        words = dict(executable_words(args.elf))
        analysis = [
            Span(entry.start, entry.end, entry.name)
            for entry in load_analysis_map(args.analysis_map)
        ]
        candidates = audit(
            analysis, load_map(args.effective_map), words, args.max_scan,
            require_inbound=not args.all_prologues,
            reject_zero_runs=not args.allow_zero_runs,
        )
        if args.native_generated_dir:
            add_native_analyzer_crosscheck(candidates, args.native_generated_dir)
        effective = load_map(args.effective_map)
        inbound_targets = set(direct_targets(words, merged_ranges(effective)))
        pointer_targets = {
            value for value in words.values()
            if value % 4 == 0 and value in words
        }
        invalid_manual_sets = []
        for entry in load_manual_config(args.manual, game["id"], args.elf):
            if entry.source not in {"manual_set", "manual_missing_set"}:
                continue
            terminator = entry.end - 8
            for candidate in range(entry.end - 8, max(entry.start - 1, entry.end - 0x40), -4):
                trailing = range(candidate + 8, entry.end, 4)
                if all(words.get(address, 0) == 0 for address in trailing) and is_terminal_jump(
                    words.get(candidate, 0)
                ):
                    terminator = candidate
                    break
            terminator_word = words.get(terminator, 0)
            valid_return = terminator_word == JR_RA
            valid_tailcall = (terminator_word >> 26) == 2
            valid_indirect_jump = (
                (terminator_word >> 26) == 0
                and (terminator_word & 0x3F) == 8
            )
            evidence_starts = {entry.start} | {
                target for target in inbound_targets | pointer_targets
                if entry.start <= target < entry.end
            }
            reachable = set().union(*(
                reachable_addresses(words, start, entry.end)
                for start in evidence_starts
            ))
            if not (valid_return or valid_tailcall or valid_indirect_jump) or terminator not in reachable:
                invalid_manual_sets.append({
                    "name": entry.name,
                    "start": f"0x{entry.start:08X}",
                    "end": f"0x{entry.end:08X}",
                    "reason": "terminal jump is not reachable from any referenced entry in the range",
                })
    except (ConfigError, OSError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    report = {
        "candidate_count": len(candidates),
        "candidates": candidates,
        "invalid_manual_set_count": len(invalid_manual_sets),
        "invalid_manual_sets": invalid_manual_sets,
    }
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
