#!/usr/bin/env python3
"""Compare native movie presentation-clock checkpoints with a PINE oracle trace."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


NATIVE_STATE = re.compile(
    r"present_clock=0x([0-9a-fA-F]+) "
    r"present_scale=0x([0-9a-fA-F]+) "
    r"source_pos=0x([0-9a-fA-F]+)"
)


def integer(text: str) -> int:
    return int(text, 0)


def read_native(path: Path) -> list[tuple[int, int, int]]:
    samples: list[tuple[int, int, int]] = []
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            match = NATIVE_STATE.search(line)
            if match:
                clock, scale, source = (int(value, 16) for value in match.groups())
                if clock != 0xFFFFFFFF and scale and source:
                    samples.append((source, clock, scale))
    return samples


def read_oracle(path: Path) -> list[tuple[int, int, int]]:
    samples: list[tuple[int, int, int]] = []
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            try:
                record = json.loads(line)
                values = record["values"]
                source = int(values["mpeg_source_position"], 16)
                clock = int(values["presentation_clock"], 16)
                scale = int(values["presentation_scale"], 16)
            except (KeyError, TypeError, ValueError, json.JSONDecodeError):
                continue
            if clock != 0xFFFFFFFF and scale and source:
                samples.append((source, clock, scale))
    return samples


def nearest(samples: list[tuple[int, int, int]], target: int) -> tuple[int, int, int]:
    return min(samples, key=lambda sample: abs(sample[0] - target))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--oracle", required=True, type=Path)
    parser.add_argument("--start", type=integer, default=0xC0000)
    parser.add_argument("--end", type=integer, default=0x180000)
    parser.add_argument("--step", type=integer, default=0x20000)
    parser.add_argument("--max-source-delta", type=integer, default=0x1000)
    parser.add_argument("--max-clock-delta-ms", type=float, default=100.0)
    args = parser.parse_args()

    native = read_native(args.native)
    oracle = read_oracle(args.oracle)
    if not native or not oracle:
        print("no comparable native/oracle movie-clock samples", file=sys.stderr)
        return 2
    if args.step <= 0 or args.end < args.start:
        print("invalid checkpoint range", file=sys.stderr)
        return 2

    failures = 0
    worst_ms = 0.0
    print("target,native_source,oracle_source,native_clock,oracle_clock,delta_ms,result")
    for target in range(args.start, args.end + 1, args.step):
        native_sample = nearest(native, target)
        oracle_sample = nearest(oracle, target)
        source_delta = abs(native_sample[0] - oracle_sample[0])
        if native_sample[2] != oracle_sample[2]:
            delta_ms = float("inf")
        else:
            delta_ms = (native_sample[1] - oracle_sample[1]) * 1000.0 / native_sample[2]
        passed = source_delta <= args.max_source_delta and abs(delta_ms) <= args.max_clock_delta_ms
        failures += not passed
        worst_ms = max(worst_ms, abs(delta_ms))
        print(
            f"0x{target:X},0x{native_sample[0]:X},0x{oracle_sample[0]:X},"
            f"{native_sample[1]},{oracle_sample[1]},{delta_ms:.3f},"
            f"{'PASS' if passed else 'FAIL'}"
        )

    print(
        f"summary: checkpoints={(args.end - args.start) // args.step + 1} "
        f"failures={failures} worst_abs_delta_ms={worst_ms:.3f}"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
