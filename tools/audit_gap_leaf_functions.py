#!/usr/bin/env python3
"""Find conservative complete functions in effective-map gaps.

The audit recognizes both short frameless leaves and stack-framed functions
whose reachable paths all end in a return or stack-restoring tail call.  It is
deliberately a triage report rather than an automatic function-map writer.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

from audit_function_boundaries import (
    JR_RA, is_stack_prologue, is_stack_restore, noncall_direct_target,
    reachable_addresses,
)
from audit_data_function_pointers import short_leaf_end
from merge_function_config import ConfigError, load_game_config, sha256_file
from report_function_coverage import (
    executable_words, load_binding_addresses, load_map, merged_ranges,
)


def stack_function_end(
    words: dict[int, int], start: int, gap_end: int,
) -> tuple[int, list[int]] | None:
    """Return a conservative CFG-derived end and its terminal instructions."""
    prologue = None
    for address in range(start, min(start + 0x20, gap_end), 4):
        word = words.get(address, 0)
        if is_stack_prologue(word):
            prologue = address
            break
        opcode = word >> 26
        if opcode in (1, 2, 3, 4, 5, 6, 7, 20, 21, 22, 23) or (
            opcode == 0 and (word & 0x3F) in (8, 9)
        ):
            break
    if prologue is None:
        return None
    reached = reachable_addresses(words, start, gap_end)
    if not reached:
        return None

    terminals: list[int] = []
    for address in sorted(reached):
        word = words.get(address)
        if word is None:
            return None
        opcode = word >> 26
        target = noncall_direct_target(address, word)
        if address != prologue and is_stack_prologue(word):
            return None
        if word == JR_RA:
            if address + 4 not in words:
                return None
            terminals.append(address)
        elif opcode == 2:
            if target is not None and not (start <= target < gap_end):
                if not is_stack_restore(words.get(address + 4, 0)):
                    return None
                terminals.append(address)
        elif target is not None and not (start <= target < gap_end):
            # A conditional path escaping the proposed gap owner is not safe to
            # promote as a complete function.
            return None
        elif opcode == 0 and (word & 0x3F) == 8:
            # A computed non-return jump cannot be bounded conservatively.
            return None

    if not terminals:
        return None

    end = max(reached) + 4
    # Every reachable path must stop before the next mapped function.  A plain
    # instruction at the final word would otherwise fall through the gap end.
    last = max(reached)
    last_word = words[last]
    last_opcode = last_word >> 26
    if end == gap_end and not (
        last_word == JR_RA
        or last_opcode == 2
        or (last_opcode == 0 and (last_word & 0x3F) == 8)
        or last - 4 in terminals
    ):
        return None
    return end, terminals


def outbound_edges(
    words: dict[int, int], start: int, end: int,
) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    calls: list[dict[str, str]] = []
    tails: list[dict[str, str]] = []
    for address in sorted(reachable_addresses(words, start, end)):
        word = words[address]
        opcode = word >> 26
        if opcode == 3:
            target = ((address + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            calls.append({"source": f"0x{address:08X}", "target": f"0x{target:08X}"})
        elif opcode == 2:
            target = noncall_direct_target(address, word)
            if target is not None and not (start <= target < end):
                tails.append({"source": f"0x{address:08X}", "target": f"0x{target:08X}"})
    return calls, tails


def discover(words: dict[int, int], ranges: list[tuple[int, int]]) -> list[dict[str, object]]:
    candidates: list[dict[str, object]] = []
    for (_, previous_end), (next_start, _) in zip(ranges, ranges[1:]):
        cursor = previous_end
        while cursor < next_start and words.get(cursor, 0) == 0:
            cursor += 4
        while cursor < next_start:
            end = short_leaf_end(words, cursor)
            kind = "frameless_leaf"
            terminals: list[int] = []
            if end is None or end > next_start:
                stack_result = stack_function_end(words, cursor, next_start)
                if stack_result is None:
                    break
                end, terminals = stack_result
                kind = "stack_framed"
            calls, tails = outbound_edges(words, cursor, end)
            candidates.append({
                "start": f"0x{cursor:08X}",
                "end": f"0x{end:08X}",
                "size": end - cursor,
                "gap_end": f"0x{next_start:08X}",
                "kind": kind,
                "terminals": [f"0x{address:08X}" for address in terminals],
                "direct_calls": calls,
                "tail_targets": tails,
            })
            cursor = end
            while cursor < next_start and words.get(cursor, 0) == 0:
                cursor += 4
    return candidates


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--map", required=True, type=Path)
    parser.add_argument("--config", type=Path,
                        help="effective config whose runtime bindings are excluded")
    parser.add_argument("--game", default=Path("config/game.toml"), type=Path)
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args(argv)
    try:
        game, _ = load_game_config(args.game)
        actual = sha256_file(args.elf)
        if actual != game["elf_sha256"]:
            raise ConfigError(
                f"ELF SHA-256 mismatch: expected {game['elf_sha256']}, found {actual}"
            )
        result = discover(
            dict(executable_words(args.elf)), merged_ranges(load_map(args.map))
        )
        bindings = load_binding_addresses(args.config) if args.config else {}
        result = [
            item for item in result
            if not any(
                int(item["start"], 0) <= address < int(item["end"], 0)
                for address in bindings
            )
        ]
    except (ConfigError, OSError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    report = {"candidate_count": len(result), "candidates": result}
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
