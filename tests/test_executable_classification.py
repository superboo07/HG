from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from classify_executable_bytes import partition, require_cleared_audits  # noqa: E402
from merge_function_config import ConfigError  # noqa: E402


class ExecutableClassificationTests(unittest.TestCase):
    def test_partition_classifies_every_byte(self) -> None:
        image = {address: 0 for address in range(0x1000, 0x1040)}
        image.update({0x1020: 1, 0x1030: 2})
        entries = partition(
            [(0x1000, 0x1040)], [(0x1008, 0x1010)], image, 0x1030,
        )
        self.assertEqual(sum(int(entry["size"]) for entry in entries), 0x40)
        self.assertEqual(
            [entry["classification"] for entry in entries],
            ["padding", "code", "investigated_gap", "data"],
        )

    def test_audit_gate_rejects_unresolved_target(self) -> None:
        zero = {"candidate_count": 0}
        coverage = {
            "overlap_count": 0,
            "uncovered_direct_target_count": 1,
            "uncovered_direct_call_target_count": 0,
        }
        with self.assertRaises(ConfigError):
            require_cleared_audits(
                coverage, zero, {"candidate_count": 0, "invalid_manual_set_count": 0}, zero,
            )


if __name__ == "__main__":
    unittest.main()
