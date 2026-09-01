#!/usr/bin/env python3
"""Infer conservative bounds for uncovered function-pointer targets."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from bisect import bisect_right
from pathlib import Path

from audit_function_boundaries import (
    JR_RA, add_native_analyzer_crosscheck, is_stack_prologue, is_stack_restore,
    noncall_direct_target, reachable_addresses,
)
from audit_data_function_pointers import native_starts
from merge_function_config import ConfigError, load_analysis_map, load_game_config, sha256_file
from report_function_coverage import executable_words, load_map


def infer(
    words: dict[int, int], seeds: list[int], known_starts: list[int], max_scan: int,
    allowed_non_prologue: set[int] | None = None,
) -> list[dict[str, object]]:
    allowed_non_prologue = allowed_non_prologue or set()
    results = []
    for start in seeds:
        if not is_stack_prologue(words.get(start, 0)) and start not in allowed_non_prologue:
            continue
        index = bisect_right(known_starts, start)
        next_start = known_starts[index] if index < len(known_starts) else start + max_scan
        scan_end = min(next_start, start + max_scan)
        terminator = None
        termination = None
        zero_run = 0
        rejected = False
        for address in range(start + 4, scan_end, 4):
            word = words.get(address)
            if word is None:
                rejected = True
                break
            if word == 0:
                zero_run += 1
                if zero_run >= 4:
                    rejected = True
                    break
            else:
                zero_run = 0
            if is_stack_prologue(word):
                rejected = True
                break
            if word == JR_RA:
                terminator, termination = address, "return"
                break
            if (word >> 26) == 2:
                target = noncall_direct_target(address, word)
                if target is not None and not (start <= target < scan_end) and is_stack_restore(
                    words.get(address + 4, 0)
                ):
                    terminator, termination = address, "tailcall"
                    break
        if rejected or terminator is None or termination is None:
            continue
        end = terminator + 8
        if terminator not in reachable_addresses(words, start, end):
            continue
        for address in range(start, end, 4):
            word = words[address]
            target = noncall_direct_target(address, word)
            if target is not None and end <= target < next_start:
                rejected = True
                break
            if (word >> 26) == 0 and (word & 0x3F) == 8 and word != JR_RA:
                rejected = True
                break
        if rejected:
            continue
        results.append({
            "target": f"0x{start:08X}",
            "proposed_end": f"0x{end:08X}",
            "size": end - start,
            "termination": termination,
            "terminator": f"0x{terminator:08X}",
            "next_known_start": f"0x{next_start:08X}",
        })
    return results


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--map", required=True, type=Path)
    parser.add_argument("--analysis-map", required=True, type=Path)
    parser.add_argument("--pointer-report", required=True, type=Path)
    parser.add_argument("--game", default=Path("config/game.toml"), type=Path)
    parser.add_argument("--native-generated-dir", type=Path)
    parser.add_argument("--max-scan", type=lambda value: int(value, 0), default=0x4000)
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args(argv)
    try:
        game, _ = load_game_config(args.game)
        actual = sha256_file(args.elf)
        if actual != game["elf_sha256"]:
            raise ConfigError(
                f"ELF SHA-256 mismatch: expected {game['elf_sha256']}, found {actual}"
            )
        pointer_data = json.loads(args.pointer_report.read_text(encoding="utf-8"))
        seed_records = pointer_data.get("candidates")
        if seed_records is None:
            seed_records = pointer_data.get("uncovered_direct_call_targets")
        if not isinstance(seed_records, list):
            raise ConfigError("seed report has neither candidates nor uncovered_direct_call_targets")
        seeds = [int(item["target"], 0) for item in seed_records]
        allowed_non_prologue = native_starts(args.native_generated_dir) | {
            int(item["target"], 0)
            for item in seed_records
            if item.get("leaf_return") is True
        }
        known_starts = sorted(set(
            [entry.start for entry in load_analysis_map(args.analysis_map)]
            + [span.start for span in load_map(args.map)]
            + seeds
        ))
        candidates = infer(
            dict(executable_words(args.elf)), seeds, known_starts, args.max_scan,
            allowed_non_prologue,
        )
        if args.native_generated_dir:
            # The shared annotator expects the conventional start/proposed_end keys.
            for item in candidates:
                item["start"] = item["target"]
            add_native_analyzer_crosscheck(candidates, args.native_generated_dir)
            for item in candidates:
                del item["start"]
    except (ConfigError, OSError, KeyError, ValueError, json.JSONDecodeError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    report = {"candidate_count": len(candidates), "candidates": candidates}
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
