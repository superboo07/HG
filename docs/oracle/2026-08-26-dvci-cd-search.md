# Oracle Experiment: DVCI CD Search Response

- Experiment ID: `HG-ORACLE-0003`
- Purpose/hypothesis: Characterize the `sid=0x80000597`, function `0` RPC used
  for `\DATA.CVM;1` without importing an emulator implementation.
- Date and operator: 2026-08-26, Codex under user authorization.
- ELF SHA-256: `3b374d53a499d2c17b205274ee9eb34280768f294f970ebf6ae6731f6a2dacb8`
- Disc SHA-256: `3f8c658eee1b35147672c597c0dbe8e36552172abf83571c52753eec9a50bcca`
- PCSX2 version and exact settings: PCSX2 2.6.3, isolated portable copy under
  ignored `build/pcsx2-oracle`, SCPH-39001 BIOS selected, PINE slot 28012. The
  user's existing PCSX2 process was not modified.
- Cold boot or save-state provenance: Cold boot of the verified user-owned U.S.
  disc image; no save state.
- Input and observation: The EE sends a 300-byte work block at `0x0047BAC0`,
  with the path beginning at offset `0x24`, and receives a four-byte status at
  `0x0047BC00`. The work block and status were sampled 200 times at 50 ms
  intervals after the call completed.
- Observed result: Status was exactly `1`. The first 36 work-block bytes were
  `{lsn=0x00164972, size=0x5E018000, name="DATA.CVM;1", date={00,2A,25,0A,06,01,D5,07}, pad=0}`.
  The EE copies exactly those 36 bytes to its caller's output object.
- Independent disc validation: The ISO9660 root entry for `DATA.CVM;1` has LBA
  1,460,594 (`0x00164972`), byte size 1,577,156,608 (`0x5E018000`), and recording
  bytes `69 01 06 0A 25 2A 24`. The RPC's eight-byte date is the corresponding
  API representation: zero, second, minute, hour, day, month, and a
  little-endian 2005 year.
- Native implementation/result: The game-scoped profile now registers an exact
  ELF-identity CD-search service, validates the extracted host file size, and
  emits the verified 36-byte record. Focused success and wrong-size tests pass.
  The next native read uses LBN `0x00164972`, proving that the metadata is
  consumed; the runtime still needs to register the LBN-to-host-file mapping.
- Pass/fail and follow-up: Protocol characterization passes. The later runtime
  added the explicit LBN-to-extracted-file mapping seam and now resolves the
  observed DATA.CVM reads without a PCSX2 runtime dependency. Continue treating
  genuinely asynchronous ordinary read timing as Phase 4 accuracy work.
- Local artifacts: The ignored capture remains at
  `build/oracle-dvci-control-block-v1.jsonl`; no game data is committed.
- Referenced revisions: PCSX2 2.6.3 behavior and PS2Recomp commit
  `14b1e5cb39b4af7e6fc12f9a29fdc751efde49d7`.
