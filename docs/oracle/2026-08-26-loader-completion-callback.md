# Oracle Experiment: Loader Completion and VBlank Callback

> Superseded in part by `2026-08-27-loader-event-thread.md`: clean v57 narrowed
> the five-sector request block and v61 identified the final cause as the wrong
> `ChangeThreadPriority` success return. The VBlank-callback falsification below
> remains valid; the follow-up proposed by this earlier record is complete.

- Experiment ID: `HG-ORACLE-0005`
- Purpose/hypothesis: Determine whether the optional engine callback at
  `0x003B83E0` drives the loader pump while the outer request waits in state 2,
  and compare the live request transition with the native diagnostic boot.
- Date and operator: 2026-08-26, Codex under user authorization.
- ELF SHA-256: `3b374d53a499d2c17b205274ee9eb34280768f294f970ebf6ae6731f6a2dacb8`
- Disc SHA-256: `3f8c658eee1b35147672c597c0dbe8e36552172abf83571c52753eec9a50bcca`
- PCSX2 version and exact settings: PCSX2 2.6.3, isolated portable copy under
  ignored `build/pcsx2-oracle`, SCPH-39001 BIOS, PINE slot 28012. The user's
  existing PCSX2 process was not modified.
- Cold boot or save-state provenance: Cold boot of the verified user-owned U.S.
  disc image; no save state.
- Input sequence and duration: Launch PCSX2 with `-batch -nogui` and sample six
  EE words 60 times at 100 ms intervals using `tools/pine_memory_probe.py`.
- Guest memory sampled: `0x003B83E0`, `0x003B4728`, `0x003C2E50`,
  `0x003C2E54`, `0x003C2E98`, and `0x003C2E99`.
- Expected values and tolerances: Exact 32-bit values for this build and BIOS.
- Observed values: `0x003B83E0` remained zero throughout. The outer request at
  `0x003B4728` and lower object at `0x003C2E50` nevertheless advanced from
  state-2 words (`0x00000201` / `0x00010201`) to state 3 and then cleared.
  A later request repeated the same transition. This falsifies the optional
  VBlank-callback hypothesis.
- Native recompilation result: Temporary scheduler logging recorded no
  `sceGsSyncVCallback` registration and no alarm registration. The retained
  bounded function trace shows that, after the diagnostic invocation first
  advances the outer request to state 3, the game naturally follows
  `0x001692F0 -> 0x001C9800 -> 0x001D1560 -> 0x001D1720 -> 0x001CA4C8`.
  Therefore the unresolved edge is specifically the initial asynchronous
  state-2-to-state-3 completion, not periodic pump delivery in general.
- Pass/fail and follow-up: Pass. Later v57-v61 work proved the relevant wake was
  delivered and corrected the priority save/restore contract; no hard-coded
  VBlank pump was added. See `2026-08-27-loader-event-thread.md` for the final
  differential and resolution.
- Local artifacts: The native retained trace is ignored at `build/ps2_log.txt`;
  the PINE sample was reviewed directly and contains no game payload data.
- Referenced revisions: PCSX2 `2.6.3`; PS2Recomp pinned revision recorded in
  `docs/PROJECT_STATUS.md`.
