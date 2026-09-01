#!/usr/bin/env python3
"""Build a conservative control-flow inventory for a captured VU1 program.

The input is local, user-owned runtime evidence produced by
PS2X_VU1_CAPTURE_PROGRAMS_DIR. The report contains decoded metadata only; it
does not copy the captured program bytes.
"""

from __future__ import annotations

import argparse
import json
import struct
from collections import Counter, deque
from pathlib import Path


DIRECT_BRANCHES = {
    0x20: "b",
    0x21: "bal",
    0x28: "ibeq",
    0x29: "ibne",
    0x2C: "ibltz",
    0x2D: "ibgtz",
    0x2E: "iblez",
    0x2F: "ibgez",
}
INDIRECT_BRANCHES = {0x24: "jr", 0x25: "jalr"}
UNCONDITIONAL_BRANCHES = {0x20, 0x21}
LOWER_PRIMARY_NAMES = {
    0x00: "lq", 0x01: "sq", 0x04: "ilw", 0x05: "isw",
    0x08: "iaddiu", 0x09: "isubiu", 0x10: "fceq", 0x11: "fcset",
    0x12: "fcand", 0x13: "fcor", 0x14: "fseq", 0x15: "fsset",
    0x16: "fsand", 0x17: "fsor", 0x18: "fmeq", 0x1A: "fmand",
    0x1B: "fmor", 0x1C: "fcget", **DIRECT_BRANCHES,
    **INDIRECT_BRANCHES,
}
LOWER_DIRECT_NAMES = {
    0x30: "iadd", 0x31: "isub", 0x32: "iaddi", 0x34: "iand", 0x35: "ior"
}
LOWER_SPECIAL_NAMES = {
    0x30: "move", 0x31: "mr32", 0x34: "lqi", 0x35: "sqi",
    0x36: "lqd", 0x37: "sqd", 0x38: "div", 0x39: "sqrt",
    0x3A: "rsqrt", 0x3B: "waitq", 0x3C: "mtir", 0x3D: "mfir",
    0x3E: "ilwr", 0x3F: "iswr", 0x40: "rnext", 0x41: "rget",
    0x42: "rinit", 0x43: "rxor", 0x64: "mfp", 0x68: "xtop",
    0x69: "xitop", 0x6C: "xgkick", 0x70: "esadd", 0x71: "ersadd",
    0x72: "eleng", 0x73: "erleng", 0x74: "eatanxy", 0x75: "eatanxz",
    0x76: "esum", 0x77: "ersqrt", 0x78: "esqrt", 0x79: "esin",
    0x7A: "ercpr", 0x7B: "waitp", 0x7C: "eatan", 0x7D: "eexp",
}


def parse_int(text: str) -> int:
    return int(text, 0)


def fnv1a64(data: bytes) -> int:
    value = 1469598103934665603
    for byte in data:
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def sign_extend_11(value: int) -> int:
    value &= 0x7FF
    return value - 0x800 if value & 0x400 else value


def upper_key(upper: int) -> str:
    op = upper & 0x3F
    if op < 0x3C:
        return f"op_0x{op:02x}"
    special = (upper & 0x3) | ((upper >> 4) & 0x7C)
    return f"special_0x{special:02x}"


def lower_key(lower: int, upper: int) -> str:
    if upper & 0x80000000:
        return "loi"
    if lower in (0, 0x8000033C):
        return "nop"
    op_hi = (lower >> 25) & 0x7F
    if op_hi != 0x40:
        return LOWER_PRIMARY_NAMES.get(op_hi, f"primary_0x{op_hi:02x}")
    direct = lower & 0x3F
    if direct in LOWER_DIRECT_NAMES:
        return LOWER_DIRECT_NAMES[direct]
    if direct >= 0x3C:
        special = (lower & 0x3) | ((lower >> 4) & 0x7C)
        return LOWER_SPECIAL_NAMES.get(special, f"special_0x{special:02x}")
    return f"direct_0x{direct:02x}"


