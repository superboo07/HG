#!/usr/bin/env python3
"""Emit a disposable VU1 AOT include from an analyzed user-owned capture."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


def parse_int(value: str) -> int:
    return int(value, 0)


def parse_int_list(value: str) -> list[int]:
    return [parse_int(item.strip()) for item in value.split(",") if item.strip()]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cfg", required=True, type=Path)
    parser.add_argument(
        "--block-entry",
        required=True,
        type=parse_int,
        action="append",
        help="block entry to emit; repeat to emit a multi-block dispatcher include",
    )
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--flag-free-masks", type=parse_int_list)
    parser.add_argument("--vf-free-mask", type=parse_int)
    parser.add_argument("--vi-free-mask", type=parse_int)
    parser.add_argument("--acc-free-mask", type=parse_int)
    parser.add_argument("--schedule-log", type=Path)
    args = parser.parse_args()

    report = json.loads(args.cfg.read_text(encoding="utf-8"))
    capture_hash = parse_int(report["capture_hash"])
    blocks = {parse_int(block["entry"]): block for block in report["blocks"]}
    pairs_by_pc = {parse_int(pair["pc"]): pair for pair in report["pairs"]}
    requested_blocks: list[tuple[int, dict, list[int]]] = []
    for entry in args.block_entry:
        block = blocks.get(entry)
        if block is None:
            raise SystemExit(f"block entry 0x{entry:x} is not in {args.cfg}")
        last = parse_int(block["last_pair"])
        pcs = list(range(entry, last + 8, 8))
        missing = [pc for pc in pcs if pc not in pairs_by_pc]
        if missing:
            raise SystemExit(
                "missing pairs: " + ", ".join(f"0x{pc:x}" for pc in missing)
            )
        if len(pcs) != int(block["pair_count"]):
            raise SystemExit(
                f"block 0x{entry:x} pair_count disagrees with its delay-slot-aware bounds"
            )
        requested_blocks.append((entry, block, pcs))

    schedule: dict[int, int] = {}
    if args.schedule_log is not None:
        pattern = re.compile(r"\[vu1-captured-schedule\] pc=0x([0-9a-fA-F]+).*?deltas=([^ ]+)")
        for line in args.schedule_log.read_text(encoding="utf-8", errors="replace").splitlines():
            match = pattern.search(line)
            if match is None:
                continue
            pc = int(match.group(1), 16)
            observed = []
            for item in match.group(2).rstrip(",").split(","):
                if not item:
                    continue
                delta_text, count_text = item.split(":", 1)
                if int(count_text, 0) != 0:
                    observed.append(int(delta_text, 0))
            if len(observed) != 1:
                raise SystemExit(
                    f"schedule for 0x{pc:x} is not invariant: {observed}"
                )
            if observed[0] < 1:
                raise SystemExit(f"invalid schedule delta for 0x{pc:x}: {observed[0]}")
            schedule[pc] = observed[0]
        missing_schedule = [
            pc
            for _, _, block_pcs in requested_blocks
            for pc in block_pcs
            if pc not in schedule
        ]
        if missing_schedule:
            raise SystemExit(
                "missing schedule entries: "
                + ", ".join(f"0x{pc:x}" for pc in missing_schedule)
            )

    lines = ["// Generated from a user-owned VU1 capture. Disposable build artifact."]
    for first, block, pcs in requested_blocks:
        required_cycles = sum(schedule.get(pc, 1) for pc in pcs)
        successors = {parse_int(value) for value in block.get("successors", [])}
        repeats_at_entry = first in successors
        conditions = [
            "enableCapturedHotBlock",
            f"capturedCodeHash == 0x{capture_hash:016x}ull",
            f"m_state.pc == 0x{first:x}u",
            f"budgetEnd - m_cycle >= {required_cycles}u",
        ]
        # The observed masks currently describe the repeated 0x158 loop entry.
        # Apply them only when emitting that single block; multi-block output is
        # guarded by exact hash and remains differential/opt-in until per-entry
        # state contracts are generated.
        apply_masks = len(requested_blocks) == 1
        if apply_masks and args.flag_free_masks:
            masks = " || ".join(
                f"m_flagPipelineFreeMask == 0x{mask:x}u"
                for mask in args.flag_free_masks
            )
            conditions.append(f"({masks})")
        if apply_masks and args.vf_free_mask is not None:
            conditions.append(f"m_vfWritePipelineFreeMask == 0x{args.vf_free_mask:x}u")
        if apply_masks and args.vi_free_mask is not None:
            conditions.append(f"m_viWritePipelineFreeMask == 0x{args.vi_free_mask:x}u")
        if apply_masks and args.acc_free_mask is not None:
            conditions.append(f"m_accWritePipelineFreeMask == 0x{args.acc_free_mask:x}u")

        condition_lines = ["if (" + conditions[0]]
        condition_lines.extend("    && " + condition for condition in conditions[1:])
        condition_lines[-1] += ")"
        lines.extend(
            [
                *condition_lines,
                "{",
                *( ["    do", "    {"] if repeats_at_entry else [] ),
                "#define PS2X_AOT_PAIR(PC, LOWER, UPPER, DELTA) \\",
                "    if (!executeCapturedPair((PC) / 8u, \\",
                "            std::integral_constant<uint32_t, LOWER>{}, \\",
                "            std::integral_constant<uint32_t, UPPER>{}, \\",
                "            std::integral_constant<uint32_t, DELTA>{})) \\",
                "        break",
            ]
        )
        for pc in pcs:
            pair = pairs_by_pc[pc]
            lower = parse_int(pair["lower"])
            upper = parse_int(pair["upper"])
            delta = schedule.get(pc, 0)
            lines.append(
                f"    PS2X_AOT_PAIR(0x{pc:04x}u, 0x{lower:08x}u, "
                f"0x{upper:08x}u, {delta}u);"
            )
        lines.extend(
            [
                "#undef PS2X_AOT_PAIR",
                "    executedGeneratedBlock = true;",
                *(
                    [
                        "    } while (m_state.pc == "
                        f"0x{first:x}u && budgetEnd - m_cycle >= {required_cycles}u);"
                    ]
                    if repeats_at_entry
                    else []
                ),
                "}",
                "",
            ]
        )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
