import importlib.util
import struct
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "analyze_vu1_capture.py"
SPEC = importlib.util.spec_from_file_location("analyze_vu1_capture", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class AnalyzeVu1CaptureTests(unittest.TestCase):
    def test_direct_branch_and_end_delay_slots_form_separate_blocks(self):
        data = bytearray(0x40)
        branch = (0x20 << 25) | 3  # pc 0 -> pc 0x20
        struct.pack_into("<II", data, 0x00, branch, 0)
        struct.pack_into("<II", data, 0x20, 0, 0x40000000)

        report = MODULE.analyze(bytes(data), [0])

        self.assertEqual(report["reachable_pairs"], 4)
        self.assertEqual([block["entry"] for block in report["blocks"]],
                         ["0x0000", "0x0020"])
        self.assertEqual(report["blocks"][0]["successors"], ["0x0020"])
        self.assertEqual(report["blocks"][1]["exit"], "end")

    def test_rejects_non_pair_aligned_capture(self):
        with self.assertRaisesRegex(ValueError, "multiple of eight"):
            MODULE.analyze(b"123", [0])

    def test_rejects_non_power_of_two_capture(self):
        with self.assertRaisesRegex(ValueError, "power of two"):
            MODULE.analyze(bytes(24), [0])


if __name__ == "__main__":
    unittest.main()
