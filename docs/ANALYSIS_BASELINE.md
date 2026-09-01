# Initial Native-Analyzer Baseline (Historical)

Date: 2026-08-25  
Game build: `SLUS-21075-v1.01`  
ELF SHA-256: `3b374d53a499d2c17b205274ee9eb34280768f294f970ebf6ae6731f6a2dacb8`  
PS2Recomp revision: `14b1e5cb39b4af7e6fc12f9a29fdc751efde49d7`

This baseline used PS2Recomp's native heuristic analyzer. It is a diagnostic
comparison point, not the authoritative function map for recompilation. It is
retained as historical evidence; the current map, artifact counts, and active
blocker are recorded in `docs/PROJECT_STATUS.md`.

## Analyzer results

- 4,036 functions initially extracted.
- 571 SCE SDK symbol matches: 396 existing functions renamed and 175 added.
- ELF entry point `0x00100008`, identified as `_start`.
- 212 functions selected for known runtime stubs.
- Analyzer console summary reported 343 detected library functions lacking
  runtime handlers; the emitted TOML contains 358 `untracked_stubs` selectors.
- 37 proposed instruction patches.
- 9 detected jump tables.
- Very large quantities of “unknown MMI” diagnostics occurred while scanning.

## First recompilation trial

The standalone merger preserved the analyzer TOML hardware metadata and emitted
a Ghidra-independent effective config. PS2Recomp returned success and generated:

- 4,039 files.
- 310,658,065 bytes of C++/header output.
- 28,134 unhandled-instruction reports across four generated functions:
  `sub_003A1860`, `sub_003C4854`, `sub_00405060`, and `sub_00446890`.

The reported raw words in the last function include readable string/data bytes,
and the four functions span implausibly large address ranges. This is strong
evidence that the heuristic scanner allowed function ranges to consume data,
not evidence that all 28,134 reports represent unsupported real instructions.

## Ghidra standalone-map comparison

Ghidra 12.1.3 analyzed the same verified ELF as `MIPS:LE:64:default` and the
upstream PS2Recomp export script emitted 7,014 function records. After standalone
normalization (including discontiguous-body clamping and deterministic names),
the third comparison run generated:

- 7,017 files and 78,282,206 bytes of C++/header output.
- Zero unhandled instructions.
- 2,324 warnings, all unresolved `JR`/`JALR` control-flow cases for which the
  recompiler emitted resumable fallback entries.
- A maximum effective function span of 12,256 bytes, eliminating the four giant
  data-consuming ranges from the native-only baseline.

The complete output then compiled and linked with the pinned PS2Recomp runtime
on Windows. A cold boot found that Ghidra ended the ELF entry function immediately
before the `BGEZAL $zero` local-link idiom at `0x0010008C`. A reviewed manual
override now owns `[0x00100008, 0x00100218)`. The resulting fourth run generated
7,017 files/78,298,600 bytes, retained zero unhandled instructions, and built
successfully. It initializes OpenGL 3.3 and audio, loads the ELF, enters at
`0x00100008`, and remains alive without stderr output during a bounded 20-second
probe.

## Conclusion and next gate

Do not patch or skip the four native-analyzer giant ranges. The standalone map
removed them. The coverage, runtime-diagnostic, and early PCSX2 comparison
gates described by the original baseline have since been completed; this file
must not be used as the current task list.

Raw analyzer output, generated code, and the 31,096-line recompilation log remain
under ignored `build/` paths.
