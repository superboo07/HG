# Haunting Ground Static Recompilation — Agent Guide

## Mission

Create a source-informed static recompilation of the U.S. PlayStation 2 release of
*Haunting Ground* using PS2Recomp as the recompiler/runtime foundation. The
result should run accurately on Windows, Linux, and macOS, with portable seams
for additional desktop operating systems. Use OpenGL for graphics output.

This is a compatibility project, not a redistribution project. A user must
supply their own legally obtained game data. Never commit or package the game
ELF, disc image, extracted assets, BIOS, memory cards, PCSX2 binaries, or other
copyrighted/proprietary inputs.

## Mandatory session startup

1. Read this file completely.
2. Read `docs/PROJECT_STATUS.md` and `docs/ROADMAP.md`.
3. Run `git status --short --branch` before editing and preserve unrelated user
   changes.
4. Work on the earliest unblocked roadmap exit criterion; record departures
   from the roadmap in the status file.
5. Treat addresses and observations as valid only for the exact build identity
   recorded in `docs/PROJECT_STATUS.md`.

## Open-source reference and oracle boundary

- PCSX2 may be used both as a memory/behavior oracle and as an open-source
  technical reference. Its source may be inspected to understand PS2 hardware,
  formats, and edge cases relevant to this static recompilation.
- This project must remain a static recompilation, not a renamed PCSX2 fork or a
  runtime dependency on PCSX2. Prefer a focused implementation of the behavior
  this game needs instead of importing an emulator subsystem wholesale.
- Do not copy source verbatim by default. If code is deliberately reused, record
  the upstream repository, exact revision, source path, authorship/notices, and
  license in the provenance ledger before committing it. Preserve all required
  notices and confirm GPL compatibility for the combined work.
- Record oracle experiments as reproducible inputs, observation points,
  expected results, PCSX2 version/settings, and game-build hash. Record facts,
  not emulator implementation.
- Game disassembly/decompilation and function maps may be used locally to
  describe the user's executable. Do not commit substantial copyrighted game
  code, decompiled source, generated recompilation output, or game assets.
- PS2Recomp may likewise be read, modified, and reused under its open-source
  license. Preserve its notices, pin the upstream revision, and keep game-specific
  behavior outside the vendored upstream tree whenever practical.

## Architecture rules

- Keep a reproducible pipeline: validate owned input -> analyze -> merge manual
  function metadata -> recompile -> build native runner -> test.
- Ghidra is permitted for offline analysis, naming, boundary discovery, and
  exporting facts. The assembly-to-C/C++ recompiler must not read, automate,
  query, or otherwise depend on a Ghidra project. Its complete inputs must be the
  verified ELF plus standalone documented config/map files. A clean build must
  work on a machine with no Ghidra installation or project.
- Generated analyzer data is never the source of truth for human corrections.
  Store reviewed corrections in `config/functions.manual.toml`; generate
  disposable effective config/maps into `build/`.
- Every manual function entry must be build-scoped and include address bounds,
  mode, provenance/evidence, confidence, and a reason. Reject overlaps, invalid
  alignment, out-of-range addresses, duplicate names/starts, and build mismatch.
- Keep game-specific bindings, IOP behavior, and patches isolated from reusable
  PS2 runtime code. Temporary return stubs must be visibly marked and tracked as
  blockers; never mistake reaching a later screen for correctness.
- Graphics must go through the project's OpenGL backend. Keep window/input,
  audio, filesystem, timing, and threading behind portable interfaces.
- Prefer portable C++20. Any SIMD/OS-specific path needs a scalar or supported
  alternate implementation and CI coverage on its target architecture.
- Preserve deterministic execution and diagnostics where possible. Gate verbose
  traces behind runtime options so normal builds remain usable.

## Repository map

- `AGENTS.md` — durable rules and project map; do not use as a running diary.
- `SKILL.md` — required handoff format. Read it fully whenever a handoff is
  requested and return the handoff inline as it specifies.
- `docs/ROADMAP.md` — staged plan, deliverables, and exit criteria.
- `docs/PROJECT_STATUS.md` — current phase, evidence, blockers, decisions, and
  next actions; this is the progress source of truth.
- `docs/ANALYSIS_BASELINE.md` — reproducible analyzer/recompiler baseline and
  evidence for current function-boundary blockers.
- `config/` — committed build identity and human-reviewed function metadata.
- `tools/` — reproducible analysis/config/oracle utilities.
- `src/` — game runner and portable platform/runtime integration.
- `tests/` — unit, differential, trace, and gameplay smoke tests.
- `third_party/PS2Recomp/` — pinned PS2Recomp Git submodule.
- `build/` — disposable build/generated output; never commit.
- `emu/` — local PCSX2, BIOS, and ISO oracle inputs; never commit or depend on
  them at runtime.
- `Haunting Ground (USA)/` — local user game dump; never commit.
- `UNEEDED images/` — the only allowed location for necessary temporary images;
  keep its contents untracked. Prefer OS temporary storage when feasible.

## Progress and handoff discipline

- During active work, revisit this guide and update `docs/PROJECT_STATUS.md` at
  meaningful checkpoints and at least every 20–30 minutes. Update `AGENTS.md`
  in the same cadence only when rules or the project map have actually changed.
- Status updates must say what was verified, what changed, the current blocker,
  and the next executable action. Do not invent completion percentages. Track
  progress by roadmap exit criteria and milestone counts.
- Before ending a work session, ensure the status file is current and commands
  needed to reproduce the latest result are captured.
- For `/handoff` or any request to continue in a new agent/session, follow the
  root `SKILL.md` exactly. The handoff must be self-contained and reference this
  guide, the status ledger, roadmap, exact paths, decisions, and known pitfalls.

## Verification standard

- Validate parsers/config mergers with malformed, boundary, overlap, and build-
  mismatch tests.
- Validate recompilation with instruction/function tests before full-game boot.
- Compare checkpoints against the PCSX2 oracle using hashes or tolerant numeric
  comparisons for registers, selected RAM, GS state, audio timing, and input.
- Run cold-boot tests; save-state-only success is insufficient.
- A milestone is complete only when its documented exit criteria pass on all
  currently supported Tier 1 hosts, or the status ledger explicitly records a
  platform-specific blocker.
