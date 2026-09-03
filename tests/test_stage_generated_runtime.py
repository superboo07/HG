import tempfile
import unittest
from pathlib import Path
from tools.stage_generated_runtime import (
    GENERATED_HEADERS,
    StageError,
    install_hg_iop_profile,
    is_protected_staged_path,
    prune_versioned_generated_directories,
    remove_stale_runner_headers,
    sync_pinned_sources,
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


class PinnedSourceSyncTests(unittest.TestCase):
    """Incremental staging must still deliver authored pinned-source fixes."""

    def make_pair(self, root: Path) -> tuple[Path, Path]:
        pinned = root / "pinned"
        staged = root / "staged"
        (pinned / "ps2xRuntime" / "src" / "lib").mkdir(parents=True)
        (staged / "ps2xRuntime" / "src" / "lib").mkdir(parents=True)
        (pinned / ".git").mkdir()
        (pinned / ".git" / "config").write_text("gitdir\n", encoding="utf-8")
        return pinned, staged

    def test_authored_cmake_change_reaches_the_staged_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            pinned, staged = self.make_pair(Path(directory))
            (pinned / "CMakeLists.txt").write_text("add_compile_options(-msse4.1)\n", encoding="utf-8")
            (staged / "CMakeLists.txt").write_text("stale\n", encoding="utf-8")

            synced, removed = sync_pinned_sources(pinned, staged)

            self.assertEqual(
                (staged / "CMakeLists.txt").read_text(encoding="utf-8"),
                "add_compile_options(-msse4.1)\n",
            )
            self.assertEqual(synced, 1)
            self.assertEqual(removed, 0)

    def test_generated_runner_and_tool_insertions_are_never_touched(self):
        with tempfile.TemporaryDirectory() as directory:
            pinned, staged = self.make_pair(Path(directory))
            (pinned / "CMakeLists.txt").write_text("pinned\n", encoding="utf-8")

            runner = staged / "ps2xRuntime" / "src" / "runner"
            runner.mkdir(parents=True)
            (runner / "register_functions.cpp").write_text("generated\n", encoding="utf-8")
            include = staged / "ps2xRuntime" / "include"
            include.mkdir(parents=True)
            for header in GENERATED_HEADERS:
                (include / header).write_text("generated header\n", encoding="utf-8")
            profile = staged / "ps2xIOP" / "src"
            profile.mkdir(parents=True)
            (profile / "haunting_ground_iop_profile.cpp").write_text("profile\n", encoding="utf-8")

            sync_pinned_sources(pinned, staged)

            self.assertTrue((runner / "register_functions.cpp").is_file())
            for header in GENERATED_HEADERS:
                self.assertTrue((include / header).is_file())
            self.assertTrue((profile / "haunting_ground_iop_profile.cpp").is_file())

    def test_removes_staged_files_dropped_upstream(self):
        with tempfile.TemporaryDirectory() as directory:
            pinned, staged = self.make_pair(Path(directory))
            (pinned / "CMakeLists.txt").write_text("pinned\n", encoding="utf-8")
            stale = staged / "ps2xRuntime" / "src" / "lib" / "removed_upstream.cpp"
            stale.write_text("stale\n", encoding="utf-8")

            _, removed = sync_pinned_sources(pinned, staged)

            self.assertFalse(stale.exists())
            self.assertEqual(removed, 1)

    def test_runtime_artifacts_beside_the_tree_are_not_deletion_candidates(self):
        with tempfile.TemporaryDirectory() as directory:
            pinned, staged = self.make_pair(Path(directory))
            (pinned / "CMakeLists.txt").write_text("pinned\n", encoding="utf-8")
            (staged / "imgui.ini").write_text("ui state\n", encoding="utf-8")
            memory_card = staged / "mc0"
            memory_card.mkdir()
            (memory_card / "BASLUS-21075").write_text("save\n", encoding="utf-8")

            sync_pinned_sources(pinned, staged)

            self.assertTrue((staged / "imgui.ini").is_file())
            self.assertTrue((memory_card / "BASLUS-21075").is_file())

    def test_git_metadata_is_never_staged(self):
        with tempfile.TemporaryDirectory() as directory:
            pinned, staged = self.make_pair(Path(directory))
            (pinned / "CMakeLists.txt").write_text("pinned\n", encoding="utf-8")

            sync_pinned_sources(pinned, staged)

            self.assertFalse((staged / ".git").exists())

    def test_protected_path_classification(self):
        self.assertTrue(is_protected_staged_path("ps2xRuntime/src/runner/main.cpp"))
        self.assertTrue(
            is_protected_staged_path("ps2xRuntime/include/ps2_recompiled_stubs.h")
        )
        self.assertFalse(is_protected_staged_path("ps2xRuntime/src/lib/ps2_runtime.cpp"))
        self.assertFalse(is_protected_staged_path("ps2xRuntime/src/runner_notes.md"))
