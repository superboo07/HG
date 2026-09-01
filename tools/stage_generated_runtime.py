#!/usr/bin/env python3
"""Stage ignored PS2Recomp output into the pinned runtime's expected paths."""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path


class StageError(RuntimeError):
    pass


GENERATED_HEADERS = ("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h")
HG_IOP_PROFILE = Path("src/iop/haunting_ground_iop_profile.cpp")
VERSIONED_GENERATED_DIRECTORY = re.compile(r"^recompiled-v([0-9]+)$")


def copy_if_changed(source: Path, destination: Path) -> bool:
    if destination.is_file() and source.read_bytes() == destination.read_bytes():
        return False
    shutil.copy2(source, destination)
    return True


def remove_stale_runner_headers(runner: Path) -> None:
    """Remove obsolete generated headers that shadow runtime/include copies."""
    for header in GENERATED_HEADERS:
        stale_header = runner / header
        if stale_header.is_file():
            stale_header.unlink()


def prune_versioned_generated_directories(
    generated: Path, keep: int, workspace_build: Path | None = None
) -> tuple[int, int]:
    """Remove old sibling ``recompiled-vN`` trees after a successful stage.

    The deliberately narrow checks keep authored source, maps, provenance
    reports, and trace logs outside the deletion scope.
    """
    if keep < 1:
        raise StageError("generated-version retention must be at least one")
    generated = generated.resolve()
    workspace_build = (workspace_build or (Path.cwd().resolve() / "build")).resolve()
    if workspace_build not in generated.parents:
        raise StageError(f"generated directory must be below {workspace_build}")
    current_match = VERSIONED_GENERATED_DIRECTORY.fullmatch(generated.name)
    if current_match is None:
        raise StageError(
            "generated-version retention requires an exact recompiled-vN directory name"
        )

    siblings: list[tuple[int, Path]] = []
    for sibling in generated.parent.iterdir():
        if not sibling.is_dir():
            continue
        match = VERSIONED_GENERATED_DIRECTORY.fullmatch(sibling.name)
        if match is not None:
            siblings.append((int(match.group(1)), sibling.resolve()))
    siblings.sort()
    current_version = int(current_match.group(1))
    if not siblings or siblings[-1][0] != current_version:
        raise StageError(
            f"refusing retention from non-latest generated version v{current_version}"
        )

    removed_count = 0
    removed_bytes = 0
    for _, target in siblings[:-keep]:
        if target.parent != generated.parent or VERSIONED_GENERATED_DIRECTORY.fullmatch(
            target.name
        ) is None:
            raise StageError(f"refusing unexpected generated-version target: {target}")
        removed_bytes += sum(
            path.stat().st_size for path in target.rglob("*") if path.is_file()
        )
        shutil.rmtree(target)
        removed_count += 1
    return removed_count, removed_bytes


def install_hg_iop_profile(workspace: Path, runtime_source: Path) -> None:
    profile_source = workspace / HG_IOP_PROFILE
    if not profile_source.is_file():
        raise StageError(f"missing Haunting Ground IOP profile: {profile_source}")

    iop = runtime_source / "ps2xIOP"
    profile_destination = iop / "src" / "haunting_ground_iop_profile.cpp"
    copy_if_changed(profile_source, profile_destination)

    cmake_path = iop / "CMakeLists.txt"
    cmake = cmake_path.read_text(encoding="utf-8")
    source_anchor = "    src/builtin_profiles.cpp\n"
    profile_source_line = "    src/haunting_ground_iop_profile.cpp\n"
    if profile_source_line not in cmake and source_anchor not in cmake:
        raise StageError(f"PS2Recomp IOP source-list anchor changed: {cmake_path}")
    if profile_source_line not in cmake:
        cmake = cmake.replace(source_anchor, source_anchor + profile_source_line, 1)
        cmake_path.write_text(cmake, encoding="utf-8", newline="")

    builtin_path = iop / "src" / "builtin_profiles.cpp"
    builtin = builtin_path.read_text(encoding="utf-8")
    namespace_anchor = "namespace ps2x::iop::detail\n{\n"
    declaration = "    ProfileDefinition createHauntingGroundProfile();\n\n"
    if declaration not in builtin and namespace_anchor not in builtin:
        raise StageError(f"PS2Recomp profile namespace anchor changed: {builtin_path}")
    if declaration not in builtin:
        builtin = builtin.replace(namespace_anchor, namespace_anchor + declaration, 1)
    return_anchor = "        return profiles;\n"
    registration = "        profiles.push_back(createHauntingGroundProfile());\n\n"
    if registration not in builtin and return_anchor not in builtin:
        raise StageError(f"PS2Recomp profile-list anchor changed: {builtin_path}")
    if registration not in builtin:
        builtin = builtin.replace(return_anchor, registration + return_anchor, 1)
        builtin_path.write_text(builtin, encoding="utf-8", newline="")


