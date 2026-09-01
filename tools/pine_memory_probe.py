#!/usr/bin/env python3
"""Read explicit 32-bit guest addresses from a PCSX2 PINE endpoint."""

from __future__ import annotations

import argparse
import json
import socket
import struct
import time


READ32 = 2
IPC_OK = 0


def encode_read32(addresses: list[int]) -> bytes:
    commands = b"".join(struct.pack("<BI", READ32, address) for address in addresses)
    return struct.pack("<I", len(commands) + 4) + commands


def decode_read32_reply(reply: bytes, count: int) -> list[int]:
    expected_size = 5 + count * 4
    if len(reply) != expected_size:
        raise ValueError(f"expected {expected_size} reply bytes, received {len(reply)}")
    announced_size, status = struct.unpack_from("<IB", reply)
    if announced_size != expected_size:
        raise ValueError(
            f"reply announced {announced_size} bytes, expected {expected_size}"
        )
    if status != IPC_OK:
        raise ValueError(f"PINE request failed with status 0x{status:02x}")
    return list(struct.unpack_from(f"<{count}I", reply, 5))


def receive_exact(connection: socket.socket, size: int) -> bytes:
    result = bytearray()
    while len(result) < size:
        block = connection.recv(size - len(result))
        if not block:
            raise ConnectionError("PINE endpoint closed the connection")
        result.extend(block)
    return bytes(result)


def read32(connection: socket.socket, addresses: list[int]) -> list[int]:
    connection.sendall(encode_read32(addresses))
    header = receive_exact(connection, 4)
    reply_size = struct.unpack("<I", header)[0]
    return decode_read32_reply(header + receive_exact(connection, reply_size - 4), len(addresses))


def parse_address(value: str) -> int:
    address = int(value, 0)
    if not 0 <= address <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("address must fit in 32 bits")
    return address


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("addresses", nargs="+", type=parse_address)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--slot", type=int, default=28011)
    parser.add_argument("--samples", type=int, default=1)
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument("--connect-timeout", type=float, default=20.0)
    args = parser.parse_args()

    if args.samples < 1:
        parser.error("--samples must be positive")
    if args.interval < 0 or args.connect_timeout <= 0:
        parser.error("interval must be non-negative and timeout must be positive")

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

    start = time.monotonic()
    with connection:
        connection.settimeout(max(2.0, args.interval + 1.0))
        for sample in range(args.samples):
            values = read32(connection, args.addresses)
            print(
                json.dumps(
                    {
                        "sample": sample,
                        "elapsed_seconds": round(time.monotonic() - start, 6),
                        "words": {
                            f"0x{address:08X}": f"0x{value:08X}"
                            for address, value in zip(args.addresses, values, strict=True)
                        },
                    },
                    separators=(",", ":"),
                ),
                flush=True,
            )
            if sample + 1 < args.samples:
                time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
