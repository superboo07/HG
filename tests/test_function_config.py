from __future__ import annotations

import csv
import hashlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from merge_function_config import ConfigError, merge  # noqa: E402


class FunctionConfigTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.elf = self.root / "game.elf"
        self._write_test_elf(self.elf)
        digest = hashlib.sha256(self.elf.read_bytes()).hexdigest()
        self.game = self.root / "game.toml"
        self.game.write_text(
            "schema_version = 1\n"
            "[game]\n"
            "id = \"test-build\"\n"
            f"elf_sha256 = \"{digest}\"\n"
            "[recompiler]\n"
            "single_file_output = false\n",
            encoding="utf-8",
        )
        self.manual = self.root / "manual.toml"
        self.output_map = self.root / "out" / "functions.csv"
        self.output_config = self.root / "out" / "recomp.toml"

    def tearDown(self) -> None:
        self.temp.cleanup()

    @staticmethod
    def _write_test_elf(path: Path) -> None:
        data = bytearray(0x300)
        data[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
        struct.pack_into(
            "<HHIIIIIHHHHHH", data, 16,
            2, 8, 1, 0x1000, 52, 0, 0, 52, 32, 1, 0, 0, 0,
        )
        struct.pack_into("<IIIIIIII", data, 52, 1, 0x100, 0x1000, 0x1000,
                         0x200, 0x200, 0x5, 0x1000)
        path.write_bytes(data)

    def _write_manual(self, functions: str = "") -> None:
        self.manual.write_text(
            "schema_version = 1\n"
            "build = \"test-build\"\n" + functions,
            encoding="utf-8",
        )

    def _write_test_table(self, values: list[int]) -> None:
        data = bytearray(self.elf.read_bytes())
        struct.pack_into(f"<{len(values)}I", data, 0x180, *values)
        self.elf.write_bytes(data)
        digest = hashlib.sha256(data).hexdigest()
        lines = self.game.read_text(encoding="utf-8").splitlines()
        lines = [
            f'elf_sha256 = "{digest}"' if line.startswith("elf_sha256 =") else line
            for line in lines
        ]
        self.game.write_text("\n".join(lines) + "\n", encoding="utf-8")

    def _merge(self, analysis: Path | None = None, release: bool = False,
               base_config: Path | None = None):
        return merge(
            self.elf, self.game, self.manual, analysis,
            self.output_map, self.output_config, self.root / "generated", release,
            base_config,
        )

    def test_merges_manual_function_and_runtime_binding(self) -> None:
        analysis = self.root / "analysis.csv"
        analysis.write_text("Name,Start,End,Size\n", encoding="utf-8")
        self._write_manual(
            "[[functions]]\n"
            "name = \"manual_00001000\"\n"
            "start = 0x1000\nend = 0x1040\n"
            "mode = \"runtime_bind\"\nhandler = \"ret0\"\n"
            "confidence = \"high\"\nevidence = \"unit test\"\n"
            "reason = \"missing entry\"\n"
        )
        self.assertEqual(self._merge(analysis), (0, 1))
        with self.output_map.open(encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(rows[0]["Start"], "0x00001000")
        self.assertIn('"ret0@0x00001000",', self.output_config.read_text())

    def test_emits_boundary_provenance_report(self) -> None:
        analysis = self.root / "analysis.csv"
        analysis.write_text(
            "Name,Start,End,Size\nFUN_00001000,0x1000,0x1040,64\n",
            encoding="utf-8",
        )
        self._write_manual(
            "[[functions]]\n"
            "name = \"manual_00001040\"\n"
            "start = 0x1040\nend = 0x1080\n"
            "mode = \"recompile\"\nconfidence = \"high\"\n"
            "evidence = \"unit-test evidence\"\nreason = \"missing entry\"\n"
        )
        report = self.root / "out" / "provenance.csv"
        merge(
            self.elf, self.game, self.manual, analysis,
            self.output_map, self.output_config, self.root / "generated",
            provenance_report_path=report, analysis_source="ghidra",
        )
        with report.open(encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(rows[0]["Source"], "ghidra")
        self.assertEqual(rows[1]["Source"], "manual")
        self.assertEqual(rows[1]["Evidence"], "unit-test evidence")

    def test_rejects_hash_mismatch(self) -> None:
        self._write_manual()
        self.game.write_text(self.game.read_text().replace("elf_sha256 = \"", "elf_sha256 = \"00"))
        with self.assertRaisesRegex(ConfigError, "elf_sha256"):
            self._merge()

    def test_rejects_malformed_toml(self) -> None:
        self.manual.write_text("schema_version = [\n", encoding="utf-8")
        with self.assertRaisesRegex(ConfigError, "TOML"):
            self._merge()

    def test_rejects_duplicate_manual_start(self) -> None:
        template = (
            "[[functions]]\nname = \"{name}\"\nstart = 0x1000\nend = {end}\n"
            "mode = \"recompile\"\nconfidence = \"high\"\n"
            "evidence = \"unit test\"\nreason = \"unit test\"\n"
        )
        self._write_manual(
            template.format(name="one", end="0x1040")
            + template.format(name="two", end="0x1080")
        )
        analysis = self.root / "analysis.csv"
        analysis.write_text("Name,Start,End,Size\n", encoding="utf-8")
        with self.assertRaisesRegex(ConfigError, "duplicate start"):
            self._merge(analysis)

    def test_rejects_invalid_runtime_handler(self) -> None:
        self._write_manual(
            "[[functions]]\nname = \"bad_binding\"\nstart = 0x1000\nend = 0x1040\n"
            "mode = \"runtime_bind\"\nhandler = \"\"\nconfidence = \"high\"\n"
            "evidence = \"unit test\"\nreason = \"unit test\"\n"
        )
        analysis = self.root / "analysis.csv"
        analysis.write_text("Name,Start,End,Size\n", encoding="utf-8")
        with self.assertRaisesRegex(ConfigError, "handler"):
            self._merge(analysis)

    def test_rejects_unaligned_function(self) -> None:
        self._write_manual(
            "[[functions]]\nname = \"bad\"\nstart = 0x1002\nend = 0x1040\n"
            "mode = \"recompile\"\nconfidence = \"low\"\n"
            "evidence = \"unit test\"\nreason = \"unit test\"\n"
        )
        analysis = self.root / "analysis.csv"
        analysis.write_text("Name,Start,End,Size\n", encoding="utf-8")
        with self.assertRaisesRegex(ConfigError, "aligned"):
            self._merge(analysis)

    def test_rejects_manual_overlap(self) -> None:
        template = (
            "[[functions]]\nname = \"{name}\"\nstart = {start}\nend = {end}\n"
            "mode = \"recompile\"\nconfidence = \"medium\"\n"
            "evidence = \"unit test\"\nreason = \"unit test\"\n"
        )
        self._write_manual(
            template.format(name="one", start="0x1000", end="0x1080")
            + template.format(name="two", start="0x1040", end="0x10C0")
        )
        analysis = self.root / "analysis.csv"
        analysis.write_text("Name,Start,End,Size\n", encoding="utf-8")
        with self.assertRaisesRegex(ConfigError, "overlap"):
            self._merge(analysis)

    def test_release_rejects_triage_skip(self) -> None:
        self._write_manual(
            "[[functions]]\nname = \"triage\"\nstart = 0x1000\nend = 0x1040\n"
            "mode = \"skip_for_triage\"\nconfidence = \"low\"\n"
            "evidence = \"unit test\"\nreason = \"ISSUE-1\"\n"
        )
        analysis = self.root / "analysis.csv"
        analysis.write_text("Name,Start,End,Size\n", encoding="utf-8")
        with self.assertRaisesRegex(ConfigError, "forbidden"):
            self._merge(analysis, release=True)

    def test_output_is_deterministic(self) -> None:
        self._write_manual(
            "[[functions]]\nname = \"later\"\nstart = 0x1080\nend = 0x10C0\n"
            "mode = \"recompile\"\nconfidence = \"high\"\n"
            "evidence = \"unit test\"\nreason = \"unit test\"\n"
            "[[functions]]\nname = \"earlier\"\nstart = 0x1000\nend = 0x1040\n"
            "mode = \"recompile\"\nconfidence = \"high\"\n"
            "evidence = \"unit test\"\nreason = \"unit test\"\n"
        )
        analysis = self.root / "analysis.csv"
        analysis.write_text("Name,Start,End,Size\n", encoding="utf-8")
        self._merge(analysis)
        first = self.output_map.read_bytes()
        self._merge(analysis)
        self.assertEqual(first, self.output_map.read_bytes())
        self.assertLess(first.index(b"earlier"), first.index(b"later"))

    def test_manual_entries_require_complete_analysis_map(self) -> None:
        self._write_manual(
            "[[functions]]\nname = \"manual\"\nstart = 0x1000\nend = 0x1040\n"
            "mode = \"recompile\"\nconfidence = \"high\"\n"
            "evidence = \"unit test\"\nreason = \"unit test\"\n"
        )
        with self.assertRaisesRegex(ConfigError, "complete standalone"):
            self._merge()

    def test_preserves_analyzer_hardware_metadata(self) -> None:
        self._write_manual()
        base = self.root / "base.toml"
        base.write_text(
            "[general]\nstubs = [\"sceTest@0x00001000\"]\n"
            "untracked_stubs = [\"unknown@0x00001040\"]\nskip = []\n"
            "[mmio]\n\"0x1000\" = \"0x10000000\"\n"
            "[jump_tables]\n[[jump_tables.table]]\naddress = \"0x2000\"\n"
            "entries = [{ index = 0, target = \"0x1000\" }]\n"
            "[patches]\ninstructions = [{ address = \"0x1000\", value = \"0x0\" }]\n",
            encoding="utf-8",
        )
        self._merge(base_config=base)
        generated = self.output_config.read_text(encoding="utf-8")
        self.assertIn("sceTest@0x00001000", generated)
        self.assertIn("[[jump_tables.table]]", generated)
        self.assertIn('[mmio]\n"0x1000" = "0x10000000"', generated)
        self.assertIn('{ address = "0x1000", value = "0x0" }', generated)

    def test_excludes_reviewed_incorrect_base_stub(self) -> None:
        self._write_manual()
        base = self.root / "base.toml"
        base.write_text(
            "[general]\nstubs = [\"InitThread@0x00001000\", \"sceTest@0x00001040\"]\n",
            encoding="utf-8",
        )
        self.game.write_text(
            self.game.read_text(encoding="utf-8")
            + 'exclude_base_stubs = ["InitThread@0x00001000"]\n',
            encoding="utf-8",
        )
        self._merge(base_config=base)
        generated = self.output_config.read_text(encoding="utf-8")
        self.assertNotIn("InitThread@0x00001000", generated)
        self.assertIn("sceTest@0x00001040", generated)

    def test_excludes_reviewed_incorrect_base_patch(self) -> None:
        self._write_manual()
        base = self.root / "base.toml"
        base.write_text(
            "[patches]\ninstructions = ["
            "{ address = \"0x1000\", value = \"0x0\" }, "
            "{ address = \"0x1040\", value = \"0x0\" }]\n",
            encoding="utf-8",
        )
        self.game.write_text(
            self.game.read_text(encoding="utf-8")
            + "exclude_base_patches = [0x1000]\n",
            encoding="utf-8",
        )
        self._merge(base_config=base)
        generated = self.output_config.read_text(encoding="utf-8")
        self.assertNotIn('{ address = "0x1000", value = "0x0" }', generated)
        self.assertIn('{ address = "0x1040", value = "0x0" }', generated)

    def test_excludes_known_analyzer_function_names(self) -> None:
        self._write_manual()
        self.game.write_text(
            self.game.read_text(encoding="utf-8")
            + 'exclude_analyzer_functions = ["sub_003A1860"]\n',
            encoding="utf-8",
        )
        self._merge()
        generated = self.output_config.read_text(encoding="utf-8")
        self.assertIn('skip = [\n  "sub_003A1860",\n]', generated)

    def test_accepts_discontiguous_ghidra_body_size(self) -> None:
        self._write_manual()
        analysis = self.root / "analysis.csv"
        analysis.write_text(
            "Name,Start,End,Size\nfragmented,0x1000,0x1080,64\n",
            encoding="utf-8",
        )
        self.assertEqual(self._merge(analysis), (1, 0))
        self.assertIn("fragmented,0x00001000,0x00001080,128",
                      self.output_map.read_text(encoding="utf-8"))

    def test_normalizes_one_byte_ghidra_body_to_instruction_width(self) -> None:
        self._write_manual()
        analysis = self.root / "analysis.csv"
        analysis.write_text(
            "Name,Start,End,Size\nundefined,0x1000,0x1001,1\n",
            encoding="utf-8",
        )
        self._merge(analysis)
        self.assertIn("undefined,0x00001000,0x00001004,4",
                      self.output_map.read_text(encoding="utf-8"))

    def test_disambiguates_duplicate_analysis_names(self) -> None:
        self._write_manual()
        analysis = self.root / "analysis.csv"
        analysis.write_text(
            "Name,Start,End,Size\n"
            "thunk_target,0x1000,0x1008,8\n"
            "thunk_target,0x1010,0x1018,8\n",
            encoding="utf-8",
        )
        self._merge(analysis)
        generated = self.output_map.read_text(encoding="utf-8")
        self.assertIn("thunk_target,0x00001000", generated)
        self.assertIn("thunk_target__at_00001010,0x00001010", generated)

    def test_renames_auto_analysis_name_to_prioritize_map_boundary(self) -> None:
        self._write_manual()
        analysis = self.root / "analysis.csv"
        analysis.write_text(
            "Name,Start,End,Size\nFUN_00001000,0x1000,0x1010,16\n",
            encoding="utf-8",
        )
        self._merge(analysis)
        generated = self.output_map.read_text(encoding="utf-8")
        self.assertIn("ghidra_00001000,0x00001000,0x00001010,16", generated)
        self.assertNotIn("FUN_00001000", generated)

    def test_clamps_discontiguous_body_at_next_function_start(self) -> None:
        self._write_manual()
        analysis = self.root / "analysis.csv"
        analysis.write_text(
            "Name,Start,End,Size\n"
            "FUN_00001000,0x1000,0x1100,32\n"
            "FUN_00001040,0x1040,0x1080,64\n",
            encoding="utf-8",
        )
        self._merge(analysis)
        generated = self.output_map.read_text(encoding="utf-8")
        self.assertIn("ghidra_00001000,0x00001000,0x00001040,64", generated)
        self.assertIn("ghidra_00001040,0x00001040,0x00001080,64", generated)

    def test_manual_entry_can_replace_analysis_boundary_at_same_start(self) -> None:
        analysis = self.root / "analysis.csv"
        analysis.write_text(
            "Name,Start,End,Size\n"
            "bad_boundary,0x1000,0x1040,64\n"
            "next_function,0x1080,0x10C0,64\n",
            encoding="utf-8",
        )
        self._write_manual(
            "[[functions]]\n"
            "name = \"reviewed_boundary\"\nstart = 0x1000\nend = 0x1080\n"
            "mode = \"recompile\"\noverride_analysis = true\nconfidence = \"high\"\n"
            "evidence = \"unit test\"\nreason = \"analyzer stopped at a local call\"\n"
        )
        self.assertEqual(self._merge(analysis), (1, 1))
        generated = self.output_map.read_text(encoding="utf-8")
        self.assertNotIn("bad_boundary", generated)
        self.assertIn("reviewed_boundary,0x00001000,0x00001080,128", generated)

    def test_manual_override_requires_matching_analysis_start(self) -> None:
        analysis = self.root / "analysis.csv"
        analysis.write_text("Name,Start,End,Size\n", encoding="utf-8")
        self._write_manual(
            "[[functions]]\n"
            "name = \"reviewed_boundary\"\nstart = 0x1000\nend = 0x1040\n"
            "mode = \"recompile\"\noverride_analysis = true\nconfidence = \"high\"\n"
            "evidence = \"unit test\"\nreason = \"unit test\"\n"
        )
        with self.assertRaisesRegex(ConfigError, "no analysis entry"):
            self._merge(analysis)

    def test_expands_reviewed_function_pointer_table(self) -> None:
        self._write_test_table([0x1000, 0x1040, 0x1080])
        self._write_manual(
            "[[function_tables]]\n"
            "name_prefix = \"static_init\"\n"
            "table_start = 0x1080\ntable_end = 0x108C\ncode_end = 0x10C0\n"
            "confidence = \"high\"\nevidence = \"reviewed pointer table\"\n"
            "reason = \"indirect startup calls\"\n"
        )
        analysis = self.root / "analysis.csv"
        analysis.write_text("Name,Start,End,Size\n", encoding="utf-8")
        self.assertEqual(self._merge(analysis), (0, 3))
        generated = self.output_map.read_text(encoding="utf-8")
        self.assertIn("static_init_00001000,0x00001000,0x00001040,64", generated)
        self.assertIn("static_init_00001080,0x00001080,0x000010C0,64", generated)

    def test_rejects_non_monotonic_function_pointer_table(self) -> None:
        self._write_test_table([0x1040, 0x1000])
        self._write_manual(
            "[[function_tables]]\n"
            "name_prefix = \"bad\"\n"
            "table_start = 0x1080\ntable_end = 0x1088\ncode_end = 0x10C0\n"
            "confidence = \"high\"\nevidence = \"unit test\"\n"
            "reason = \"unit test\"\n"
        )
        analysis = self.root / "analysis.csv"
        analysis.write_text("Name,Start,End,Size\n", encoding="utf-8")
        with self.assertRaisesRegex(ConfigError, "strictly increasing"):
            self._merge(analysis)

    def test_expands_exact_boundary_function_set(self) -> None:
        analysis = self.root / "analysis.csv"
        analysis.write_text(
            "Name,Start,End,Size\nold_one,0x1000,0x1010,16\n"
            "old_two,0x1040,0x1050,16\n",
            encoding="utf-8",
        )
        self._write_manual(
            "[[function_sets]]\nname_prefix = \"epilogue\"\n"
            "override_analysis = true\nconfidence = \"high\"\n"
            "evidence = \"reviewed ELF return sequences\"\nreason = \"truncated epilogues\"\n"
            "ranges = [\n"
            "  { start = 0x1000, analysis_end = 0x1010, end = 0x1020 },\n"
            "  { start = 0x1040, analysis_end = 0x1050, end = 0x1060 },\n"
            "]\n"
        )
        self.assertEqual(self._merge(analysis), (0, 2))
        generated = self.output_map.read_text(encoding="utf-8")
        self.assertIn("epilogue_00001000,0x00001000,0x00001020,32", generated)

    def test_function_set_rejects_stale_analysis_end(self) -> None:
        analysis = self.root / "analysis.csv"
        analysis.write_text("Name,Start,End,Size\nold,0x1000,0x1010,16\n", encoding="utf-8")
        self._write_manual(
            "[[function_sets]]\nname_prefix = \"epilogue\"\n"
            "override_analysis = true\nconfidence = \"high\"\n"
            "evidence = \"unit test\"\nreason = \"unit test\"\n"
            "ranges = [{ start = 0x1000, analysis_end = 0x100C, end = 0x1020 }]\n"
        )
        with self.assertRaisesRegex(ConfigError, "expected analysis end"):
            self._merge(analysis)

    def test_expands_exact_missing_function_set(self) -> None:
        analysis = self.root / "analysis.csv"
        analysis.write_text("Name,Start,End,Size\n", encoding="utf-8")
        self._write_manual(
            "[[missing_function_sets]]\nname_prefix = \"vtable_leaf\"\n"
            "confidence = \"high\"\nevidence = \"reviewed pointer targets\"\n"
            "reason = \"omitted indirect leaves\"\n"
            "ranges = [\n"
            "  { start = 0x1000, end = 0x1008 },\n"
            "  { start = 0x1020, end = 0x1028 },\n"
            "]\n"
        )
        self.assertEqual(self._merge(analysis), (0, 2))
        generated = self.output_map.read_text(encoding="utf-8")
        self.assertIn("vtable_leaf_00001000,0x00001000,0x00001008,8", generated)


if __name__ == "__main__":
    unittest.main()