def analyze(data: bytes, starts: list[int], include_pairs: bool = False) -> dict[str, object]:
    if not data or len(data) % 8:
        raise ValueError("capture size must be a nonzero multiple of eight bytes")
    size = len(data)
    if size & (size - 1):
        raise ValueError("VU1 capture size must be a power of two")
    mask = size - 1

    pairs = [struct.unpack_from("<II", data, offset) for offset in range(0, size, 8)]
    requested = [start & mask for start in starts]
    queue = deque(requested)
    reachable: set[int] = set()
    leaders = set(requested)

    while queue:
        pc = queue.popleft()
        if pc in reachable:
            continue
        reachable.add(pc)
        lower, upper = pairs[pc // 8]
        i_bit = bool(upper & 0x80000000)
        e_bit = bool(upper & 0x40000000)
        op_hi = (lower >> 25) & 0x7F if not i_bit else -1

        if e_bit:
            reachable.add((pc + 8) & mask)
            continue
        if op_hi in DIRECT_BRANCHES:
            reachable.add((pc + 8) & mask)
            target = (pc + 8 + sign_extend_11(lower) * 8) & mask
            leaders.add(target)
            queue.append(target)
            if op_hi not in UNCONDITIONAL_BRANCHES:
                fallthrough = (pc + 16) & mask
                leaders.add(fallthrough)
                queue.append(fallthrough)
            continue
        if op_hi in INDIRECT_BRANCHES:
            reachable.add((pc + 8) & mask)
            continue
        queue.append((pc + 8) & mask)

    upper_counts: Counter[str] = Counter(
        upper_key(pairs[pc // 8][1]) for pc in reachable
    )
    lower_counts: Counter[str] = Counter(
        lower_key(*pairs[pc // 8]) for pc in reachable
    )
    blocks: list[dict[str, object]] = []
    for entry in sorted(leaders & reachable):
        pc = entry
        block_pairs: list[int] = []
        exit_kind = "fallthrough"
        successors: list[int] = []
        while pc in reachable:
            lower, upper = pairs[pc // 8]
            block_pairs.append(pc)
            i_bit = bool(upper & 0x80000000)
            e_bit = bool(upper & 0x40000000)
            op_hi = (lower >> 25) & 0x7F if not i_bit else -1
            if e_bit:
                delay_pc = (pc + 8) & mask
                if delay_pc in reachable:
                    block_pairs.append(delay_pc)
                exit_kind = "end"
                break
            if op_hi in DIRECT_BRANCHES:
                delay_pc = (pc + 8) & mask
                if delay_pc in reachable:
                    block_pairs.append(delay_pc)
                target = (pc + 8 + sign_extend_11(lower) * 8) & mask
                successors.append(target)
                if op_hi not in UNCONDITIONAL_BRANCHES:
                    successors.append((pc + 16) & mask)
                exit_kind = DIRECT_BRANCHES[op_hi]
                break
            if op_hi in INDIRECT_BRANCHES:
                delay_pc = (pc + 8) & mask
                if delay_pc in reachable:
                    block_pairs.append(delay_pc)
                exit_kind = INDIRECT_BRANCHES[op_hi]
                break
            next_pc = (pc + 8) & mask
            if next_pc in leaders:
                successors.append(next_pc)
                exit_kind = "known_entry"
                break
            pc = next_pc

        blocks.append(
            {
                "entry": f"0x{entry:04x}",
                "last_pair": f"0x{block_pairs[-1]:04x}",
                "pair_count": len(block_pairs),
                "exit": exit_kind,
                "successors": [f"0x{successor:04x}" for successor in successors],
            }
        )

    report: dict[str, object] = {
        "capture_hash": f"0x{fnv1a64(data):016x}",
        "code_size": size,
        "requested_starts": [f"0x{start & mask:04x}" for start in starts],
        "reachable_pairs": len(reachable),
        "blocks": blocks,
        "reachable_upper_classes": dict(upper_counts.most_common()),
        "reachable_lower_classes": dict(lower_counts.most_common()),
    }
    if include_pairs:
        report["pairs"] = [
            {
                "pc": f"0x{pc:04x}",
                "lower": f"0x{pairs[pc // 8][0]:08x}",
                "upper": f"0x{pairs[pc // 8][1]:08x}",
                "lower_class": lower_key(*pairs[pc // 8]),
                "upper_class": upper_key(pairs[pc // 8][1]),
                "dest": (pairs[pc // 8][1] >> 21) & 0xF,
                "ft": (pairs[pc // 8][1] >> 16) & 0x1F,
                "fs": (pairs[pc // 8][1] >> 11) & 0x1F,
                "fd": (pairs[pc // 8][1] >> 6) & 0x1F,
                "vi_t": (pairs[pc // 8][0] >> 16) & 0xF,
                "vi_s": (pairs[pc // 8][0] >> 11) & 0xF,
                "vi_d": (pairs[pc // 8][0] >> 6) & 0xF,
                "imm11": sign_extend_11(pairs[pc // 8][0]),
                "flags": "".join(
                    name
                    for bit, name in (
                        (0x80000000, "I"), (0x40000000, "E"),
                        (0x20000000, "M"), (0x10000000, "D"),
                        (0x08000000, "T"),
                    )
                    if pairs[pc // 8][1] & bit
                ),
            }
            for pc in sorted(reachable)
        ]
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--start", action="append", type=parse_int, required=True)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--include-pairs", action="store_true")
    args = parser.parse_args()

    report = analyze(args.capture.read_bytes(), args.start, args.include_pairs)
    rendered = json.dumps(report, indent=2) + "\n"
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
