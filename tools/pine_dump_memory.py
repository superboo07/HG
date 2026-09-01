#!/usr/bin/env python3
"""Dump a contiguous guest-memory range through a PCSX2 PINE endpoint."""

from __future__ import annotations

import argparse
import socket
import struct
import time
from pathlib import Path

from pine_movie_ring_probe import read32s


def parse_u32(value: str) -> int:
    parsed = int(value, 0)
    if not 0 <= parsed <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("value must fit in 32 bits")
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("address", type=parse_u32)
    parser.add_argument("size", type=parse_u32)
    parser.add_argument("output", type=Path)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--slot", type=int, default=28012)
    parser.add_argument("--connect-timeout", type=float, default=20.0)
    parser.add_argument("--words-per-request", type=int, default=8192)
    args = parser.parse_args()

    if args.address % 4 or args.size % 4:
        parser.error("address and size must be four-byte aligned")
    if args.size == 0:
        parser.error("size must be positive")
    if not 1 <= args.words_per_request <= 10000:
        parser.error("--words-per-request must be from 1 through 10000")

    deadline = time.monotonic() + args.connect_timeout
    while True:
        try:
            connection = socket.create_connection((args.host, args.slot), timeout=1.0)
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise SystemExit("PINE endpoint did not become ready")
            time.sleep(0.1)

    data = bytearray()
    word_count = args.size // 4
    with connection:
        connection.settimeout(5.0)
        for first_word in range(0, word_count, args.words_per_request):
            count = min(args.words_per_request, word_count - first_word)
            first_address = args.address + first_word * 4
            addresses = [first_address + index * 4 for index in range(count)]
            words = read32s(connection, addresses)
            data.extend(struct.pack(f"<{count}I", *words))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(
        f"dumped {len(data)} bytes from 0x{args.address:08X} "
        f"through 0x{args.address + args.size:08X} to {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
