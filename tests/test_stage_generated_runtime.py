import tempfile
import unittest
from pathlib import Path
from tools.stage_generated_runtime import (
    GENERATED_HEADERS,
    StageError,
    install_hg_iop_profile,
    prune_versioned_generated_directories,
    remove_stale_runner_headers,
)


class HauntingGroundIopProfileStagingTests(unittest.TestCase):
    def make_tree(self, root: Path) -> tuple[Path, Path]:
        workspace = root / "workspace"
        runtime = root / "runtime"
        (workspace / "src" / "iop").mkdir(parents=True)
        (runtime / "ps2xIOP" / "src").mkdir(parents=True)
        (workspace / "src" / "iop" / "haunting_ground_iop_profile.cpp").write_text(
            "profile source\n", encoding="utf-8"
        )
        (runtime / "ps2xIOP" / "CMakeLists.txt").write_text(
            "add_library(ps2_iop STATIC\n    src/builtin_profiles.cpp\n)\n",
            encoding="utf-8",
        )
        (runtime / "ps2xIOP" / "src" / "builtin_profiles.cpp").write_text(
            "namespace ps2x::iop::detail\n{\n"
            "    std::vector<ProfileDefinition> createBuiltinProfiles()\n"
            "    {\n"
            "        std::vector<ProfileDefinition> profiles;\n"
            "        return profiles;\n"
            "    }\n"
            "}\n",
            encoding="utf-8",
        )
        return workspace, runtime

    def test_installs_profile_and_registration_hooks(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace, runtime = self.make_tree(Path(directory))
            install_hg_iop_profile(workspace, runtime)

            copied = runtime / "ps2xIOP" / "src" / "haunting_ground_iop_profile.cpp"
            self.assertEqual(copied.read_text(encoding="utf-8"), "profile source\n")
            cmake = (runtime / "ps2xIOP" / "CMakeLists.txt").read_text(encoding="utf-8")
            self.assertIn("src/haunting_ground_iop_profile.cpp", cmake)
            builtin = (runtime / "ps2xIOP" / "src" / "builtin_profiles.cpp").read_text(
                encoding="utf-8"
            )
            self.assertIn("ProfileDefinition createHauntingGroundProfile();", builtin)
            self.assertIn("profiles.push_back(createHauntingGroundProfile());", builtin)

            install_hg_iop_profile(workspace, runtime)
            builtin_again = (
                runtime / "ps2xIOP" / "src" / "builtin_profiles.cpp"
            ).read_text(encoding="utf-8")
            self.assertEqual(builtin_again.count("ProfileDefinition createHauntingGroundProfile();"), 1)
            self.assertEqual(
                builtin_again.count("profiles.push_back(createHauntingGroundProfile());"), 1
            )

    def test_rejects_changed_upstream_anchor(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace, runtime = self.make_tree(Path(directory))
            (runtime / "ps2xIOP" / "CMakeLists.txt").write_text(
                "add_library(ps2_iop STATIC)\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(StageError, "source-list anchor changed"):
                install_hg_iop_profile(workspace, runtime)

    def test_removes_only_stale_runner_local_generated_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Path(directory) / "runner"
            runner.mkdir()
            for header in GENERATED_HEADERS:
                (runner / header).write_text("stale\n", encoding="utf-8")
            preserved = runner / "runner_support.h"
            preserved.write_text("keep\n", encoding="utf-8")

            remove_stale_runner_headers(runner)

            for header in GENERATED_HEADERS:
                self.assertFalse((runner / header).exists())
            self.assertEqual(preserved.read_text(encoding="utf-8"), "keep\n")

    def test_prunes_only_old_exact_versioned_generated_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            phase = build / "phase3"
            phase.mkdir(parents=True)
            for version in range(1, 13):
                generated = phase / f"recompiled-v{version}"
                generated.mkdir()
                (generated / "generated.cpp").write_bytes(b"data")
            preserved = phase / "recompiled-current"
            preserved.mkdir()
            (preserved / "authored.txt").write_text("keep\n", encoding="utf-8")
            log = phase / "recompile-v1.log"
            log.write_text("keep\n", encoding="utf-8")

            removed, removed_bytes = prune_versioned_generated_directories(
                phase / "recompiled-v12", keep=10, workspace_build=build
            )

            self.assertEqual(removed, 2)
            self.assertEqual(removed_bytes, 8)
            self.assertFalse((phase / "recompiled-v1").exists())
            self.assertFalse((phase / "recompiled-v2").exists())
            self.assertTrue((phase / "recompiled-v3").is_dir())
            self.assertTrue((phase / "recompiled-v12").is_dir())
            self.assertTrue((preserved / "authored.txt").is_file())
            self.assertTrue(log.is_file())

    def test_prune_rejects_non_latest_or_non_versioned_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            phase = build / "phase3"
            (phase / "recompiled-v1").mkdir(parents=True)
            (phase / "recompiled-v2").mkdir()
            (phase / "recompiled-current").mkdir()

            with self.assertRaisesRegex(StageError, "non-latest"):
                prune_versioned_generated_directories(
                    phase / "recompiled-v1", keep=1, workspace_build=build
                )
            with self.assertRaisesRegex(StageError, "exact recompiled-vN"):
                prune_versioned_generated_directories(
                    phase / "recompiled-current", keep=1, workspace_build=build
                )


if __name__ == "__main__":
    unittest.main()
