#!/usr/bin/env python3
"""Cold-boot PINE probe for the opening-movie middleware and ring buffer."""

from __future__ import annotations

import argparse
import json
import socket
import struct
import time

from pine_pad_pulse import receive_exact, write_and_read16


READ32 = 2
IPC_OK = 0
PAD_ADDRESS = 0x004F1550
MENU_OBJECT = 0x009C90E4
MENU_VTABLE = 0x0046A110


def read32s(connection: socket.socket, addresses: list[int]) -> list[int]:
    commands = b"".join(struct.pack("<BI", READ32, address) for address in addresses)
    connection.sendall(struct.pack("<I", len(commands) + 4) + commands)
    header = receive_exact(connection, 4)
    reply_size = struct.unpack("<I", header)[0]
    reply = header + receive_exact(connection, reply_size - 4)
    expected = 5 + 4 * len(addresses)
    if len(reply) != expected or reply[4] != IPC_OK:
        raise ValueError(f"unexpected PINE reply {reply.hex()}")
    return list(struct.unpack_from(f"<{len(addresses)}I", reply, 5))


def pulse(
    connection: socket.socket,
    buttons: int,
    active: float = 0.08,
    observer=None,
) -> None:
    active_low = 0xFFFF & ~buttons
    deadline = time.monotonic() + active
    while time.monotonic() < deadline:
        write_and_read16(connection, PAD_ADDRESS, active_low)
        if observer is not None:
            observer()
        time.sleep(0.002)
    deadline = time.monotonic() + 0.20
    while time.monotonic() < deadline:
        write_and_read16(connection, PAD_ADDRESS, 0xFFFF)
        if observer is not None:
            observer()
        time.sleep(0.002)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--slot", type=int, default=28012)
    parser.add_argument("--connect-timeout", type=float, default=30.0)
    parser.add_argument("--menu-timeout", type=float, default=60.0)
    parser.add_argument("--transition-timeout", type=float, default=12.0)
    args = parser.parse_args()

    deadline = time.monotonic() + args.connect_timeout
    while True:
        try:
            connection = socket.create_connection((args.host, args.slot), timeout=1.0)
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise SystemExit("PINE endpoint did not become ready")
            time.sleep(0.1)

    ring_addresses = list(range(0x003BFCC0, 0x003BFD68, 4))
    source_addresses = list(range(0x003D01A8, 0x003D021C, 4))
    producer_addresses = list(range(0x003C28B0, 0x003C2970, 4))
    loader_addresses = list(range(0x003C6A20, 0x003C6C58, 4))
    file_vtable_addresses = list(range(0x003D5B80, 0x003D5BB0, 4))
    state_addresses = [0x003B72FC, 0x01081488, 0x003EA14C, 0x003EA150]
    addresses = (
        state_addresses
        + ring_addresses
        + source_addresses
        + producer_addresses
        + loader_addresses
        + file_vtable_addresses
    )

    with connection:
        connection.settimeout(2.0)
        deadline = time.monotonic() + args.menu_timeout
        while True:
            try:
                menu_object = read32s(connection, [MENU_OBJECT])[0]
            except ValueError:
                menu_object = 0
            if menu_object == MENU_VTABLE:
                break
            if time.monotonic() >= deadline:
                raise SystemExit("main menu did not become ready")
            time.sleep(0.02)

        start = time.monotonic()
        rapid_addresses = [
            0x003C6A20,
            0x003C292C,
            0x003B72FC,
            0x01081488,
            0x003EA14C,
            0x003EA150,
            0x00888404,
            0x00888408,
            0x0088840C,
            0x00888410,
            0x0196144C,
            0x01961450,
            0x01961454,
            0x009C9140,
            0x009C9150,
            0x0044E958,
            0x0044E960,
            0x0047E37C,
        ]
        last_rapid_values: list[int] | None = None

        def record_rapid_transition() -> None:
            nonlocal last_rapid_values
            values = read32s(connection, rapid_addresses)
            if values == last_rapid_values:
                return
            record = {
                "elapsed_seconds": round(time.monotonic() - start, 6),
                "sample_kind": "input_transition",
                "values": {
                    f"0x{address:08X}": f"0x{value:08X}"
                    for address, value in zip(rapid_addresses, values)
                },
            }
            print(json.dumps(record, separators=(",", ":")), flush=True)
            last_rapid_values = values

        pulse(connection, 0x0010)
        pulse(connection, 0x0010)
        record_rapid_transition()
        pulse(connection, 0x4000, observer=record_rapid_transition)

        last_values: list[int] | None = None
        deadline = start + args.transition_timeout
        while time.monotonic() < deadline:
            values = read32s(connection, addresses)
            if values != last_values:
                record = {
                    "elapsed_seconds": round(time.monotonic() - start, 6),
                    "values": {
                        f"0x{address:08X}": f"0x{value:08X}"
                        for address, value in zip(addresses, values)
                    },
                }
                print(json.dumps(record, separators=(",", ":")), flush=True)
                last_values = values
            middleware_state = (values[0] >> 8) & 0xFF
            if middleware_state >= 3:
                return 0
            time.sleep(0.005)

    raise SystemExit("middleware did not reach state 3")


if __name__ == "__main__":
    raise SystemExit(main())
