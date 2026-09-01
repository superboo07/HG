# Haunting Ground Static Recompilation Roadmap

## Definition of success

The U.S. NTSC release boots from a user-supplied dump, reaches menus and
gameplay, can be completed with correct saves, audio, input, timing, and visual
output, and builds reproducibly on Tier 1 desktop targets. No Sony BIOS, game
image/assets, PCSX2 component, or generated copyrighted game code is shipped.

Tier 1 targets are Windows x86-64, Linux x86-64, and macOS arm64. Windows arm64
and Linux arm64 are Tier 2 until the runtime's SIMD paths and CI are proven.
Other operating systems are enabled through portable platform interfaces rather
than promised before a toolchain and tester exist.

## Phase 0 — Governance and reproducible baseline

Deliverables:

- Establish `AGENTS.md`, this roadmap, a status ledger, and repository ignores.
- Record exact ELF/disc hashes, serial, region, game version, PCSX2 oracle
  version, and later its deterministic settings.
- Write an oracle observation template, an open-source provenance ledger, and an
  asset/legal boundary document.
- Select and pin a known PS2Recomp revision; record its GPL-3.0 obligations.

Exit criteria:

- A fresh agent can locate the inputs, rules, current state, and immediate next
  action without relying on chat history.
- `git status` cannot accidentally stage local game, BIOS, emulator, save, trace,
  screenshot, or generated recompilation artifacts.

## Phase 1 — Portable project skeleton and toolchain

Deliverables:

- Add a CMake C++20 superproject with presets for Windows, Linux, and macOS.
- Integrate a pinned PS2Recomp revision under `third_party/` without modifying
  its source for game-specific hacks.
- Add native runner targets, tests, formatting/linting, dependency policy, and
  CI build matrix.
- Define platform interfaces for window/input, time, filesystem, audio, and
  OpenGL context creation. Use a small cross-platform library where justified,
  but keep OpenGL calls in our renderer.

Exit criteria:

- Empty runner and unit tests configure, compile, and run on all Tier 1 CI hosts.
- Dependency revisions and build prerequisites are documented and reproducible.

## Phase 2 — ELF analysis and manual function configuration

Status: complete (2026-08-27). The reproducible proof and exact artifact counts
are recorded in `docs/PROJECT_STATUS.md`.

Deliverables:

- Analyze `SLUS_210.75` with Ghidra and PS2Recomp's analyzer as complementary
  offline tools; keep the Ghidra project and raw/generated analysis local.
- Produce a coverage report for executable ranges, discovered functions, gaps,
  indirect targets, overlays, jump tables, and suspicious instructions.
- Create `config/game.toml` for immutable build identity and paths supplied via
  local overrides/environment, never hard-coded user machine paths.
- Create `config/functions.manual.toml` for functions the analyzer missed.
- Implement `tools/merge_function_config` to validate and merge the generated
  function map with manual entries into `build/generated/functions.effective.csv`
  and PS2Recomp's effective TOML.
- Define a stable standalone export/import format between analysis and
  recompilation. The recompiler receives the verified ELF, effective TOML, and
  effective function map only; it must have no Ghidra API, installation, project,
  database, script-runner, or headless-analysis dependency.

Planned manual entry schema (to validate against the pinned upstream format):

```toml
schema_version = 1

[[builds]]
id = "SLUS-21075-v1.01"
elf_sha256 = "3b374d53a499d2c17b205274ee9eb34280768f294f970ebf6ae6731f6a2dacb8"

[[functions]]
build = "SLUS-21075-v1.01"
name = "manual_00123450"
start = 0x00123450
end = 0x00123510
mode = "recompile" # recompile | runtime_bind | skip_for_triage
handler = ""        # required only for runtime_bind
confidence = "high"
evidence = "call target and verified return boundary"
reason = "not emitted by analyzer"
```

Validation rules:

- Bounds are half-open `[start, end)`, four-byte aligned, executable, ordered,
  non-overlapping, and tied to the exact ELF SHA-256.
