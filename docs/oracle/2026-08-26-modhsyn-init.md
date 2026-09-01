# Oracle Experiment: MODHSYN Initialization Response

- Experiment ID: `HG-ORACLE-0004`
- Purpose/hypothesis: Determine the guest-visible response used by the early
  `sid=0x77777777` MODHSYN command stream without importing emulator code.
- Date and operator: 2026-08-26, Codex under user authorization.
- ELF SHA-256: `3b374d53a499d2c17b205274ee9eb34280768f294f970ebf6ae6731f6a2dacb8`
- Disc SHA-256: `3f8c658eee1b35147672c597c0dbe8e36552172abf83571c52753eec9a50bcca`
- PCSX2 version and exact settings: PCSX2 2.6.3, isolated portable copy under
  ignored `build/pcsx2-oracle`, SCPH-39001 BIOS, PINE slot 28012. The user's
  existing PCSX2 process was not modified.
- Cold boot or save-state provenance: Cold boot of the verified user-owned U.S.
  disc image; no save state.
- Input and observation: The EE wrapper uses the fixed 32-byte command block at
  `0x01970D40` and four-byte response at `0x01973EC0`. Both were sampled 1,500
  times at 10 ms intervals while initialization transitioned into streaming.
- Observed result: The response word remained exactly zero. The command block
  transitioned from the volume/setup state
  `{0,0,0xFF,0x00FFFFFF,0,0,1,0}` to streaming setup states whose stable words
  were `{0,0,0x000E3C00,0x0017AAC0,0x4000,1,1,0}`; intermediate fourth words
  `0x0010EAC0`, `0x0014AAC0`, and `0x00156AC0` were also observed.
- Native implementation/result: The exact Haunting Ground profile claims this
  SID, clears the four-byte response to the observed zero status, and forwards
  the original SID/function/send/receive buffers through the portable audio
  backend seam. This is explicitly boot compatibility; command-specific
  MODHSYN synthesis and timing remain open accuracy work.
- Pass/fail and follow-up: The initialization response characterization passes.
  Follow up with command-specific audio comparison once visible boot is stable.
- Local artifacts: The ignored capture remains at
  `build/oracle-hg-sound-init-v1.jsonl`; no game data is committed.
- Referenced revisions: PCSX2 2.6.3 behavior and PS2Recomp commit
  `14b1e5cb39b4af7e6fc12f9a29fdc751efde49d7`.
