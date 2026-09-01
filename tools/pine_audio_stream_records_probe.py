#!/usr/bin/env python3
"""Probe active movie-audio stream records at the cathedral checkpoint."""

from __future__ import annotations

import argparse
import json
import socket
import time

from pine_movie_ring_probe import read32s


CATHEDRAL_ROOT_STATE = 0x01081488
UPDATE_FLAG = 0x003C2840
RECORD_BASE = 0x003C2850
RECORD_STRIDE = 0x60
RECORD_COUNT = 40

# Every field used by the update gates plus the word-aligned stream/ring state.
WORD_OFFSETS = tuple(range(0, RECORD_STRIDE, 4))


def connect(host: str, slot: int, timeout: float) -> socket.socket:
    deadline = time.monotonic() + timeout
    while True:
        try:
            return socket.create_connection((host, slot), timeout=1.0)
        except OSError:
            if time.monotonic() >= deadline:
                raise SystemExit("PINE endpoint did not become ready")
            time.sleep(0.1)


def bytes_from_words(words: list[int]) -> bytes:
    return b"".join(word.to_bytes(4, "little") for word in words)


def summarize_record(index: int, words: list[int]) -> dict[str, object]:
    raw = bytes_from_words(words)
    return {
        "index": index,
        "address": f"0x{RECORD_BASE + index * RECORD_STRIDE:08X}",
        "gate_bytes": {
            f"+0x{offset:02X}": f"0x{raw[offset]:02X}"
            for offset in (0, 1, 2, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A)
        },
        "words": {
            f"+0x{offset:02X}": f"0x{words[offset // 4]:08X}"
            for offset in WORD_OFFSETS
            if words[offset // 4] != 0
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--slot", type=int, default=28011)
    parser.add_argument("--connect-timeout", type=float, default=30.0)
    parser.add_argument("--trigger-timeout", type=float, default=60.0)
    parser.add_argument("--duration", type=float, default=12.0)
    parser.add_argument("--interval", type=float, default=0.005)
    args = parser.parse_args()
    if args.duration <= 0 or args.interval < 0:
        parser.error("duration must be positive and interval must be non-negative")

    state_addresses = [
        RECORD_BASE + index * RECORD_STRIDE for index in range(RECORD_COUNT)
    ]
    all_addresses = [
        RECORD_BASE + index * RECORD_STRIDE + offset
        for index in range(RECORD_COUNT)
        for offset in WORD_OFFSETS
    ]

    with connect(args.host, args.slot, args.connect_timeout) as connection:
        connection.settimeout(3.0)
        deadline = time.monotonic() + args.trigger_timeout
        while read32s(connection, [CATHEDRAL_ROOT_STATE])[0] == 0:
            if time.monotonic() >= deadline:
                raise SystemExit("cathedral movie root did not become active")
            time.sleep(0.002)

        start = time.monotonic()
        last_signature: tuple[int, ...] | None = None
        sample = 0
        while time.monotonic() - start < args.duration:
            state_words = read32s(connection, state_addresses)
            active_indices = [
                index for index, word in enumerate(state_words) if (word & 0xFF) == 1
            ]
            selected_addresses = [UPDATE_FLAG, CATHEDRAL_ROOT_STATE]
            for index in active_indices:
                base = RECORD_BASE + index * RECORD_STRIDE
                selected_addresses.extend(base + offset for offset in WORD_OFFSETS)
            values = read32s(connection, selected_addresses)
            signature = tuple(active_indices) + tuple(values)
            if signature != last_signature:
                records = []
                cursor = 2
                for index in active_indices:
                    word_count = len(WORD_OFFSETS)
                    records.append(
                        summarize_record(index, values[cursor : cursor + word_count])
                    )
                    cursor += word_count
                print(
                    json.dumps(
                        {
                            "sample": sample,
                            "elapsed_seconds": round(time.monotonic() - start, 6),
                            "update_flag": f"0x{values[0]:08X}",
                            "root_state": f"0x{values[1]:08X}",
                            "active_records": records,
                        },
                        separators=(",", ":"),
                    ),
                    flush=True,
                )
                last_signature = signature
            sample += 1
            if args.interval:
                time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