- Starts/names are unique after merging; conflicts fail with actionable errors.
- `skip_for_triage` requires a tracked issue/reason and is forbidden in release
  builds. Runtime handlers must exist and match the expected calling contract.
- The merge is deterministic and emits a provenance report showing whether each
  boundary was analyzer-, Ghidra-, or manually supplied.

Exit criteria:

- [x] Every executable byte is classified as code, data/padding, or an investigated
  gap; all reachable entry points have a recompilation or intentional binding.
- [x] Merge tests cover malformed TOML, hash mismatch, overlaps, duplicate entries,
  invalid handlers, and stable output ordering.
- [x] From a clean checkout containing approved standalone metadata, recompilation
  succeeds on a host where Ghidra is not installed and no Ghidra project exists.

## Phase 3 — First native execution

Status: complete (2026-08-27). Windows x86-64 cold-boot proof, exact artifact
hashes, regression results, and the Linux/macOS host-verification carry-over are
recorded in `docs/PROJECT_STATUS.md`.

Deliverables:

- Generate recompiled C++ locally and link it with the PS2Recomp runtime and the
  project runner.
- Implement ELF/data loading from the user's dump and verify guest memory layout,
  entry point, register state, syscalls, timing source, and deterministic logs.
- Triage hard blockers one at a time: missing functions, syscalls, IOP RPC, DMA,
  and unsupported EE/MMI/VU0 behavior.

Milestones and exit criteria:

1. [x] Entry executes and exits through a controlled diagnostic path.
2. [x] Boot advances deterministically through early initialization.
3. [x] The title/menu loop runs without temporary return stubs on critical paths.

## Phase 4 — IOP, storage, input, audio, and movies

Status: active. The current v163 Windows checkpoint and evidence are in
`docs/PROJECT_STATUS.md`. Progress is tracked by the exit checks below, not by
an invented percentage.

Deliverables:

- Add game-scoped IOP HLE profiles for required modules/RPC services rather than
  embedding behavior in generic global stubs.
- Implement asynchronous disc/file reads against the user-supplied data source,
  controller mapping, memory cards, timers/threads/semaphores, audio streaming,
  and CRI movie/audio services needed by this build.
- Define portable save locations and endian-stable save tests.

Exit criteria:

- [x] The real title/menu loop accepts deterministic controller input without a
  generated callback patch or permanent injected buttons.
- [x] New Game completes loading and reaches stable gameplay through the
  ordinary asynchronous storage and scheduler paths.
- [x] The real opening CRI Sofdec movie decodes and presents without damaged,
  duplicated, or out-of-order video data.
- [x] Movie and ordinary streamed audio are audible and remain synchronized
  with video/game timing.
- [ ] Saves are created in the portable save location with endian-stable test
  coverage and reload in a separate-process cold boot.
- [ ] The storage, input, movie/audio, and save checkpoints agree with the
  build-scoped PCSX2 oracle within documented tolerances.
- [ ] All currently available Tier 1 hosts pass, or unavailable/platform-specific
  verification is recorded explicitly in the status ledger.

Replay note: Start followed by Triangle is the verified cutscene-skip input.
Opening-movie progression and A/V validation must omit both buttons while the
movie is active; the skip sequence may be tested separately. If Start is needed
at the title prompt, schedule it only after that prompt is demonstrated and do
not follow it with Triangle.

## Phase 5 — OpenGL GS renderer

Active sequencing note (2026-08-31): the user made practical PS2-rate gameplay
the immediate Phase 4 priority after profiling demonstrated that the current
software GS is the dominant frame-time bottleneck. The OpenGL renderer work
below is therefore being pulled forward before the remaining Phase 4 save and
oracle gates. This dependency inversion does not mark Phase 4 complete; its
remaining exit criteria stay open and resume after the renderer reaches a
validated usable-speed checkpoint. The active architecture and differential
gates are recorded in `docs/OPENGL_GS_DESIGN.md`.

