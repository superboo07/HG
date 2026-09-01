#!/usr/bin/env python3
"""Inject bounded active-high PS2 button pulses through PCSX2 PINE memory writes."""

from __future__ import annotations

import argparse
import json
import socket
import struct
import time


READ16 = 1
WRITE16 = 5
IPC_OK = 0


def parse_u32(value: str) -> int:
    parsed = int(value, 0)
    if not 0 <= parsed <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("value must fit in 32 bits")
    return parsed


def receive_exact(connection: socket.socket, size: int) -> bytes:
    result = bytearray()
    while len(result) < size:
        block = connection.recv(size - len(result))
        if not block:
            raise ConnectionError("PINE endpoint closed the connection")
        result.extend(block)
    return bytes(result)


def write_and_read16(connection: socket.socket, address: int, value: int) -> int:
    commands = struct.pack("<BIHBI", WRITE16, address, value, READ16, address)
    connection.sendall(struct.pack("<I", len(commands) + 4) + commands)
    header = receive_exact(connection, 4)
    reply_size = struct.unpack("<I", header)[0]
    reply = header + receive_exact(connection, reply_size - 4)
    if len(reply) != 7 or reply[4] != IPC_OK:
        raise ValueError(f"unexpected PINE reply {reply.hex()}")
    return struct.unpack_from("<H", reply, 5)[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", type=parse_u32, default=0x004F1550)
    parser.add_argument("--buttons", type=parse_u32, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--slot", type=int, default=28011)
    parser.add_argument("--pulses", type=int, default=1)
    parser.add_argument("--active-seconds", type=float, default=0.08)
    parser.add_argument("--period-seconds", type=float, default=0.5)
    parser.add_argument("--write-interval", type=float, default=0.002)
    parser.add_argument("--connect-timeout", type=float, default=20.0)
    args = parser.parse_args()

    if args.buttons > 0xFFFF:
        parser.error("--buttons must fit in 16 bits")
    if args.pulses < 1:
        parser.error("--pulses must be positive")
    if not 0 < args.active_seconds <= args.period_seconds:
        parser.error("active duration must be positive and no longer than the period")
    if args.write_interval <= 0 or args.connect_timeout <= 0:
        parser.error("write interval and timeout must be positive")

    deadline = time.monotonic() + args.connect_timeout
    while True:
        try:
            connection = socket.create_connection((args.host, args.slot), timeout=1.0)
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise SystemExit(
                    f"could not connect to PINE at {args.host}:{args.slot} "
                    f"within {args.connect_timeout:g} seconds"
                )
            time.sleep(0.1)

    active_low = 0xFFFF & ~args.buttons
    start = time.monotonic()
    with connection:
        connection.settimeout(2.0)
        for pulse in range(args.pulses):
            pulse_start = time.monotonic()
            writes = 0
            observed = None
            while time.monotonic() - pulse_start < args.active_seconds:
                observed = write_and_read16(connection, args.address, active_low)
                writes += 1
                time.sleep(args.write_interval)
            release_until = pulse_start + args.period_seconds
            while time.monotonic() < release_until:
                observed = write_and_read16(connection, args.address, 0xFFFF)
                time.sleep(args.write_interval)
            print(
                json.dumps(
                    {
                        "pulse": pulse,
                        "elapsed_seconds": round(time.monotonic() - start, 6),
                        "address": f"0x{args.address:08X}",
                        "buttons": f"0x{args.buttons:04X}",
                        "active_writes": writes,
                        "observed_after_write": f"0x{observed:04X}",
                    },
                    separators=(",", ":"),
                ),
                flush=True,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
