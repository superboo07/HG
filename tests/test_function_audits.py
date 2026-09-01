from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from audit_data_function_pointers import short_leaf_end  # noqa: E402
from audit_missing_pointer_functions import infer  # noqa: E402
from audit_gap_leaf_functions import discover, stack_function_end  # noqa: E402
from audit_function_call_graph import build_graph, shortest_path  # noqa: E402
from report_function_coverage import Span  # noqa: E402


class FunctionAuditTests(unittest.TestCase):
    def test_short_leaf_accepts_work_before_return_and_delay_slot(self) -> None:
        words = {
            0x1000: 0x00C0102B,  # sltu v0,zero,a2
            0x1004: 0xA0820042,  # sb v0,0x42(a0)
            0x1008: 0x03E00008,  # jr ra
            0x100C: 0x0080102D,  # move v0,a0 (delay slot)
        }
        self.assertEqual(short_leaf_end(words, 0x1000), 0x1010)

    def test_short_leaf_rejects_escaping_control_flow(self) -> None:
        words = {
            0x1000: 0x08000800,  # j 0x2000
            0x1004: 0,
            0x1008: 0x03E00008,
            0x100C: 0,
        }
        self.assertIsNone(short_leaf_end(words, 0x1000))

    def test_infer_accepts_reviewed_non_prologue_leaf_seed(self) -> None:
        words = {
            0x1000: 0x00C0102B,
            0x1004: 0xA0820042,
            0x1008: 0x03E00008,
            0x100C: 0x0080102D,
        }
        result = infer(words, [0x1000], [0x1000, 0x1100], 0x100, {0x1000})
        self.assertEqual(result[0]["proposed_end"], "0x00001010")

    def test_gap_discovery_chains_adjacent_leaf_functions(self) -> None:
        words = {
            0x1008: 0x03E00008,
            0x100C: 0,
            0x1010: 0xAC800008,
            0x1014: 0x03E00008,
            0x1018: 0,
            0x101C: 0,
            0x1020: 0x27BDFFF0,
        }
        result = discover(words, [(0x1000, 0x1008), (0x1020, 0x1030)])
        self.assertEqual(
            [(item["start"], item["end"]) for item in result],
            [("0x00001008", "0x00001010"), ("0x00001010", "0x0000101C")],
        )

    def test_gap_discovery_accepts_stack_function_with_two_tail_exits(self) -> None:
        words = {
            0x1000: 0x27BDFFF0,  # addiu sp,sp,-0x10
            0x1004: 0x14800004,  # bnez a0,0x1018
            0x1008: 0xFFBF0000,
            0x100C: 0x08000800,  # j 0x2000
            0x1010: 0x27BD0010,  # stack restore in delay slot
            0x1014: 0,
            0x1018: 0x08000C00,  # j 0x3000
            0x101C: 0x27BD0010,  # stack restore in delay slot
            0x1020: 0,
            0x1024: 0x03E00008,
            0x1028: 0,
            0x1030: 0x27BDFFF0,
        }
        result = discover(words, [(0x0FF0, 0x1000), (0x1030, 0x1040)])
        self.assertEqual(result[0]["start"], "0x00001000")
        self.assertEqual(result[0]["end"], "0x00001020")
        self.assertEqual(result[0]["kind"], "stack_framed")
        self.assertEqual(
            [edge["target"] for edge in result[0]["tail_targets"]],
            ["0x00002000", "0x00003000"],
        )
        self.assertEqual(result[1]["start"], "0x00001024")

    def test_stack_function_rejects_computed_nonreturn_jump(self) -> None:
        words = {
            0x1000: 0x27BDFFF0,
            0x1004: 0x00800008,  # jr a0
            0x1008: 0x27BD0010,
        }
        self.assertIsNone(stack_function_end(words, 0x1000, 0x100C))

    def test_gap_discovery_accepts_delayed_stack_prologue(self) -> None:
        words = {
            0x1000: 0x3C03003C,
            0x1004: 0x00041600,
            0x1008: 0x24633C0C,
            0x100C: 0x00021603,
            0x1010: 0x27BDFFF0,
            0x1014: 0xFFBF0000,
            0x1018: 0xDFBF0000,
            0x101C: 0x03E00008,
            0x1020: 0x27BD0010,
            0x1024: 0,
            0x1030: 0x27BDFFF0,
        }
        result = discover(words, [(0x0FF0, 0x1000), (0x1030, 0x1040)])
        self.assertEqual(result[0]["start"], "0x00001000")
        self.assertEqual(result[0]["end"], "0x00001024")
        self.assertEqual(result[0]["kind"], "stack_framed")

    def test_call_graph_routes_gap_owner_to_mapped_target(self) -> None:
        words = {
            0x1000: 0x0C000800,  # jal 0x2000
            0x1004: 0,
            0x2000: 0x08000C00,  # j 0x3000
            0x2004: 0,
            0x3000: 0x03E00008,
            0x3004: 0,
        }
        spans = [
            Span(0x1000, 0x1008, "gap"),
            Span(0x2000, 0x2008, "middle"),
            Span(0x3000, 0x3008, "target"),
        ]
        graph, unresolved, _ = build_graph(words, spans, set(words))
        self.assertEqual(unresolved, [])
        path = shortest_path(graph, 0x1000, 0x3000)
        self.assertIsNotNone(path)
        self.assertEqual(len(path or []), 2)


if __name__ == "__main__":
    unittest.main()