def stage(
    generated: Path, ps2recomp: Path, runtime_source: Path, incremental: bool = False
) -> tuple[int, int]:
    generated = generated.resolve()
    ps2recomp = ps2recomp.resolve()
    if not (ps2recomp / "ps2xRecomp" / "CMakeLists.txt").is_file():
        raise StageError(f"not a PS2Recomp checkout: {ps2recomp}")
    runtime_source = runtime_source.resolve()
    workspace_build = (Path.cwd().resolve() / "build")
    if runtime_source == workspace_build or workspace_build not in runtime_source.parents:
        raise StageError(f"runtime source copy must be below {workspace_build}")
    if runtime_source.exists() and not incremental:
        shutil.rmtree(runtime_source)
    if not runtime_source.exists():
        shutil.copytree(ps2recomp, runtime_source, ignore=shutil.ignore_patterns(".git"))
    install_hg_iop_profile(Path.cwd().resolve(), runtime_source)

    runtime = runtime_source / "ps2xRuntime"
    if not (runtime / "CMakeLists.txt").is_file():
        raise StageError(f"missing PS2Recomp runtime: {runtime}")
    if not (generated / "register_functions.cpp").is_file():
        raise StageError(f"missing generated register_functions.cpp: {generated}")
    for header in GENERATED_HEADERS:
        if not (generated / header).is_file():
            raise StageError(f"missing generated header: {generated / header}")

    runner = runtime / "src" / "runner"
    expected_runner = (runtime / "src" / "runner").resolve()
    if runner.exists() and runner.resolve() != expected_runner:
        raise StageError(f"refusing to replace unexpected runner path: {runner.resolve()}")
    if runner.exists() and not incremental:
        shutil.rmtree(runner)
    runner.mkdir(parents=True, exist_ok=True)
    remove_stale_runner_headers(runner)

    generated_sources = sorted(generated.glob("*.cpp"))
    if incremental:
        expected_names = {source.name for source in generated_sources}
        for stale in runner.glob("*.cpp"):
            if stale.name not in expected_names:
                stale.unlink()

    source_count = 0
    source_bytes = 0
    for source in generated_sources:
        copy_if_changed(source, runner / source.name)
        source_count += 1
        source_bytes += source.stat().st_size

    include = runtime / "include"
    for header in GENERATED_HEADERS:
        copy_if_changed(generated / header, include / header)
    return source_count, source_bytes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated", required=True, type=Path)
    parser.add_argument("--ps2recomp", default=Path("third_party/PS2Recomp"), type=Path)
    parser.add_argument("--runtime-source", default=Path("build/runtime-source"), type=Path,
                        help="disposable PS2Recomp source copy below the workspace build directory")
    parser.add_argument("--incremental", action="store_true",
                        help="content-sync an existing disposable runtime instead of recreating it")
    parser.add_argument(
        "--retain-generated-versions", type=int,
        help="after staging recompiled-vN, retain only the newest N sibling generated trees",
    )
    args = parser.parse_args(argv)
    try:
        count, size = stage(args.generated, args.ps2recomp, args.runtime_source, args.incremental)
        removed_count = 0
        removed_bytes = 0
        if args.retain_generated_versions is not None:
            removed_count, removed_bytes = prune_versioned_generated_directories(
                args.generated, args.retain_generated_versions
            )
    except (OSError, StageError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(f"staged {count} generated C++ files ({size} bytes) into {args.runtime_source}")
    if args.retain_generated_versions is not None:
        print(
            f"pruned {removed_count} old generated version directories "
            f"({removed_bytes} bytes); retained the newest {args.retain_generated_versions}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