Windows implementation checkpoint (2026-08-31): gate 1 is complete on the
available Windows/NVIDIA host. The runner requests an OpenGL 4.3 context behind
`PS2X_ENABLE_OPENGL_43_GS`, and an opt-in GLSL compute/SSBO round-trip passes
exactly while `PS2GS` remains 87/87. Linux capability and Apple fallback still
require their own host validation. No gameplay primitive uses the GPU yet, so
this checkpoint is infrastructure rather than renderer completion.

Deliverables:

- Capture GS register/transfer observations at small deterministic scenes using
  the oracle protocol. Consult relevant PCSX2 source where it accelerates an
  accurate implementation, recording exact source/revision provenance and any
  deliberately reused code or algorithms.
- Implement local VRAM formats, transfers, swizzling/addressing, draw state,
  texture sampling, palettes, depth, alpha test/blending, fog, scissor, and
  display circuits needed by observed game workloads.
- Translate observed GS draws to OpenGL with explicit state caching and shader
  variants. Start with a broadly available core profile; add optional newer
  paths only behind capability checks.
- Add headless frame capture and perceptual/exact image comparisons as suitable.

Exit criteria:

- Title, menus, representative rooms, characters, effects, FMVs, and UI match
  oracle captures within documented tolerances with no validation errors.

## Phase 6 — VIF/VU1, DMA, and scene correctness

Deliverables:

- Inventory VIF streams and VU1 microprograms used by the game.
- Implement/recompile their required behavior and GIF packet production; verify
  DMA ordering, interrupts, stalls, and frame pacing.
- Add focused golden tests for animation/skinning, camera, lighting, particles,
  and collision/game-state code whose errors are visually subtle.

Exit criteria:

- Representative gameplay scenes are stable and agree with oracle checkpoints
  for selected memory/register/GS outputs across long deterministic runs.

## Phase 7 — Accuracy campaign and full playthrough

Deliverables:

- Build a checkpoint suite spanning boot, every major area/boss, inventory,
  puzzles, dog AI, panic/fear behavior, endings, death/retry, saves, and FMVs.
- Run differential traces that compare only documented guest-visible state.
- Fix timing, floating-point, undefined-behavior, uninitialized-memory, and
  threading differences; test 30/60 Hz display situations and frame pacing.

Exit criteria:

- At least one clean new-game-to-ending playthrough succeeds per Tier 1 host.
- All checkpoint tests pass, no critical function uses a triage stub, save data
  remains compatible, and known deviations are documented with severity.

## Phase 8 — Portability, performance, and release packaging

Deliverables:

- Exercise x86-64 and arm64 paths, replace unsupported unconditional intrinsics,
  and run sanitizers plus optimized/reproducible builds.
- Add renderer capability fallbacks, controller rebinding, audio-device recovery,
  window/fullscreen/scaling options, and accessible diagnostics.
- Package code/runtime only. On first run, validate and import a user's legally
  obtained matching game dump; fail clearly for wrong region/version/hash.
- Produce licenses, source offer/compliance material, build guide, compatibility
  report, and known-issues list.

Exit criteria:

- Release artifacts build from a clean checkout on Tier 1 hosts, contain no
  proprietary inputs/generated game code, and pass smoke tests and license audit.

## Oracle experiment protocol

Each comparison record should contain:

- experiment ID, purpose, date, operator, build hashes, and PCSX2 version;
- cold boot or save-state provenance, emulator settings, input script, duration;
- guest addresses/registers/GS events sampled and why they are relevant;
- expected values or tolerances, actual native values, result, and follow-up;
- artifact hashes and local-only paths (screenshots belong only in
  `UNEEDED images/` when they cannot stay in OS temporary storage).

Oracle observations and open-source references are complementary evidence.
Prefer narrow hypothesis-driven experiments over broad dumps, and document the
provenance of implementation decisions derived from PCSX2 or PS2Recomp source.
