# Oracle Experiment: Early Syscall Table Discovery

- Experiment ID: `HG-ORACLE-0001`
- Purpose/hypothesis: Verify the guest-visible kernel syscall table address and
  handlers installed by the game's early `InitTLBFunctions`/
  `InitSystemCallTableAddress` path.
- Date and operator: 2026-08-26, Codex under user authorization.
- ELF SHA-256: `3b374d53a499d2c17b205274ee9eb34280768f294f970ebf6ae6731f6a2dacb8`
- Disc SHA-256: `3f8c658eee1b35147672c597c0dbe8e36552172abf83571c52753eec9a50bcca`
- PCSX2 version and exact settings: PCSX2 2.6.3, isolated portable copy under
  ignored `build/pcsx2-oracle`, SCPH-39001 BIOS selected, PINE slot 28012. The
  user's already-running PCSX2 instance on slot 28011 was not modified.
- Cold boot or save-state provenance: Cold boot of the verified user-owned U.S.
  disc image; no save state.
- Input sequence and duration: Launch in batch/no-GUI mode, wait for early game
  initialization, then read the listed words through PINE and stop only the
  isolated process.
- Guest registers/memory/GS events sampled: EE words at `0x003EB0C0`,
  `0x003EB0C8`, `0x003EB0CC`, `0x80014E68`, and `0x80014F0C`.
- Expected values and tolerances: Exact 32-bit values for this ELF/BIOS pair.
- Observed values: syscall-table base `0x80014D00`; slot `0x5A` at
  `0x80014E68` contained `0x00275AD8`; slot `0x83` at `0x80014F0C` contained
  `0x00276380`; globals `0x003EB0C8 = 0x00000083` and
  `0x003EB0CC = 0x00276380`.
- Native recompilation result: The observations identified omitted syscall
  helper functions and allowed the static runtime to advance beyond its prior
  syscall-0x83 loop without copying emulator implementation.
- Pass/fail and follow-up: Pass. Keep the addresses scoped to the recorded build
  and BIOS; add a differential automated checkpoint after deterministic PINE
  capture support exists.
- Local artifact paths and hashes: PCSX2 copy and transient logs remain ignored
  under `build/`; no BIOS, disc data, dump, or generated game code is committed.
- Referenced PCSX2/PS2Recomp revision and source paths, if any: PCSX2 `PINE.cpp`
  and `Memory.cpp` were consulted for protocol/address-alias behavior;
  PS2Recomp is pinned at `14b1e5cb39b4af7e6fc12f9a29fdc751efde49d7`.
