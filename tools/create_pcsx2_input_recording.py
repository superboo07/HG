#!/usr/bin/env python3
"""Create a deterministic PCSX2 v1 power-on input recording."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


BUTTONS = {
    "left": (0, 0x80, 7),
    "down": (0, 0x40, 9),
    "right": (0, 0x20, 6),
    "up": (0, 0x10, 8),
    "start": (0, 0x08, None),
    "r3": (0, 0x04, None),
    "l3": (0, 0x02, None),
    "select": (0, 0x01, None),
    "square": (1, 0x80, 13),
    "cross": (1, 0x40, 12),
    "circle": (1, 0x20, 11),
    "triangle": (1, 0x10, 10),
    "r1": (1, 0x08, 15),
    "l1": (1, 0x04, 14),
    "r2": (1, 0x02, 17),
    "l2": (1, 0x01, 16),
}


def parse_press(value: str) -> tuple[str, int, int]:
    try:
        button, frame_range = value.lower().split(":", 1)
        start_text, end_text = frame_range.split("-", 1)
        start = int(start_text, 0)
        end = int(end_text, 0)
    except (ValueError, TypeError) as error:
        raise argparse.ArgumentTypeError(
            "press must use BUTTON:START-END with inclusive frame bounds"
        ) from error
    if button not in BUTTONS:
        raise argparse.ArgumentTypeError(
            f"unknown button {button!r}; expected one of {', '.join(BUTTONS)}"
        )
    if start < 0 or end < start:
        raise argparse.ArgumentTypeError("press frame range is invalid")
    return button, start, end


def fixed_text(value: str, size: int) -> bytes:
    encoded = value.encode("utf-8")
    if len(encoded) >= size:
        raise ValueError(f"text must be shorter than {size} UTF-8 bytes")
    return encoded + bytes(size - len(encoded))


def make_pad_frame(presses: list[str]) -> bytes:
    data = bytearray((0xFF, 0xFF, 0x7F, 0x7F, 0x7F, 0x7F) + (0,) * 12)
    for button in presses:
        group, bit, pressure_offset = BUTTONS[button]
        data[group] &= ~bit
        if pressure_offset is not None:
            data[pressure_offset] = 0xFF
    return bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--frames", type=int, required=True)
    parser.add_argument("--press", action="append", default=[], type=parse_press)
    parser.add_argument("--author", default="Haunting Ground Phase 4 oracle")
    parser.add_argument("--game", default="Haunting Ground")
    parser.add_argument("--emulator", default="PCSX2-v2.6.3")
    args = parser.parse_args()

    if args.frames < 1:
        parser.error("--frames must be positive")
    for button, start, end in args.press:
        if end >= args.frames:
            parser.error(
                f"{button} press ending at frame {end} exceeds the recording"
            )

    active: list[list[str]] = [[] for _ in range(args.frames)]
    for button, start, end in args.press:
        for frame in range(start, end + 1):
            active[frame].append(button)

    header = b"".join(
        (
            bytes((1,)),
            fixed_text(args.emulator, 50),
            fixed_text(args.author, 255),
            fixed_text(args.game, 255),
            struct.pack("<II?", args.frames, 0, False),
        )
    )
    neutral_pad = make_pad_frame([])
    payload = b"".join(
        make_pad_frame(frame_presses) + neutral_pad
        for frame_presses in active
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + payload)
    print(
        f"wrote {args.frames} frames ({len(header) + len(payload)} bytes) "
        f"to {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
