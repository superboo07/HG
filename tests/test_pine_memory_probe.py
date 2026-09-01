from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from pine_memory_probe import decode_read32_reply, encode_read32  # noqa: E402


class PineMemoryProbeTests(unittest.TestCase):
    def test_encodes_batched_read32_request(self) -> None:
        self.assertEqual(
            encode_read32([0x008883CC, 0x008883D0]),
            struct.pack("<I", 14)
            + struct.pack("<BI", 2, 0x008883CC)
            + struct.pack("<BI", 2, 0x008883D0),
        )

    def test_decodes_successful_read32_reply(self) -> None:
        reply = struct.pack("<IBII", 13, 0, 0x00123456, 0x89ABCDEF)
        self.assertEqual(decode_read32_reply(reply, 2), [0x00123456, 0x89ABCDEF])

    def test_rejects_failed_reply(self) -> None:
        with self.assertRaisesRegex(ValueError, "status 0xff"):
            decode_read32_reply(struct.pack("<IBI", 9, 0xFF, 0), 1)


if __name__ == "__main__":
    unittest.main()
