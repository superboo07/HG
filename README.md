# Haunting Ground Static Recompilation

Source-informed static recompilation of the U.S. PlayStation 2 release of
*Haunting Ground*, based on PS2Recomp. The native renderer uses OpenGL. The
project has completed its reproducible analysis pipeline and first native
execution phases. Active Phase 4 work is bringing controller input, New Game,
CRI Sofdec movies/audio, asynchronous storage, and cold-boot saves to
guest-visible accuracy.

The only supported game executable is currently `SLUS_210.75` version 1.01
with SHA-256
`3b374d53a499d2c17b205274ee9eb34280768f294f970ebf6ae6731f6a2dacb8`.
The current Windows runner reaches the real menu/New Game route. The latest
movie work passes the former MPEG wait and reaches controller read 484, but
IPU DMA FIFO pacing and clean synchronized movie playback remain incomplete.
See `docs/PROJECT_STATUS.md` for the exact current checkpoint; this README is
only the stable project overview.

Users must supply their own matching game dump. Game data, Sony BIOS files,
PCSX2 binaries, generated recompiled game code, and extracted assets are not
part of this repository.

## Windows development build

Prerequisites are Visual Studio 2022 with C++ tools, CMake 3.24+, Git, and
Python 3.11+.

```powershell
git submodule update --init --recursive
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

Build the pinned PS2Recomp analyzer and recompiler with:

```powershell
cmake --build build/windows-msvc --config Debug --target hg_ps2recomp_tools
```

See `docs/FUNCTION_CONFIG.md` for the Ghidra-independent recompilation metadata
boundary and `docs/ROADMAP.md` for the staged accuracy plan.

Generated game code remains ignored. To stage it for a local runtime build:

```powershell
python tools/stage_generated_runtime.py `
  --generated build/phase3/recompiled-v151 `
  --ps2recomp third_party/PS2Recomp `
  --runtime-source build/runtime-source-phase3 `
  --incremental `
  --retain-generated-versions 10
```

The utility validates the PS2Recomp checkout, creates a disposable source copy
below `build/`, replaces that copy's runner directory, and copies the two
generated headers expected by the runtime build. Authored reusable runtime or
recompiler corrections belong in canonical `third_party/PS2Recomp` first and
must be synchronized into the staged source; game-generated code remains only
under `build/`.

The retention option removes only exact numeric `recompiled-vN` siblings and
keeps the newest ten. Nonstandard legacy directories are not pruning targets.
Generated code, game data, oracle binaries, traces, and captures remain local
and ignored.
