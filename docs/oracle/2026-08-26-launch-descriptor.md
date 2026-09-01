# Oracle Experiment: Launch Descriptor Transition

- Experiment ID: `HG-ORACLE-0002`
- Purpose/hypothesis: Determine whether the game-side launch descriptor at
  `0x008883CC` remains zero or is initialized and later retargeted during a
  cold boot.
- Date and operator: 2026-08-26, Codex under user authorization.
- ELF SHA-256: `3b374d53a499d2c17b205274ee9eb34280768f294f970ebf6ae6731f6a2dacb8`
- Disc SHA-256: `3f8c658eee1b35147672c597c0dbe8e36552172abf83571c52753eec9a50bcca`
- PCSX2 version and exact settings: PCSX2 2.6.3, isolated portable copy under
  ignored `build/pcsx2-oracle`, SCPH-39001 BIOS selected, PINE slot 28012. The
  user's existing PCSX2 process was not modified.
- Cold boot or save-state provenance: Cold boot of the verified user-owned U.S.
  disc image; no save state.
- Input sequence and duration: Launch in batch/no-GUI mode and sample five EE
  words 200 times at 100 ms intervals with `tools/pine_memory_probe.py`.
- Guest memory sampled: `0x008883CC`, `0x008883D0`, `0x008883D4`,
  `0x00487A00`, and `0x003EB0C0`.
- Expected values and tolerances: Exact 32-bit values for this build and BIOS.
- Observed values: The descriptor began as
  `{0x00000000, 0xFFFFFFFF, 0x002D1E40}` and its target changed to
  `0x002D1B20` after approximately 1.8 seconds. `0x00487A00` contained
  `0x0046F360`; the syscall table check remained `0x80014D00`.
- Native recompilation result: Native diagnostics proved the initializer copies
  the three ELF words from `0x003D8920` to `0x008883CC` bit-exactly. This
  falsified the earlier zero-descriptor hypothesis. A subsequent IRQ-stack fix
  kept the main thread live and allowed sustained GS traffic.
- Pass/fail and follow-up: Pass. Add a native checkpoint for the later
  `0x002D1B20` transition once runtime memory sampling is exposed cleanly.
- Local artifacts: The timestamped JSONL capture remains ignored at
  `build/oracle-overlay-descriptor-v1.jsonl`; no game data is committed.
- Referenced revisions: PCSX2 2.6.3 PINE protocol behavior and PS2Recomp commit
  `14b1e5cb39b4af7e6fc12f9a29fdc751efde49d7`.
