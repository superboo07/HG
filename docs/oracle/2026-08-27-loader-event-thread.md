# Oracle Experiment: Loader Event-Thread Initialization and Dispatch

- Experiment ID: `HG-ORACLE-0006`
- Purpose/hypothesis: Determine why the native no-pump boot never dispatches the
  category-4 worker that owns the loader pump, and distinguish missing guest
  initialization from a scheduler/completion-delivery failure.
- Date and operator: 2026-08-27, Codex under user authorization.
- ELF SHA-256: `3b374d53a499d2c17b205274ee9eb34280768f294f970ebf6ae6731f6a2dacb8`
- Disc SHA-256: `3f8c658eee1b35147672c597c0dbe8e36552172abf83571c52753eec9a50bcca`
- PCSX2 version and exact settings: PCSX2 2.6.3, isolated portable copy under
  ignored `build/pcsx2-oracle`, SCPH-39001 BIOS, PINE slot 28012.
- Cold boot or save-state provenance: Cold boot of the verified user-owned U.S.
  disc image; no save state.
- Oracle observation: The shared engine-priority word at `0x003BFBF8` becomes
  one during the isolated PCSX2 boot. This observation is build-specific.
- Exact-wait debugger observation: At main loader wait PC `0x00169384` for the
  five-sector request, PCSX2's raw EE thread records show main thread 1 running
  at priority zero, engine threads 3-6 at priority one, and audio thread 7 at
  priority ten. At post-`iWakeupThread` PC `0x001CB6D0`, the particular handler
  instance made engine thread 4 ready at priority one while main remained the
  active priority-zero context. PCSX2's debugger reads the raw
  `currentPriority` member; it does not apply a display offset.
- Static evidence: A whole-image scan of the verified ELF finds only two static
  stores to `0x003BFBF8`, at `0x001CBE18` and `0x001CCA08`. Both follow
  `ReferThreadStatus` and copy the returned current-priority field at offset
  `0x18`. PS2SDK's `ee_thread_status_t` independently documents that layout.
- Native v55/v56 observation: After restoring the real libkernel `InitThread`
  helper and accepting its priority-zero idle thread, the active initializer
  still produced priority zero. Generated code showed that the inherited
  analyzer patch `{ address = "0x1cbe18", value = "0x0" }` had deleted the
  active delay-slot store even though the verified ELF word is a real `sw`.
- Native v57 observation: Excluding that exact false patch restores the store.
  The no-pump cold boot creates idle thread 2, four engine workers 3-6, and audio
  thread 7. The workers settle at priorities 8, 16, 18, and 25. The game then
  naturally completes the early one-sector read wave and renders more than 120
  frames before the five-sector request stalls at `0x00169384`.
- Scheduler snapshot at the remaining block: Main thread 1 is running at
  priority 1. Category-4 thread 5 is ready at `0x0026C0C8`, the return from
  syscall 50 (`SleepThread`), at priority 18. The request is state 2 with five
  sectors pending; no `sceCdRead` call has yet been issued for it.
- Wake-source trace: A targeted strict-priority v60 probe records category-4
  thread 5 becoming ready while guest interrupt handler `0x001CB690` is active.
  That handler checks for wait/wait-suspend status and calls the interrupt-safe
  `iWakeupThread` wrapper at `0x0026D068`. At the wake, main thread 1 is current
  at priority 1, the target is priority 18, and the runtime is inside interrupt
  dispatch. This proves registration and interrupt-side wake delivery occur.
- Priority-syscall ABI observation: The game's `0x001CAF60` helper calls
  `ChangeThreadPriority` and stores `v0` at `0x003BFBF0`; `0x001CAFE8` later
  reloads that word as the priority for the restore call. A PCSX2 breakpoint at
  post-call PC `0x001CAFA4` while engine thread 4 was running observed target
  thread id 4 and `v0=1`, matching its previous priority. The native syscall
  instead returned `KE_OK` (`0`) while separately discarding the scheduler's
  already-computed `oldPriority` output.
- Differential diagnostic: A staged-only scheduler experiment granted one
  bounded slice to each ready priority instead of always reselecting priority
  one. Without any external `0x001CA4C8` invocation, the category-4 worker
  submitted and completed the five-sector read and boot continued through later
  328-, 66-, 98-, and 141-sector reads. The broad fairness change was reverted.
- Native v61 differential: Returning `oldPriority` on successful normal and
  interrupt-safe priority changes makes the game's save/restore pair valid. A
  strict-priority cold boot with no external `0x001CA4C8` invocation issues the
  five-sector `sceCdRead` at tick 240, followed by the 328-, 66-, 98-, and
  141-sector reads, and reaches tick 1440 with 227 DMA and 121 GIF submissions.
- Conclusion: The false stub and false patch caused the earlier initialization
  failures, and the final apparent starvation was caused by the wrong
  `ChangeThreadPriority` success return—not a special interrupt-return dispatch
  rule. The game's restore used native's erroneous zero return, the scheduler
  rejected priority zero, and temporary priorities remained installed. The
  reusable syscall fix removes the address-specific pump dependency while
  preserving strict EE scheduling. The broad cross-priority diagnostic remains
  reverted.
- Local artifacts: `build/no-pump-v57-clean-store.combined.log`,
  `build/fair-priority-v59.combined.log`,
  `build/category4-wake-v60.combined.log`,
  `build/priority-return-v61.stdout.log`, effective v57 config/map, generated
  code, PCSX2 distribution, BIOS, and disc image are ignored and remain local.
- Open-source reference: PS2SDK `ee/kernel/include/kernel.h` documents the
  thread-status layout, and `ee/kernel/src/thread.c` documents libkernel's
  priority-zero top thread plus priority-one main-thread initialization. PCSX2
  `pcsx2/DebugTools/BiosDebugData.h` documents that the debugger exposes the raw
  EE `currentPriority` field. No upstream source was copied.

This record supersedes the earlier inference in
`docs/oracle/2026-08-26-loader-completion-callback.md` that the missing edge was
most likely solely inside the HLE `sceCdRead` path. v57 proved the request was
blocked one stage earlier, and v61 identifies and fixes the priority-restore
contract that prevented the natural request path from reaching `sceCdRead`.
