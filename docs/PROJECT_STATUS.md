# Project Status

Last updated: 2026-09-02 (America/Chicago)

## Current phase

Phase 4 — IOP, storage, input, audio, and movies. Phase 3 completed on
2026-08-27. Phase 0 exit criteria are complete. Phase 1 now builds on Windows
and Linux x86-64; macOS arm64 remains an unverified carried platform task.

### Phase 4 first Linux x86-64 pipeline and runner build (2026-09-02)

The whole reproducible pipeline ran end to end on Linux x86-64 (CachyOS,
GCC 16.2.1, Python 3.14.7) against a user-supplied disc dump, producing a
linked `ps2EntryRunner` for the first time on a non-Windows host. The runner
has **not** been executed; reaching a linked binary is not evidence of
guest-visible correctness.

Verified build identity. The user's ISO SHA-256 is
`3f8c658eee1b35147672c597c0dbe8e36552172abf83571c52753eec9a50bcca`, matching
`config/game.toml`. Extraction to the ignored `Haunting Ground (USA)/` gives
`SLUS_210.75` with SHA-256
`3b374d53a499d2c17b205274ee9eb34280768f294f970ebf6ae6731f6a2dacb8` and
`MODULES/SNDDRV.IRX` with SHA-256
`3B8E76B6FCAA1B3FBE59848A6094BD230D5FE9643496E3AFF5F3B969730404BF`, both
matching the recorded identities. `SYSTEM.CNF` reads `VER = 1.01`,
`VMODE = NTSC`. `DATA.CVM` is 1,577,156,608 bytes (0x5E018000), the size the
game-scoped CD-search service expects.

Pipeline results: the merger validated 3,865 analysis and 7,200 manual
functions; `report_function_coverage.py` reported 11,065 functions, 70.3441%
executable-byte coverage, 0 overlaps, 0 uncovered direct targets and 0
uncovered direct call targets, exiting 0; recompilation completed successfully
into 11,068 files; staging installed 11,066 generated sources (206,331,733
bytes).

Four portability and build defects were found and fixed:

- The pinned runtime did not compile on Linux at all. `ps2_runtime_macros.h`
  calls SSE4.1 intrinsics that MSVC exposes unconditionally but GCC and Clang
  reject as "inlining failed ... target specific option mismatch" without an
  explicit ISA flag. `ps2_runtime.cpp`, `ps2_memory.cpp` and `ps2_iop_host.cpp`
  all failed. PS2Recomp now enables SSE4.1 for non-MSVC x86 after probing the
  flag; SSE4.1 is the exact baseline, with no AVX or SSE4.2 intrinsics in the
  tree.
- CI could not have caught that: the workflow built only the portable skeleton
  and never the pinned runtime. A Linux job now compiles `ps2_runtime`,
  `ps2_recomp`, `ps2_analyzer` and `ps2x_tests`. It deliberately does not run
  the suite, which needs the generated recompilation of a user-supplied ELF.
- `ctest` failed on every platform, not just Linux. `function_config_tests` ran
  `unittest discover` from the build directory, so the tests could not
  `import tools`. The test now sets `WORKING_DIRECTORY` to the source root;
  ctest passes 2/2.
- `stage_generated_runtime.py --incremental` silently never delivered authored
  pinned-source corrections, refreshing only generated runner sources and the
  two generated headers. Authored runtime and CMake fixes therefore could not
  reach a staged build, contradicting the documented staging contract. The tool
  now content-syncs pinned sources and prunes upstream-removed files, with
  generated code, the two generated headers and this tool's own IOP-profile
  insertions protected, and pruning confined to the pinned tree's top-level
  directories so `mc0`/`imgui.ini` are never deletion candidates. Six tests
  cover the behavior.

Two build-resource fixes were also required. `register_functions.cpp` is a
single generated function of about 41 MB; inside a unity batch under `-O2` it
consumed over one hour and forty minutes of a single core while producing no
hot code, and it now builds at `-O0 -g0` outside the unity batches. Generated
unity translation units were measured peaking above 3 GiB of compiler RSS,
which drove a 16 GiB host into swap under a core-count `-j`, so
`ps2EntryRunner` compile concurrency is now sized from host RAM with a
single-slot link pool. An optional ccache/sccache launcher was added, carrying
the sloppiness settings the precompiled-header path needs.

Open Linux blocker: `ps2x_tests` reports 478/488 with the pinned runtime built
from the canonical tree. Nine failures are expected in that configuration
because the Haunting Ground IOP profile was not staged. The tenth is not
explained by staging and is a genuine finding: `VBlank remains host paced while
guest code stays runnable` fails reproducibly on an idle machine across three
runs. It exercises only the portable scheduler, with no game profile involved,
and asserts a 100 ms wall-clock deadline on `vsyncTick`. This is unresolved and
untriaged.

The next executable action is to triage that VBlank pacing failure against a
Windows run of the same test, then perform a first cold-boot execution of the
Linux `ps2EntryRunner` and record what it reaches.

### Phase 4 v163 GS dirty-range synchronization prototype (2026-09-01)

The first dependency-aware OpenGL/CPU GS coherence slice is implemented behind
the opt-in `PS2X_ENABLE_OPENGL_GS_DIRTY_RANGES`; the default route is unchanged.
It conservatively describes draw reads and writes as 256-byte GS blocks,
coalesces adjacent blocks into partial SSBO upload/readback ranges, and uses an
ordered fence before allowing a CPU fallback to bypass GPU readback. The fence
also tracks commands already removed from the host queue, closing the prior
empty-queue/in-flight race. Wrapping or unsupported surfaces retain the full
synchronization fallback. Canonical and staged runtime sources are synchronized.

Two focused coherence tests bring `PS2GS` to 89/89 in
`build/phase3/ps2x-tests-v163-gs-coherence-r2.log`; the Release runner rebuilds
successfully with only the established Raylib duplicate-symbol `/FORCE`
warnings. A deterministic sprite-enabled read-1500 replay completed without a
deadlock or coherence failure, but it did not pass the speed gate: at 300,031
GPU draws it had reduced downloads to 1,261 MiB while still uploading 3,504 MiB,
reported zero disjoint CPU fallbacks, and reached read 1500 no faster than the
84.340-second prior sprite run. The run was stopped after that checkpoint at
the user's request; evidence is
`build/phase3/phase4-v163-dirty-ranges-sprite-live-r1.log`.

The prototype therefore remains disabled by default. Its counters show that
the current conservative descriptor falls back to whole-image invalidation at
nearly every GPU/CPU transition. The next executable action is to classify the
unsupported fallback states causing invalidation, add exact transfer/write
ranges for the demonstrated classes, and repeat full-VRAM equivalence plus a
matched timing run before considering default enablement.

### Phase 4 v163 recomp timing/readback research checkpoint (2026-09-01)

The user's 63.189-second 1920x1080/60 capture was reviewed against the supplied
normal-speed longplay reference (`https://www.youtube.com/watch?v=JvdAHmVA-vI`).
This remains a throughput failure rather than evidence that the guest clock
should simply be multiplied: the established target is approximately 29.97
game updates/s, while the active 3D route is still compute/backpressure bound.

The previously staged CT32/FST/MODULATE sprite route is now implemented and
differentially verified. A non-writing first capture was skipped, an overlapping
texture/framebuffer capture was rejected, and the first disjoint point-sampled
capture changed 372 bytes and matched the CPU result over all 4 MiB, including a
64-draw ordered batch. A second real-game linear-filtered capture changed 158
bytes and also matched all 4 MiB plus the 64-draw batch. The proven states use
`ABE=1`, `PABE=0`, `COLCLAMP=1`, CT32 source/destination, fixed FST UVs,
`CLAMP=0x0000037c003fc00a`, and ALPHA values `0x0000001000000068` (point) or
`0x0000002000000068` (linear). Evidence is:

- `build/phase3/v163-ct32-sprite-r3.*` and
  `build/phase3/v163-ct32-sprite-probe-r4.stderr.log`;
- `build/phase3/v163-ct32-sprite-blended-linear-r1.*` and
  `build/phase3/v163-ct32-sprite-blended-linear-probe-r1.log`.

Live integration exposed and fixed a real circular wait. The GS packet worker
held `m_stateMutex` while waiting for an ordered GPU readback; the OpenGL-owning
host thread attempted to take the same mutex only to append a diagnostic present
history row, so it could no longer service the readback. Presentation history
now uses a non-blocking lock and skips only that diagnostic row when contended;
packet synchronization also pumps host work between bounded waits. The traced
route now completes GPU batches and readbacks instead of freezing at controller
read 1266. Evidence is `build/phase3/phase4-v163-ct32-sprite-sync-r3.log`.

The speed gate prevents enabling the sprite path by default. Matched read-1500
runs measured 83.858 seconds with the first point path versus 83.717 seconds
with sprites disabled; point+linear widened coverage measured 84.340 seconds.
The change therefore moves raster work but does not yet improve total frame
throughput. New transfer counters explain why: by 100,003 eligible draws the
hybrid route had spent about 70.46 ms in dispatch (including 57.40 ms of full
uploads) but 409.56 ms in 335 full 4 MiB readbacks. At 200,008 draws, readback
time had reached 720.66 ms. `PS2X_ENABLE_OPENGL_GS_SPRITE=1` now gates the
verified sprite experiment; normal execution retains the earlier T8 route.
Evidence is:

- `build/phase3/phase4-v163-ct32-sprite-route-timing-r2.log`;
- `build/phase3/phase4-v163-ct32-sprite-disabled-timing-r1.log`;
- `build/phase3/phase4-v163-ct32-sprite-point-linear-timing-r1.log`;
- `build/phase3/phase4-v163-opengl-transfer-cost-r1.log`.

Additional static-recompilation research was performed from primary project
sources; no source was copied. N64ModernRuntime revision
`cdf5abbd5026fef5c364c676e4667c45e42b6863` (`ultramodern/src/events.cpp`)
anchors VI interrupts to an absolute host deadline on a critical timing thread
and feeds a separate graphics action queue. Project8Recomp revision
`c8c21e2781acd134de6b2ccf12b666145e00ea6c` (`CHANGELOG.md` and
`patches/rexglue-sdk/README.md`) corrected below-speed Windows behavior with
high-resolution deadline waits, bounded vblank catch-up, and waits on the exact
producer event, then retained only A/B-measured command/cache changes.
TheSimpsonsGameRecomp revision `42c478592b0b5fc35e615df5ec79e16d04852d89`
documents the same separation between guest vblank semantics and achieved
render throughput, plus a staged native-renderer takeover. Most directly,
DownpourRecomp revision `66c075d9fe9cbf712ac1694a7b108ae630a0e06a`
describes replacing dozens of mid-frame memexport fence drains with queued
readbacks and one end-of-frame drain. Its published v1.0 SDK source at
`03b3282fd1263c5642f5925ba625b3ba0f6940c9` shows the preceding triple-buffered,
targeted-submission design and explicitly records why stale guest-visible data
is unsafe.

The applicable next action is therefore not a timing multiplier. Add exact
GPU/CPU local-memory dependency tracking so disjoint work does not force a full
4 MiB round trip, coalesce guest-visible downloads at the real sync/swap
boundary, and retain immediate targeted synchronization only when a CPU draw or
guest read overlaps GPU-dirty blocks. Prove the coherence transitions with
captured before/after VRAM and matched read-1500 A/B runs before enabling the
sprite path. Phase 4 remains incomplete.

### Phase 4 v163 external-architecture and live fallback checkpoint (2026-09-01)

The speed investigation was cross-checked against two mature open-source PS2
implementations rather than continuing with local interpreter guesses. No
source was copied. The inspected references were:

- Play! at revision `e9aa5ace0a5a54a86f6911d24ca82651f5853f71`, especially
  `Source/ee/VuBasicBlock.cpp`, `Source/ee/VuExecutor.cpp`, and
  `Source/ee/VUShared.cpp`;
- PCSX2 2.6.3 at the already recorded local revision, especially its microVU
  program/pipeline-state cache and hardware-GS submission architecture.

Both validate the same direction: VU optimization must compile complete blocks
with precomputed stalls, flag liveness, branch-delay behavior, and live exit
pipeline state; GS optimization must retain a GPU-resident local-memory model
and widen accurate hardware rendering instead of repeatedly round-tripping the
complete 4 MiB image through a narrow hybrid seam.

The captured VU schedule is now proven invariant across 1,000,000 live pairs.
Almost every reachable pair advances one cycle; the six fixed stalls are
`0x0090=2`, `0x00F0=3`, `0x01D0=2`, `0x01E8=3`, `0x0228=3`, and `0x0238=3`.
The generated side-owned dispatcher now covers six nonterminal blocks and
uses those compile-time deltas. Generalized compile-time write metadata fixed
the earlier multi-block scheduler-sequence mismatch. More than 2,097,152 live
generated blocks matched the scalar interpreter exactly, including registers,
flags, active queues/readiness, VU memory, and XGKICK state. The repeated
`0x0158` self-loop is now executed inside one generated dispatch and has passed
65,536 additional live scalar-differential superblocks. `PS2VU1` remains 41/41.
Evidence is:

- `build/phase3/phase4-v163-vu-captured-schedule-r1.log`;
- `build/phase3/phase4-v163-vu-six-block-differential-r2.log`;
- `build/phase3/phase4-v163-vu-loop-dispatch-collapse-diff-r1.log`;
- `build/phase3/ps2x-tests-v163-vu-loop-dispatch-collapse-r1.log`.

The performance result prevents overclaiming. Compile-time scheduling improved
the one-block read-1500 result from 83.131 to 81.323 seconds, but the six-block
measurement was 82.060 seconds, within run variance. Expanding blocks while
still materializing every scalar queue write cannot close the gap; the later
compiler must carry only live exit state.

A fresh hybrid-GS trace found the more immediately actionable workload. At
100,002 GPU draws the route had also submitted 282,808 CPU fallback draws and
performed 210 ordered full-image readbacks; at 500,003 GPU draws those totals
were 995,788 CPU fallbacks and 1,054 readbacks. A twelve-second delayed fallback
profile found that CT32 textured sprites dominate CPU raster time: the largest
blended point-sampled class consumed 1,705.52 ms, with other CT32 sprite classes
consuming 625.58, 598.35, and 31.04 ms. CT16/T8H and untextured sprite classes
add further cost. The already accelerated T8/STQ triangle class is not the
largest remaining CPU class. Evidence is:

- `build/phase3/phase4-v163-gs-hybrid-churn-profile-r1.log`;
- `build/phase3/phase4-v163-gs-fallback-class-profile-r1.log`;
- `build/phase3/phase4-v163-vu-six-block-timing-r2.result.txt`.

Current executable action: add an exact CT32/FST sprite compute path beginning
with the demonstrated dominant state, capture CPU before/after VRAM as the
differential oracle, and expand only after exact 4 MiB comparisons pass. Keep
the scalar VU interpreter and CPU GS backend as correctness oracles and
fallbacks. Phase 4 remains incomplete.

Work-in-progress at handoff: the CPU capture seam for the guarded CT32/FST
sprite class and the corresponding parameter/eligibility metadata were added
to canonical and staged sources, but the GLSL sprite body has not yet been
implemented and these final edits have not been rebuilt. Because eligibility
currently recognizes the new sprite state before its shader path exists, the
next agent must finish the shader or temporarily keep sprite eligibility false
before running a normal game replay. Do not treat the current runner binary as
containing these unbuilt edits.

### Phase 4 v163 VU1 hot-path checkpoint (2026-09-01)

The live OpenGL route established that GPU completion is not the active
gameplay limiter, so the current speed work is focused on the scalar VU1
producer. Three semantics-preserving operand-preparation changes are verified:

- broadcast FMAC operations no longer normalize an already-normalized VT lane
  a second time;
- ACC, Q, I, VS, and VT snapshots are normalized only when the decoded opcode
  and selected destination lanes can observe them;
- the hot upper NOP and CLIP paths bypass unrelated normalized-operand setup.

Canonical and staged VU sources are synchronized. The complete `PS2VU1`
hazard/timing suite remains 41/41 after each step, with the latest completed
log at `build/phase3/ps2x-tests-v163-vu-hot-upper-fast-r1.log`.

Matched twelve-second delayed profiles use exactly 35 VIF1 chains and
136,391,880 bytes. Against the original live-OpenGL baseline, complete VIF
time fell from 5,484.24 ms to 5,152.59 ms (6.0%) and VU callback time fell from
4,964.61 ms to 4,640.94 ms (6.5%). DIRECT and UNPACK work remain essentially
flat. Evidence is:

- `build/phase3/phase4-v163-opengl-vu-producer-profile-r1.log`
- `build/phase3/phase4-v163-vu-hot-upper-profile-r1.log`
- intermediate comparisons in
  `build/phase3/phase4-v163-vu-lazy-operands-profile-r1.log` and
  `build/phase3/phase4-v163-vu-source-lanes-profile-r1.log`

The gated VU aggregate profiler now also records upper-opcode and destination-
lane counts. In both dominant starts (`0x288` and `0x60`), the same nine upper
classes account for the hot loop: NOP, MADDbc, MULq, MADDAbc x/y/z, MULAbc,
FTOI0/FTOI4, and CLIP. Approximately 39% of their pairs are product-sum FMAC
operations. Evidence is
`build/phase3/phase4-v163-vu-opcode-profile-r1.log`.

A first single-pass exact-FMAC attempt used thread-local state to preserve the
old private interface. Although its flag, overflow/underflow, pipeline, and
XGKICK regressions passed 41/41, it regressed the matched live sample by about
1.1% versus the preceding hot-upper build (`4,695.49` ms VU and `5,207.33` ms
total). That exact experiment was removed. A proper by-reference private
interface then eliminated the second product/sticky pass without thread-local
traffic. It passes `PS2VU1` 41/41 in
`build/phase3/ps2x-tests-v163-vu-fmac-byref-r1.log` and improves the same
35-chain sample to `4,617.34` ms VU and `5,127.08` ms total. Relative to the
original live-OpenGL baseline, the verified cumulative reductions are now
7.0% in VU time and 6.5% in complete VIF time. Evidence is:

- `build/phase3/phase4-v163-vu-single-pass-fmac-profile-r1.log` (rejected);
- `build/phase3/phase4-v163-vu-fmac-byref-profile-r1.log` (retained).

The cancelled generated-unity rebuild left thirteen zero-byte object files.
Only those exact damaged unity sources were recompiled, after which the runner
was relinked from the existing Release response inputs. No game map was
regenerated and v163 remains current. The runner is linked, smoke-executes, and
the retained by-reference build produced the matched profile above.

A follow-up audit found that the Visual Studio multi-configuration build never
entered the project's fast Release-mode branch because that branch tested only
`CMAKE_BUILD_TYPE`. The canonical and staged runtime now enable the Release
flags for multi-configuration generators as well. The portable baseline keeps
`/fp:precise` and does not force AVX2, while the actual VU compile now receives
`/O2 /Ob2 /Oi /GL /GF /GS- /Gy /fp:precise /Gw`. `PS2VU1` remains 41/41 in
`build/phase3/ps2x-tests-v163-vu-ltcg-r1.log`. A matched instrumented profile
fell to 4,604.17 ms VU and 5,117.38 ms complete VIF time; the production-style
profile without per-instruction opcode census measured 4,588.84 ms VU and
5,096.29 ms total. Evidence is:

- `build/phase3/phase4-v163-vu-ltcg-profile-r1.log`;
- `build/phase3/phase4-v163-vu-ltcg-production-profile-r1.log`.

Two additional scalar experiments were rejected and fully removed. A guarded
boundary-only wider-precision FMAC route passed 41/41 but regressed the matched
sample to 4,781.88 ms VU. Expanding the interpreter-embedded decoded-pair array
to cache upper fields also passed 41/41. After a complete generated-code rebuild
made the new layout safe to execute, however, the real game stopped producing
geometry after its first two gameplay draws. The unit suite was insufficient to
detect that semantic regression, so the experiment was completely reverted in
both canonical and staged sources. The restored exact and boundary-optimized
layouts each pass `PS2VU1` 41/41. Future block metadata must be side-owned and
must be checked against captured real microprograms before replacing scalar
execution. Evidence is:

- `build/phase3/phase4-v163-vu-fmac-boundary-fast-profile-r1.log` (rejected);
- `build/phase3/ps2x-tests-v163-vu-cached-upper-decode-r1.log` (initial
  rejected layout attempt);
- `build/phase3/ps2x-tests-v163-vu-predecoded-upper-default-r1.log` and
  `build/phase3/ps2x-tests-v163-vu-predecoded-upper-boundary-r1.log` (41/41 but
  insufficient);
- `build/phase3/phase4-v163-vu-predecoded-upper-boundary-profile-r1.log`
  (decisive two-draw live failure);
- `build/phase3/ps2x-tests-v163-vu-post-revert-default-r1.log` and
  `build/phase3/ps2x-tests-v163-vu-post-revert-boundary-r1.log` (restored
  layouts, 41/41 each).

The `PS2X_VU_NATIVE_RESULT_FLAGS` experiment first tried the raw PCSX2-style
result-only flag model. It passed 39/41 focused tests, failing only the two
strict exceptional-product/overflow cases, but the live game stopped producing
geometry after the first two gameplay draws. That raw shortcut is disproven
for Haunting Ground and is not the implemented fast route.

The refined opt-in path took ordinary finite result and product flags directly
from their normalized host-float values, while falling back to the exact scalar
calculation at exponent boundaries. It initially passed the full `PS2VU1`
suite 41/41 and one long live run, and reduced a matched sample from 4,588.84 ms
to 4,393.58 ms VU time (4.25%). A clean rebuilt-runner comparison disproved it:
the exact path exceeded 100,000 gameplay draws, while the refined path stopped
after its first two. The shortcut and its environment gate were therefore
removed completely. Exact FMAC behavior remains the only production path.
Evidence is:

- `build/phase3/phase4-v163-vu-native-result-flags-profile-r1.log` (raw model,
  rejected after the two-draw gameplay stall);
- `build/phase3/phase4-v163-vu-boundary-result-flags-profile-r1.log` (initial
  favorable measurement, now rejected);
- `build/phase3/phase4-v163-vu-post-revert-live-r1.log` (refined path stalls at
  two draws after the clean rebuild);
- `build/phase3/phase4-v163-vu-post-revert-exact-live-r1.log` (exact path
  exceeds 100,000 draws under the same replay);
- `build/phase3/ps2x-tests-v163-vu-final-exact-r1.log` (41/41 after removal).

The Release helper is now applied only to `ps2_runtime`, not the approximately
11,000-function generated runner target. The generated game code remains on
normal `/O2`, while the measured runtime hotspot retains precise-float LTCG.
This removed a multi-minute whole-game LTCG penalty from every runtime-only
relink; after the one-time generated-object rebuild, the refined runtime linked
in about ten seconds. This is a build-loop correction, not a gameplay-speed
claim.

These interpreter improvements are useful but cannot alone provide the roughly
fivefold speedup still required for 29.97 updates/s. The PCSX2 microVU reference
confirms the appropriate major architecture: program identity/generation
tracking, cached analyzed blocks, native block execution, and explicit
pipeline/flag state at block boundaries. Continue with a portable analyzed-
block layer and host-native x86-64 execution for the two measured hot starts,
retaining the exact scalar interpreter as the differential oracle and portable
fallback.

An opt-in local capture seam now makes that route concrete. Setting
`PS2X_VU1_CAPTURE_PROGRAMS_DIR` records each unique complete VU1 code image and
start PC by a deterministic FNV-1a hash, together with generation and size
metadata. It is inert in normal runs, emits only under disposable `build/`, and
does not embed or commit the user's program bytes. `PS2VU1` remains 41/41 in
`build/phase3/ps2x-tests-v163-vu-capture-scaffold-r1.log`; the capture replay
also exceeded 100,000 gameplay draws. The stable late-gameplay image observed
in this route is hash `0xa198bebb3e5909d9`, generation `0xbf`, with both hot
starts `0x60` and `0x288`. Earlier matching pairs occur at hashes
`0x95a9ec11eace3c78` and `0x70bbc999c6a7b554`. The capture set is only about
1.7 MB (206 metadata/binary files), not another generated-code archive.
Evidence is `build/phase3/phase4-v163-vu-program-capture-r1.log` and
`build/phase3/vu1-captures-v163/`.

The next executable optimization action is to analyze the captured
`0xa198bebb3e5909d9` control-flow regions rooted at `0x60` and `0x288`, emit
side-owned AOT host blocks for their exact hash, and dispatch them only after a
scalar-versus-AOT differential run proves matching VU registers, pipeline
flags, VU memory, and XGKICK output at every block exit. Every unmatched hash or
unsupported block continues through the exact interpreter.

`tools/analyze_vu1_capture.py` now performs the first of those steps without
copying program bytes into its report. For the stable captured image, both hot
starts reduce to 71 reachable instruction pairs in seven delay-slot-aware basic
blocks. One 15-pair conditional block at `0x0158` is the internal loop; `0x0288`
is a two-pair adapter into the shared `0x0060` body. The reachable upper side is
exactly the nine classes already identified dynamically. The machine-readable
report is `build/phase3/vu1-a198bebb3e5909d9-hot-cfg.json`. Three focused tests
cover direct-branch/end delay slots, malformed alignment, and invalid capture
sizes in `tests/test_analyze_vu1_capture.py`.

The corresponding lower side is also narrow: 11 LQ, 11 SQ, six IADDI, five
ISUBIU, four ISW, four MTIR, four FCAND, four IADD, three IADDIU, three WAITQ,
three DIV, two direct branches, two IBEQ, and one each SQI, IBNE, XTOP, and
XGKICK (plus five NOPs). This confirms the hot path is a compact vertex
transform/packet loop with one scalar divide pipeline, not an unrestricted VU
workload. The first AOT implementation can cover this exact operation set and
fall back at block granularity for anything else.

A 1-in-256 sampled phase profiler now separates each interpreted pair into
pre-execution decode/hazard work, upper/lower semantics, and post-execution
pipeline/writeback/control work. It is part of the existing aggregate profiler
and remains inactive in ordinary runs. `PS2VU1` passes 41/41 in
`build/phase3/ps2x-tests-v163-vu-sampled-phase-profiler-r1.log`. In the valid
12-second gameplay sample, start `0x288` averaged 25 ns pre, 67 ns execution,
and 52 ns post over 43,805 sampled pairs; start `0x60` averaged 27/68/53 ns over
30,039 samples. Instrumentation overhead inflates the absolute sampled times,
but the proportions are decisive: approximately 53% of pair cost lies outside
the arithmetic instruction bodies. Optimizing FMAC math alone cannot close the
gap. A compiled block must remove repeated hazard scans, pipeline queue
search/writeback, and per-pair control handling as well as specialize the nine
upper operations. Evidence is
`build/phase3/phase4-v163-vu-sampled-phase-profile-r2.log`.

The first safe block-oriented change now validates the tracked VU code pointer,
size, memory owner, and generation once at synchronous `run()` entry, then
indexes the decoded cache directly for each aligned pair. MPG/direct writes are
still observed through the generation check at every run boundary, and
untracked/misaligned code retains the old fallback. `PS2VU1` remains 41/41 in
`build/phase3/ps2x-tests-v163-vu-run-cache-r1.log`, and the deterministic live
profile again exceeds 100,000 draws. Normalized host time improves from 68.94
to 68.25 ns/cycle at start `0x288` and from 69.92 to 69.19 ns/cycle at `0x60`,
approximately 1.0% in each dominant program. This is retained, but it also
quantifies that per-pair cache validation is only a small portion of the
remaining cost. Evidence is
`build/phase3/phase4-v163-vu-run-cache-profile-r1.log`.

A follow-up attempt to jump directly to the next pipeline-ready cycle when
XGKICK was inactive passed the focused suite but was neutral to slightly slower
in the live route. The dominant gameplay microprogram keeps PATH1 XGKICK active
through its hot body, so the proposed inactive interval was not present. That
experiment was removed completely rather than retained as dead complexity.

The active XGKICK transfer itself was then tightened without changing its
two-cycle credit model or packet order. Each due 16-byte quadword now uses one
or two contiguous copies across the circular VU-memory boundary instead of 16
per-byte modulo/copy operations. `PS2VU1` passes 41/41 in
`build/phase3/ps2x-tests-v163-vu-xgkick-bulk-copy-r1.log`, and the deterministic
profile reached more than 900,000 completed live draws. Normalized dominant
program cost fell from 68.25 to 67.42 ns/cycle at start `0x288` and from 69.19
to 68.33 ns/cycle at start `0x60`, approximately 1.2% in each case. The change
is retained. XGKICK bookkeeping now accounts for only about 2.9% and 3.1% of
those two samples, further confirming that the remaining gain must come from
compiling the VU block and its exact PATH1 schedule rather than optimizing the
copy primitive alone. Evidence is
`build/phase3/phase4-v163-vu-xgkick-bulk-copy-profile-r1.log`.

Two further queue-scheduling experiments were measured and removed. An
earliest-ready pipeline calendar passed 41/41 but increased dominant cost to
68.29 and 69.07 ns/cycle because the hot loop commits often enough that
calendar recomputation costs more than the skipped empty scans. Evidence is
`build/phase3/phase4-v163-vu-pipeline-calendar-profile-r1.log`; the focused log
is `build/phase3/ps2x-tests-v163-vu-pipeline-calendar-r1.log`.

PCSX2 was then inspected as an open-source architecture reference at exact
upstream revision `00482e8adadd4baa8ac720e382803d169142a8ad`; the sparse local
reference is disposable `build/pcsx2-source-ref`. No source was copied. Its
microVU compiler accumulates PATH1 cycles and synchronizes transfers at memory
writes, block exits, and conflicting kicks. Reproducing that schedule in the
scalar interpreter passed 41/41 but regressed the hot starts to 68.19 and 69.20
ns/cycle because this 71-pair program performs eleven SQ operations and forces
frequent synchronization. It was removed completely; the restored runtime
passes 41/41 in
`build/phase3/ps2x-tests-v163-vu-post-batching-revert-r1.log`. Evidence for the
rejected live run is
`build/phase3/phase4-v163-vu-xgkick-scheduled-batch-profile-r1.log`.

The useful PCSX2 result is architectural rather than a scalar-loop tweak:
program copies are cached by start PC, analyzed blocks are keyed by the
pipeline state actually live at entry, flag-read liveness is traced through
successors, and XGKICK cycles are embedded in emitted blocks. The next speed
implementation must therefore replace the 71-pair interpreter loop with
cached native blocks; further queue polling changes are not expected to close
the approximately fivefold gap.

The corresponding native-block entry-state census is now complete. Across a
fixed twelve-second gameplay sample, all 19,221 calls at start `0x288` used the
single state hash `0xf61b5009b28df046`, and all 14,211 calls at start `0x60`
used the single state hash `0xc2ae4da6c40595c6`. No secondary variant was
observed. Evidence is
`build/phase3/phase4-v163-vu-entry-state-profile-r1.log`. This is a favorable
constraint for the first compiler: the stable captured program requires one
verified entry variant for each hot start, not a combinatorial state
dispatcher.

Two implementation probes clarified what that compiler must and must not do.
Suppressing exact FMAC flag reconstruction for the apparent flag-dead region
still stopped live geometry after two draws, despite passing `PS2VU1` 41/41;
the experiment was rejected. A constant-instantiated version of the repeated
15-pair block preserved sustained rendering beyond 100,000 draws and passed
41/41, but retained the scalar semantic and pipeline helpers and regressed the
hot starts to approximately 71.06 ns/cycle (`0x288`) and 71.94 ns/cycle
(`0x60`). Evidence is
`build/phase3/phase4-v163-vu-hot-flag-liveness-profile-r1.log`,
`build/phase3/phase4-v163-vu-captured-hot-block-profile-r1.log`, and their
matching focused test logs. Mere unrolling is therefore disproven; the useful
native block must emit direct host arithmetic plus pre-analyzed hazards,
flag timelines, stores, branch-delay behavior, and XGKICK scheduling, with the
scalar interpreter retained for differential comparison and fallback.

The first direct-emission probe now covers every upper and lower operation in
the repeated `0x0158-0x01C8` block and uses compile-time writeback metadata.
Unlike the helper-only unroll, it remains live beyond 100,000 draws. The
complete direct form measured 61.66 ns/cycle at start `0x288` and 63.47 at
`0x60`; static writeback metadata improved those values to approximately 59.85
and 61.80 ns/cycle. Against the retained XGKICK-copy baseline, this is an
approximately 11.2% and 9.6% reduction. A cleaned repeat measured 59.78 and
61.58 ns/cycle. Evidence is:

- `build/phase3/phase4-v163-vu-captured-direct-complete-loop-profile-r1.log`;
- `build/phase3/phase4-v163-vu-captured-static-writeback-r2.log`;
- `build/phase3/phase4-v163-vu-captured-clean-profile-r1.log`.

The compiled block was compared at every exit with a cloned exact scalar
interpreter. After correcting the checker to compare the live internal cycle
counter rather than the oracle's finalized public snapshot, 50,000 consecutive
blocks matched registers, scalar state, MAC/status/clip flags, all active
pipeline queues and readiness metadata, branch state, stores, and the complete
VU data image. `PS2VU1` remains 41/41. Evidence is
`build/phase3/phase4-v163-vu-captured-static-diff-r14.log` and
`build/phase3/ps2x-tests-v163-vu-captured-static-writeback-r1.log`.

This probe is not production-enabled. Subsequent no-observer/default-on and
trace-enabled replays stopped after two draws even though earlier matched runs
continued beyond 100,000. That observer sensitivity means the code-hash guard
alone is insufficient; the block also needs an explicit analyzed entry-state
contract and external-side-effect differential. A narrow trace established
that the repeated loop normally enters with `vi5 = 46`, decrements once per
15-cycle iteration, and advances `vi3`/`vi4` by three each time. Its flag free
mask alternates `0xFD`/`0xFE`, while the sampled VF/VI/ACC free masks remain
`0xFFF1`/`0xFF`/`0xFF`. Evidence is
`build/phase3/phase4-v163-vu-captured-loop-trace-r1.log`. The exact scalar path
therefore remains the default. After removing the rejected shortcuts and trace
instrumentation, the rebuilt runner again exceeded 100,000 gameplay draws and
the focused suite passed 41/41; see
`build/phase3/phase4-v163-vu-final-stable-live-r1.log` and
`build/phase3/ps2x-tests-v163-vu-final-stable-r1.log`.

Two tempting follow-ups are rejected. Removing the hazard scan while retaining
only a block-budget guard appeared to match under the differential observer,
but a clean no-observer replay stopped after two draws; the scalar oracle had
masked the timing defect. A conservative normal-exponent FMAC flag shortcut
also passed 41/41 but stopped live geometry after two draws and was fully
reverted. Exact flag behavior and per-pair hazard evaluation remain required
until the generated block represents their complete timeline. The next action
is to move the exact-hash block out of reusable runtime source into a generated,
side-owned superblock and collapse its proven pipeline schedule as one native
state transform; expanding scalar shortcuts is no longer the active strategy.

That ownership split is now implemented. The reusable VU core exposes an
optional `ps2_vu1_aot_blocks.inc` include seam, while
`tools/generate_vu1_aot_include.py` validates a delay-slot-aware analyzed block
and emits the exact hash/pairs only into disposable build output. The generated
file in `build/runtime-source-phase3/ps2xRuntime/src/lib/vu/` exactly reproduces
the analyzed `0x0158` block; the canonical runtime no longer requires that
user-owned generated file. Generator byte-for-byte reproduction passed, the
runner rebuilt, and `PS2VU1` remains 41/41 in
`build/phase3/ps2x-tests-v163-vu-generated-seam-r1.log`. This is an
architecture/ownership checkpoint, not a new speed claim; the generated body
is still opt-in until its external timing contract is proven.

Adding the observed `0xFD`/`0xFE`, `0xFFF1`, `0xFF`, `0xFF` pipeline-mask
contract to the generated dispatch did not resolve the no-observer two-draw
stall (`build/phase3/phase4-v163-vu-generated-entry-guard-live-r1.log`). The
missing contract is therefore not just free-slot shape. Because MSCAL/MSCNT are
currently invoked synchronously from `PS2Runtime`, the next implementation will
retain exact scalar semantics while moving VU1 execution behind an explicit
worker/fence seam modeled on the PS2's concurrent VU1, or replace the entire
46-iteration body with an externally differential-tested superblock. The
generated partial block remains disabled by default.

The apparent no-observer AOT stall was subsequently separated from VU
correctness. A live no-side-effect scalar-versus-AOT checker matched the full
architectural state, all active scheduler queues/readiness metadata, XGKICK
state, and the entire VU data image through controller read 1500 and more than
300,000 submitted draws. An independent pre-workload PATH1 capture also
matched byte-for-byte: 1,638 records, 4,504,032 bytes, SHA-256
`65bd90d0d5ce9bfa6a5d5b79aa2e37799677ca1cc8cdd2e8d14645d378c35a63`.
Evidence is `build/phase3/phase4-v163-vu-aot-live-differential-exact-r1.log`,
`build/phase3/vu-scalar-path1-r1.bin`, and
`build/phase3/vu-aot-path1-r1.bin`.

The actual stall was an ordered OpenGL readback waiting for the display thread
while holding `m_backendLifetimeMutex`; host queue service attempted to acquire
the same mutex. Canonical and staged GS ownership now use a copied
`shared_ptr`, so backend lifetime is retained without holding the lifetime
mutex across `processHostWork`, shutdown, VRAM snapshots, or presentation
latching. `PS2GS` passes 87/87 in
`build/phase3/ps2x-tests-v163-gs-shared-backend.log`. Narrow opt-in tracing then
proved the remaining delay was not another deadlock: ordered readbacks were
queued behind hundreds of GPU work items, while the Raylib loop serviced the
queue only once per 60 Hz presentation iteration. Evidence is
`build/phase3/phase4-v163-vu-aot-opengl-sync-r1.log`.

An opt-in continuously serviced host loop (`PS2X_UNCAPPED_HOST_SERVICE`) removes
that artificial 60 Hz queue-service barrier and lets the AOT route proceed
normally. Exact identical-replay read-1500 timings are now:

- exact scalar with 60 Hz host service: 86.638 seconds;
- exact scalar with continuous host service: 83.885 seconds;
- generated 15-pair AOT with continuous host service: 83.131 seconds.

The corresponding logs are
`build/phase3/phase4-v163-vu-scalar-read1500-timing-r1.log`,
`build/phase3/phase4-v163-vu-scalar-uncapped-host-r1.log`, and
`build/phase3/phase4-v163-vu-aot-uncapped-host-r1.log`. Continuous queue service
therefore contributes about 3.2%, while the currently narrow AOT block adds
only about 0.9% end-to-end. Neither closes the gap. The hard NTSC presentation
target is 16.68 ms per field (59.94 Hz), with approximately 33.37 ms per
29.97-Hz game update. If this replay polls once per field, read 1500 would be
near 25 seconds; an exact PCSX2 replay is still required before treating that
controller-read conversion as an oracle measurement.

Current executable action: decouple continuous ordered GPU queue service from
60 Hz window presentation, then expand generated execution from the one
`0x0158-0x01C8` loop block to the complete seven-block, 71-pair program rooted
at `0x0060`/`0x0288`. The performance-critical version must collapse the proven
46-iteration pipeline/branch schedule into a block state transform; merely
adding more compile-time pair calls cannot supply the required speedup.

### Phase 4 v163 live OpenGL/VU bottleneck checkpoint (2026-08-31)

Gameplay speed remains the user's explicit first priority. The required NTSC
timing target is approximately 29.97 game updates per second (33.37 ms per
update) while presenting 59.94 fields per second. The current active 3D scene
is still only approximately 5–6 updates per second, so roughly a fivefold
overall speedup remains necessary before the runner can claim PS2-rate
gameplay.

The ordered OpenGL 4.3 hybrid GS backend is now live on the available Windows
RTX 5070 host for the exact measured T8/STQ/MODULATE/CT32/Z24 state. It keeps
the raw 4 MiB GS local-memory image resident in an SSBO, accepts immutable draw
work from the GS worker, executes OpenGL work on the context-owning display
thread, and performs ordered readback before CPU fallback, transfers, and
presentation. Unsupported states continue through the established CPU backend.
`PS2X_DISABLE_OPENGL_GS` remains the diagnostic opt-out.

The captured real-game single draw matches the CPU backend across all 4 MiB
exactly, including its 1,001 changed bytes. A 64-draw ordered batch derived from
that capture also matches exactly. Draw parameters are stored in an SSBO and
the shader dispatch is limited to the conservative union of per-draw bounding
boxes instead of the complete 512x448 scissor. The deterministic live route
produces a coherent gameplay frame containing Fiona, the room, camera,
geometry, textures, and depth without the earlier experimental corruption.
Evidence is:

- `build/phase3/phase4-v163-opengl-live-t8-replay-probe-r2.log`
- `build/phase3/phase4-v163-opengl-live-batch-probe-r2.log`
- `build/phase3/ps2x-tests-v163-opengl-live-batch.log` (`PS2GS` 87/87)
- `build/phase3/phase4-v163-opengl-live-route-r1.log`
- `build/phase3/phase4-v163-opengl-live-route-capture.log`
- `UNEEDED images/v163-opengl-live-gameplay.png`

Live completion timing disproves the GPU rasterizer as the present frame-rate
limit. The first 100,002 gameplay triangles required only approximately
53.4 ms of cumulative GPU dispatch completion time and 200,023 required about
104.7 ms, while the first 100,000 triangles took roughly nine seconds of wall
time to arrive. The GPU is keeping up with submitted work; geometry production
upstream is not.

Aggregate VIF/VU profiling now isolates that producer bottleneck. In the
matched window, 35 VIF1 calls processed 136,391,880 bytes in 5,484.24 ms:
73,605 VU callbacks consumed 4,964.61 ms (90.5%), 263,060 UNPACK commands
consumed 445.94 ms, and 35,667 DIRECT commands consumed 63.72 ms. The hottest
VU1 starts are `0x288` (26,916 calls, 1,655.15 ms) and `0x60` (19,006 calls,
1,049.07 ms); together they account for about 2.70 seconds of the sampled VU
time. Evidence is:

- `build/phase3/phase4-v163-opengl-live-dispatch-profile-r1.log`
- `build/phase3/phase4-v163-opengl-vu-producer-profile-r1.log`

The selected optimization path is therefore: first remove redundant eager
normalization and bookkeeping from the scalar VU1 interpreter with the 41-test
hazard/timing suite as a gate; then translate/cache the demonstrated hot VU1
microprogram blocks into native host execution while retaining exact flags,
pipeline stalls, branch behavior, and XGKICK order; finally overlap the ordered
EE, VIF/VU1, and OpenGL stages where guest-visible barriers permit it. Every
optimized result must be differentially checked against the scalar interpreter
and PCSX2. More GPU raster work alone cannot provide the remaining speedup.

Before treating the hybrid backend as generally authoritative, its batching
eligibility must reuse the CPU backend's proven texture/CLUT/frame/depth alias
test, host work draining must receive a presentation-safe budget, and shared
queue/state access must receive a focused concurrency audit. Phase 4 remains
incomplete: this checkpoint proves a correct live GPU subset and locates the
next bottleneck, not PS2-rate gameplay, complete movies/synchronization,
asynchronous ordinary reads, or cold-boot save reload.

### Phase 4 v163 OpenGL GS priority checkpoint (2026-08-31)

Gameplay speed remains the user's explicit first priority. The verified
multicore/VU series reduced complete VIF-chain time from approximately 460.1 ms
to 169.8 ms and VU callback time from approximately 281.5 ms to 148.0 ms. The
ordered asynchronous GS worker reduced guest-thread DIRECT/GIF blocking to
about 2.4 ms per chain and the persistent raster pool exposes fifteen worker
threads plus the caller for safe large draws. The deterministic route's read
1500 checkpoint improved from approximately 103.7 seconds to 84.640 seconds.
These are real improvements, but the runner remains far below PS2 gameplay
rate because the ordered GS consumer still takes roughly 198-200 ms per frame
chain and the VU producer takes roughly 170 ms.

The latest GS profile identifies the architectural workload: one five-second
gameplay window submits roughly 50,000-61,000 unblended, linearly filtered T8
triangles plus thousands of CT32 sprites. Reusing consecutive triangle CLUT
entries produced only a small approximately 2.3% class-level improvement. A
broader T8 texel-cache experiment produced no repeatable gain and was removed;
its initial parallel use also exposed and then eliminated a worker data race.
Incremental barycentric stepping changed edge pixels and failed the exact
cached-versus-serial GS differential test, so it was fully reverted.

An exact row-banded triangle prototype preserves per-pixel draw order by
processing independent 32-line bands on the raster pool. `PS2GS` remains 87/87
with it, but the game's compatible state runs average only 10.5 triangles
(maximum observed 252), causing synchronization overhead to outweigh saved
raster time. Read 1500 regressed to 86.325 seconds without a threshold and
85.805 seconds with a 64-triangle threshold. The prototype is therefore
diagnostic opt-in only through `PS2X_ENABLE_GS_TRIANGLE_BATCH`; it is disabled
in normal execution. Evidence is:

- `build/phase3/phase4-v163-gameplay-triangle-bands-r33.log`
- `build/phase3/phase4-v163-gameplay-triangle-batch-distribution-r34.log`
- `build/phase3/phase4-v163-gameplay-triangle-bands-threshold-r35.log`
- `build/phase3/ps2x-tests-v163-triangle-bands-threshold.log`

The next performance milestone is an accurate OpenGL GS raster backend, with
the CPU backend retained for differential testing, unsupported-state fallback,
and readback. Implement it incrementally from the proven hot path rather than
as a visual approximation: ordered primitive submission, PS2 texture/CLUT
addressing, alpha and destination-alpha tests, fixed-function blend equation,
depth semantics, framebuffer masks/feedback barriers, VRAM upload/readback,
and synchronization boundaries must remain explicit. Compare GPU output and
selected VRAM checkpoints against the current CPU backend and PCSX2 before
making it the default. Phase 4 movie, save, and route work remains paused until
gameplay timing is practical. The implementation and validation contract is
documented in `docs/OPENGL_GS_DESIGN.md`.

The first OpenGL foundation gate is now verified on Windows. CMake option
`PS2X_ENABLE_OPENGL_43_GS` defaults on for Windows/Linux and requests Raylib's
OpenGL 4.3 backend; it remains off on Apple and leaves the CPU renderer as the
portable fallback. The rebuilt v163 runner created an OpenGL 4.3 context on the
local NVIDIA GeForce RTX 5070, compiled and dispatched a GLSL 4.30 compute
shader, wrote a 256-word shader-storage buffer, applied an explicit memory
barrier, read the buffer back, and matched every word exactly. The opt-in probe
reported `available=1 passed=1`. `PS2GS` remains 87/87 after the context and
runtime integration changes. Evidence is:

- `build/phase3/phase4-v163-opengl43-probe.log`
- `build/phase3/ps2x-tests-v163-opengl43-foundation.log`

This proves only the required host compute/SSBO mechanism; no gameplay draw is
GPU-rasterized yet, and no gameplay-speed claim is attached to this checkpoint.
The second foundation gate now also passes. A deterministic OpenGL compute
probe generated 65,536 mixed CT32, Z32/Z24, T8, and CSM1 CT32-CLUT addresses;
every byte offset matched the established CPU implementation exactly across
varied base blocks, buffer widths, coordinates, CSA values, and TEXCLUT
offsets. On the available RTX 5070, 512 completed dispatches processed 33.6
million mappings at approximately 30,877.4 million mappings/second. This is a
narrow address-kernel throughput result, not a frame-rate result. `PS2GS`
remains 87/87. Evidence is:

- `build/phase3/phase4-v163-opengl-address-map-probe.log`
- `build/phase3/ps2x-tests-v163-opengl-address-map.log`

The immediate implementation gate is the measured unblended, linearly filtered
T8 triangle class: immutable draw batching from the ordered GS worker to the
context-owning display thread, exact raw-VRAM sampling/writes, and GPU-versus-CPU
touched-byte comparison before enabling it in normal gameplay.

The first complete T8 raster kernel now passes that synthetic differential
gate. It performs triangle coverage and FST interpolation, linearly filters
four PSMT8 samples, resolves the CSM1 CT32 palette, applies the demonstrated
GEQUAL Z32 test, and writes raw swizzled CT32/Z32 memory. One 17,684-pixel
triangle matched the CPU result across the entire 4 MiB VRAM image exactly.
Explicit GLSL `precise` arithmetic and double-precision depth interpolation are
required to reproduce the CPU path's rounding; earlier variants were rejected
for 55 and then 37/53 differing bytes. The completed RTX 5070 benchmark reports
approximately 42,916.3 million candidate pixels/second and 13,234.7 million
covered pixels/second. This remains a synthetic single-triangle result; the
live game still uses the CPU renderer until ordered batching and barriers are
integrated. Evidence is:

- `build/phase3/phase4-v163-opengl-t8-depth-probe.log`
- `build/phase3/ps2x-tests-v163-opengl-t8-probe.log` (`PS2GS` 87/87)

The immediate action is the live main-thread GPU command queue: preserve GIF
order, batch only source/destination-disjoint T8 draws, close batches at
transfers/feedback/unsupported state, and differentially compare touched VRAM
before allowing the GPU result to become authoritative.

### Phase 4 v163 multicore GS performance checkpoint (2026-08-31)

Gameplay speed is the user's explicit first priority; route/save/audio work is
paused until the native runner reaches practical PS2-rate execution without
discarding draws or changing guest timing. The current work retains ordered
guest execution and GS draw ordering, while parallelizing only raster rows
whose texture, palette, framebuffer, and depth address ranges are proven
non-wrapping and mutually disjoint.

The live runner now also overlaps the ordered GIF/GS consumer with the
guest/VIF/VU producer, matching the PS2's separate processing units more
closely instead of executing both stages synchronously on the game thread.
Packets are copied into one FIFO and consumed by one GS worker; uploads,
register writes, draws, and transfers therefore retain exact packet order.
Presentation, `sceGsSyncPath(0)`, native transfer shortcuts, readbacks, reset,
and backend replacement wait for the queue to become idle. Live execution
enables this worker by default (`PS2X_DISABLE_ASYNC_GS` is the diagnostic
opt-out), while construction-time test APIs remain synchronous.

In the matched gameplay window, normalized VIF chain time fell from about
429.7 ms to 232.8 ms (45.8%). DIRECT/GIF blocking inside VIF fell from about
136.1 ms to 2.4 ms per chain, the five-second window completed 13 chains
instead of 11, and controller read 1500 moved from approximately 103.7 seconds
to 91.1 seconds. The remaining producer cost is approximately 211.5 ms of VU
callbacks per chain, so gameplay is materially faster but still below PS2
rate. Evidence is
`build/phase3/phase4-v163-gameplay-async-gs-profile-r22.log`. Focused suites
pass `PS2GS` 87/87, `PS2RuntimeExpansion` 41/41, and `PS2Memory` 52/52 in the
corresponding `ps2x-tests-v163-async-gs-*-r2.log` files.

Follow-up VU work removed a duplicate full pipeline commit scan per executed
instruction, iterates decoded VI read/write masks instead of scanning all 15
integer registers, retains the required toward-zero floating-point mode across
each integer-only VIF chain, and tracks pending pipeline slots with exact free
bitmasks. Allocation and `pipelinesPending()` are now constant-time, while a
cycle commit visits only active entries. The 41-test `PS2VU1` hazard/timing
suite remains green after every step; the final log is
`build/phase3/ps2x-tests-v163-vu-pipeline-masks.log`.

The latest matched profile completes 16 frame chains in its five-second
capture. VU callbacks now cost approximately 148.0 ms per chain and complete
VIF processing costs approximately 169.8 ms per chain, versus 281.5 ms and
460.1 ms respectively at the first gameplay VIF baseline. Read 1500 now
arrives at 84.640 seconds, versus approximately 103.7 seconds before this
multicore/VU series. Evidence is
`build/phase3/phase4-v163-gameplay-vu-pipeline-masks-r28.log`. The ordered GS
consumer is now the longer stage at roughly 198-200 ms per chain, so the next
action is to optimize its measured CT32 strip and T8 triangle classes; more
VU-only work would currently be hidden behind that consumer.

Presentation conversion now runs on the host/display thread from the exact
completed-frame VRAM snapshot instead of converting the frame on the guest
thread. Exact CT32/Z32/Z24/CT24 aligned-page fills bypass per-pixel swizzle
address calculation; Z24/CT24 retain their destination high byte. The former
full-screen CT32+Z24 clear now has an exact guarded bulk path. Differential
tests include unaligned RMW behavior and a page-aligned 4 MiB wrap case.

A persistent fifteen-worker raster-row pool plus the calling thread now handles the demonstrated
disjoint full-screen CT32-to-CT24 copy and T4 composite. It replaces the
inconsistent standard parallel-policy experiment while preserving serial
fallbacks for framebuffer feedback, wrapping ranges, overlapping color/depth,
and small draws. Live slow-sprite timing changed as follows:

- the repeated 512x448 cached-T4 composite fell from 8.37 ms average to
  1.35 ms average;
- the 640x448 CT32-to-CT24 presentation copy is approximately 1.09 ms for
  observed parallel executions, versus roughly 3.2-3.4 ms before;
- `sceDmaSend@0x0010D6E8` fell from 1,404.17 ms to 648.759 ms in the same
  ten-second cold-boot window (53.8%);
- `sceGsSwapDBuffDc@0x0010D200` is 178.294 ms, versus 745.44 ms before the
  completed-frame/deferred-conversion/page-fill series (76.1%).

Evidence is
`build/phase3/startup-slow-sprites-v163-row-pool-r10.log` and
`build/phase3/startup-profile-v163-row-pool-r11.log`. Canonical and staged
runtime sources remain synchronized. The focused `PS2GS` suite passes 87/87
in `build/phase3/ps2x-tests-v163-async-gs-ps2gs-r2.log`; this does not yet prove
perfect gameplay speed or whole-route visual equivalence.

The immediate action is to reduce the remaining serialized VU producer cost,
then run an unprofiled full-route timing and internal-frame equivalence replay.
Do not resume the remaining Phase 4 save/movie gates until this speed gate is
practical and stable.

The gameplay profile has now been narrowed below the EE dispatch boundary.
Each active-scene frame submits one VIF1 source chain of approximately
3,896,904 bytes. Ten consecutive chains spent 4,601.49 ms in VIF1 processing;
DMA completion and handler bookkeeping remained in the microsecond range.
Within the same five-second window, 21,030 VU calls consumed 2,814.57 ms,
10,187 DIRECT submissions consumed 1,644.77 ms, and 75,160 UNPACK commands
consumed 139.335 ms. Evidence is
`build/phase3/phase4-v163-gameplay-transfer-profile-r8.log` and
`build/phase3/phase4-v163-gameplay-vif1-profile-r9.log`. Delayed opt-in DMA,
transfer, and aggregate VIF profilers were added so subsequent gameplay-only
measurements do not generate large startup traces.

The renderer now reuses a single computed CT32 destination address for the
read/modify/write blend operation. This reduced the dominant nearest CT32
alpha-sprite class from 410.430 ms to 370.113 ms per matched five-second
window (9.8%); the complete `PS2GS` suite remains 87/87. The serial T8
triangle palette cache is now lazy while parallel triangles retain an eager,
read-only palette; both modes are covered by full-VRAM differential tests.
Evidence is `build/phase3/phase4-v163-gameplay-ct32-address-profile-r10.log`,
`build/phase3/ps2x-tests-v163-ct32-single-address.log`, and
`build/phase3/ps2x-tests-v163-lazy-serial-t8-palette.log`. The latest live
profile shows that the T8 triangle class is still approximately 643 ms, so the
next action is to capture its exact GS state and remove only state checks and
memory-address work proven invariant for that live class.

That route measurement now proves the menu/movie-control interval sustains the
game's apparent 30 reads/s cadence through read 1300, but the first active 3D
scene falls to approximately 1.75 reads/s (read 1400 at 56.064 s, read 1500 at
113.697 s, and read 1600 at 170.990 s). Evidence is
`build/phase3/phase4-v163-multicore-route-timing-r1.log`. A dispatch profile
delayed into that exact scene attributes 9,561.26 ms of a ten-second window to
34 synchronous `sceDmaSend` calls—approximately 281 ms each—while all guest
and scheduler costs are small. Evidence is
`build/phase3/phase4-v163-gameplay-dispatch-profile-r2.log`.

A new delayed aggregate draw profiler avoids the severe timing distortion of
per-draw logs. In its five-second gameplay window the largest exact classes
are 30,520 linear T8 triangle-strip batches at 739.547 ms, CT32 alpha-blended
sprites at 669.715 ms (nearest) plus 412.102 ms (linear), and 4,985 blended T8
triangle strips at 218.690 ms. Evidence is
`build/phase3/phase4-v163-gameplay-gs-aggregate-r4.log`. The immediate code
target is the guarded CT32 sprite path: its live state has disabled alpha and
destination-alpha tests, ALWAYS depth test, masked depth writes, CT32 output,
and disjoint source/destination ranges, allowing the generic per-pixel state
dispatcher to be replaced with the same fixed-function blend arithmetic.

### Phase 4 v163 live-route and CT24 profiling checkpoint (2026-08-31)

The automated v163 save-route replay passed both restored garden vtable
families and completed every recorded input transition through read 3951.
Execution continued beyond read 4245 with DMA/GIF activity and no missing
target, unhandled RPC, memory-card, fatal, or exception diagnostic. Evidence is
`build/phase3/phase4-v163-save-route-r1.log`; the reusable exact input sequence
is `build/phase3/save-route-input-v1.csv`.

The route controller originally exited while its final up-right state remained
held. The CSV now contains a mandatory neutral release at read 4010. The
apparent persistent white output was subsequently disproved as a Windows
Graphics Capture/OpenGL-window artifact: internal frames show Fiona and the
staircase correctly through VSync tick 8375. The internally captured route was
encoded as the verified 70.6-second, 6,635,550-byte H.264 file
`UNEEDED images/Haunting-Ground-v163-gameplay-to-stairs.mp4`. Its 1,411 raw PPM
sources occupied 1,387,090,605 bytes and were truncated after successful full
decode verification, leaving zero payload bytes in the temporary capture
directory.

The internal presentation trace does reveal the actual outstanding flash:
thirty isolated exact-black presentations occur in the bounded tick-7600+
trace, eighteen of which landed on captured video frames. Representative
sequence `7649 nonblack -> 7651 zero -> 7652 nonblack` retains
`displayFbp=0`, `sourceFbp=0`, and `preferred=0` throughout. This rules out the
former page-272 source-selection regression and instead points to host latching
the displayed CPU VRAM between the game's clear and completed scene draw.
Evidence is `build/phase3/phase4-v163-staircase-video-r1.log`.

The fresh v163 profile also motivated a behavior-preserving CT24 rectangle
fill. The two guarded full-screen CT24 clear paths now call one rectangle helper
whose inner loop uses the same `PixelStorageTraits<C24>::Write` address and RMW
operation as the former per-pixel external calls. A differential test compares
the complete VRAM image against the old per-pixel path, including preservation
of destination alpha. Canonical and staged sources are synchronized. The
focused `PS2GS` suite passes 81/81 in
`build/phase3/ps2x-tests-v163-ct24-fill.log`. Reprofiling the same ten-second
window reduced `sceGsSwapDBuffDc@0x0010D200` from 601.42 ms to 561.402 ms
(approximately 6.7%) while `sceDmaSend@0x0010D6E8` remained effectively flat
at 1,409.24 ms. Evidence is
`build/phase3/startup-dispatch-profile-v163-ct24-fill.log`.

Deterministic replay now also accepts optional raw analog axes in each event as
`first-end:buttons:lx:ly:rx:ry`, retains the old button-only format, and returns
all axes to center outside event ranges. This removes the external-controller
held-state failure from future route replays. `PadInput` passes 18/18, including
valid analog progression plus malformed and out-of-range rejection, in
`build/phase3/ps2x-tests-v163-analog-replay.log`. The test target is current;
the full runner rebuild has now resumed after the already-linked CT24 runner
produced the user-requested staircase video.

The swap/presentation ordering correction is now implemented in canonical and
staged runtime sources. `sceGsSwapDBuffDc` and `sceGsSwapDBuff` latch the
completed display page after applying the selected display environment but
before submitting the next clear packet. The host window loop consumes that
explicit stable latch instead of independently snapshotting and converting the
entire 4 MiB GS local memory at every VSync. A focused regression seeds the
completed display page, proves the latch retains it, and proves the active draw
page is subsequently cleared. The regression initially failed because the
test had initialized memory without binding `PS2Runtime::gs()`; binding the
core subsystems corrected the fixture. `PS2GS` now passes 81/81 in
`build/phase3/ps2x-tests-v163-swap-latch.log`. Live zero-flash and timing
validation awaits the active runner link and deterministic replay.

Immediate actions are to finish the active runner link, replay the same route
with the self-contained analog specification, require zero isolated black
presentations, and reprofile both host presentation and guest GIF work. The
current GS backend is a synchronous CPU rasterizer; OpenGL presents the host
frame but does not yet rasterize guest GS primitives. A complete GPU backend is
therefore architectural work, while the current checkpoint first removes the
redundant full-VRAM host snapshots and preserves exact swap semantics.

### Phase 4 v163 component-family and fresh profiling checkpoint (2026-08-31)

The deterministic v152 route passed the former `0x00221B60` boundary, kept
DMA/GIF advancing, and exposed the next exact missing indirect target at read
4067: `0x002032BC -> 0x002AC5D0`. Typed scheduler evidence confirms the main
thread parked only after that missing call; this was not a scheduler-wake
inference. Evidence is `build/phase3/phase4-v152-save-route-r2.log`.

The live object's vtable at `0x0046DFC0` and its related tables at
`0x0046E000`, `0x0046E040`, and `0x0046E080` contain exactly five uncovered
uniform adapters. Exact verified-U.S.-ELF bounds now restore all five in one
map: `0x002AC5D0-0x002AC5F8`, `0x002AC760-0x002AC788`,
`0x002ACC60-0x002ACC88`, `0x002ACD20-0x002ACD48`, and
`0x002AD130-0x002AD158`. Each performs the demonstrated class-table selection
and delay-slot-aware tail call to mapped `0x00100B40`; no state or success
result is forced. The exact evidence is recorded as
`live_garden_component_adapter_family_v153` in
`config/functions.manual.toml`.

Because guarded generated retention found older numeric trees through v162,
the combined map was correctly renumbered to v163 rather than bypassing the
safety check. v163 processes 11,065 functions, recompiles 10,857 bodies, uses
208 runtime stubs and 390,114 resumable entries, and reports zero skips, decode
failures, unhandled instructions, correctness-critical guest fallbacks,
failures, or errors. The staged Windows runner links successfully. Retention
pruned two old generated trees and reclaimed 422,925,892 bytes.

A fresh ten-second v163 dispatch profile replaces the old v104 assumption.
`sceDmaSend@0x0010D6E8` remains the largest exclusive cost at 1,417.53 ms, down
from approximately 2,920 ms in the old profile. The new second-largest cost is
`sceGsSwapDBuffDc@0x0010D200` at 601.42 ms; its implementation executes the
selected GS clear packet, so the current startup cost remains concentrated in
synchronous GS/GIF rendering rather than pad input or file I/O. Evidence is
`build/phase3/startup-dispatch-profile-v163.log`. The immediate next action is
the exact automated save-route replay on v163, followed by a focused,
semantics-preserving investigation of the current GS clear/swap path.

### Phase 4 v152 garden-route boundary checkpoint (2026-08-31)

The verified New Game save-route replay now follows the exact PCSX2/video
garden path through the hydrant, overhead garden, fountain, and first staircase.
After Fiona crossed that staircase, the live v151 runner reached a genuine
unmapped indirect target rather than a movement or scheduler stall:
`0x002223BC -> 0x00221B60`. Evidence is
`build/phase3/phase4-v163-save-route-r4.log`.

Exact verified-U.S.-ELF inspection identifies `0x00221B60` as one of two
adjacent table adapters in the active table rooted at `0x0046C548`. A complete
family audit restored both adapters, their shared target, and the table's three
other uncovered executable members with delay-slot-aware bounds:

- `0x00221B40-0x00221B58`
- `0x00221B60-0x00221B78`
- `0x00222E40-0x00222F60`
- `0x00223520-0x0022362C`
- `0x00223630-0x002237D4`
- `0x002237E0-0x002238EC`

The exact evidence and rationale are recorded in `config/functions.manual.toml`
as `live_garden_route_vtable_family_v152`. v152 merged and recompiled cleanly:
11,060 functions processed, 10,852 recompiled bodies, 208 stubs, 390,114
resumable entries, and zero skipped functions, decode failures, unhandled
instructions, correctness-critical guest fallbacks, failures, or errors. The
generated output is staged with the ten-version retention policy. The Windows
runner's required full generated-source unity rebuild is active; the immediate
next action is to link it and replay the proven route with typed thread
snapshots enabled, then continue to the game-created save and separate-process
cold-boot reload gate.

### Phase 4 v162 SNDDRV crackle isolation and de-click checkpoint (2026-08-31)

The user's remaining intermittent crackle is now localized to the portable
audio callback rather than the decoded game stream. A direct-versus-presented
PCM comparison found that the guest-submitted PCM is continuous, while the
Raylib callback occasionally exhausts its queue for 192-672 frames and used to
insert a hard zero edge in both directions. In a 15-second baseline those edges
produced four sample jumps above 8,000 and a worst-channel jump of 11,041.
Gated `PS2X_SNDDRV_TIMING_TRACE` diagnostics prove the common exact case: a
4,544-frame submission was followed by 4,800 requested callback frames, leaving
a 256-frame underrun. Evidence is
`build/phase3/phase4-v162-audio-callback-timing-r8.log`.

Two pacing alternatives were tested and rejected rather than retained. Making
the callback eligible at the short IOP-copy delay, or one mailbox period before
nominal playback completion, changed the guest's refill batching and severely
stalled the later stream. The original asynchronous callback schedule is fully
restored. The output callback now applies a bounded 64-frame/1.33-ms linear
de-click ramp at each real-PCM-to-underrun edge and at the next resume edge.
This preserves the exact underrun duration and real-sample ordering without
repeating or synthesizing PCM.

The fresh 15-second output capture
`build/phase3/phase4-v162-audio-output-pcm-r13.s16le` has zero jumps above 8,000;
its maximum channel jumps are 3,473 and 4,252, versus 7,799 and 11,041 in the
same-length pre-fix baseline. The right-channel result now equals the clean
direct PCM's prior 4,252 maximum. The replay log is
`build/phase3/phase4-v162-audio-declick-r13.log`. Canonical and staged runtime
sources are byte-identical, both targets link, and focused suites pass:

- `PS2IopSubsystem`: 13/13 in
  `build/phase3/ps2x-tests-v162-snddrv-declick-iop-r13.log`
- `PS2RuntimeExpansion`: 41/41 in
  `build/phase3/ps2x-tests-v162-snddrv-declick-runtime-r13.log`
- `PS2GS`: 80/80 in
  `build/phase3/ps2x-tests-v162-gs-movie-fastpath-r13.log`

This removes the measured click-producing discontinuities but does not pretend
that the underlying short producer misses are gone; subjective confirmation
from a new review recording is still required. Phase 4 remains active, with
game-created save/cold-boot reload still the principal functional exit gate.

### Phase 4 v162 internal A/V proof capture and route-replay checkpoint (2026-08-31)

A fresh zero-skip natural-opening replay captured the runtime's own presented
frames every three VSync ticks rather than recording the Windows swap chain.
The corresponding SNDDRV output was captured from the same process and aligned
from its logged host timestamps: audio began 174.594 ms before the first saved
video frame. The resulting 23.95-second mobile proof is H.264 at 640x512/20 fps
with 48 kHz stereo AAC; the standalone review copy is 160 kb/s MP3. The encoded
audio measures -19.6 dBFS mean and -4.8 dBFS peak, so it is non-silent and does
not digitally clip. Evidence:

- `build/phase3/phase4-v162-internal-natural-movie-capture-r3.log`
- `build/phase3/phase4-v162-internal-natural-movie-audio-r3.s16le`
- `build/phase3/phase4-v162-internal-natural-movie-audio-mobile-r3.mp3`
- `build/phase3/phase4-v162-internal-natural-movie-av-mobile-r3.mp4`

The capture also adds gated, behavior-neutral diagnostics in canonical and
staged `ps2_runtime.cpp`: per-present nonblack/RGB-energy counters plus bounded
`PS2X_CAPTURE_FRAME_FROM_TICK`/`PS2X_CAPTURE_FRAME_TO_TICK` sequence capture
with a `{tick}` filename token. Internal frame-energy tracing found zero-valued
output only in contiguous fade spans, while desktop/window capture had inserted
additional isolated black frames. The new internal proof still contains short
near-black cuts according to FFmpeg's threshold detector, so visual correctness
must be judged against the corresponding natural PCSX2 sequence rather than by
assuming every dark cut is either valid or invalid.

The user revised the earlier audio assessment from broadly acceptable to
"close" but possibly distorted and requested a new review. That review remains
pending because local Codex workspace links are not reachable from the user's
mobile client; do not claim subjective audio acceptance until the new MP3/MP4
has been delivered through an explicitly authorized accessible channel and the
user confirms it.

For the save route, the next interactive run will be recorded as a deterministic
input profile: exact Cross pulses needed to advance dialogue, directional hold
durations, room/camera checkpoints, the grandfather-clock movie trigger, clothes
interaction/tutorial sequence, and the save interaction. This is intended to
replace repeated manual navigation while preserving observable game behavior.

### Phase 4 v162 asynchronous New Game gameplay checkpoint (2026-08-31)

A fresh deterministic replay exercised the real New Game route with the new
background `sceCdRead` implementation, then used the separately validated
Start/Triangle cutscene skip solely to reach the gameplay checkpoint quickly.
The loader completed through the asynchronous busy/completion path and reached
the corrected first playable ruins scene. At VSync 3,100 the renderer presents
Fiona and the coherent environment from `displayFbp=0`/`sourceFbp=0`; execution
continues to submit DMA/GIF work afterward without a storage deadlock, missing
target, or reappearance of the black-page substitution.

Evidence:

- `build/phase3/phase4-v162-async-storage-gameplay-r1.log`
- `UNEEDED images/v162-async-storage-gameplay-r1.ppm`
- `UNEEDED images/v162-async-storage-gameplay-r1.png`

Together with the existing natural 6,270-frame opening/EOF replay, every-30-
frame ordering evidence, the v162 black-frame regression fix, audible PCM
recording pending renewed user review, and the seven-checkpoint PCSX2 clock comparison
below, the Windows New Game, opening-video, and A/V exit checks are now marked
complete in `docs/ROADMAP.md`. The remaining Phase 4 functional gate is a
game-created save in the new portable location that reloads in a separate
process; oracle consolidation and unavailable Tier 1 host results follow it.

### Phase 4 v162 portable save root and movie-clock comparison (2026-08-31)

Memory cards no longer default beside the user-owned ELF. ELF-based runtime
configuration now selects a per-game directory under the host's standard user
data root: `%LOCALAPPDATA%` on Windows, `~/Library/Application Support` on
macOS, and `$XDG_DATA_HOME` or `~/.local/share` on Linux. The common suffix is
`PS2Recomp/<ELF filename>/mc0`, so this verified build uses
`PS2Recomp/SLUS_210.75/mc0`. `PS2X_MC_ROOT` remains an exact opt-in override for
portable installations and test isolation. Canonical and staged runtime trees
are synchronized. `PS2RuntimeIO` passes 15/15, including the new default and
override regression plus the existing byte-stable libmc cold-reinitialization
coverage, in `build/phase3/ps2x-tests-v162-portable-save-root.log`.

The natural opening trace now exposes the guest's presentation clock, 48 kHz
scale, and MPEG source position. The reproducible comparison utility
`tools/compare_movie_av_checkpoints.py` matches these against the existing
PCSX2 2.6.3 PINE oracle. Across seven source checkpoints from `0xC0000` through
`0x180000`, native is 27.229-93.896 ms ahead of the oracle and every checkpoint
passes the documented 100 ms tolerance. Evidence:

- `build/phase3/phase4-v162-natural-av-state-r1.log`
- `build/phase3/oracle-v160-cathedral-through-second-stream-r1.jsonl`
- `build/phase3/phase4-v162-av-oracle-comparison.log`

This is objective A/V-clock evidence over the advancing real opening, and the
user initially described the intro-to-gameplay audio as broadly acceptable but
later requested a fresh review because it may still sound distorted. The
existing v160 zero-skip replay already demonstrates natural video
progress through frame 6,270 and EOF; the v162 presentation correction and
late-movie trace remove the subsequently identified black-frame substitution.
Phase 4 remains active pending a game-created save and separate-process reload,
plus the remaining exit-check consolidation and Tier 1 host results.

### Phase 4 v162 black-frame presentation regression fixed (2026-08-31)

User review of the intro-to-gameplay proof recording found repeated random
flashes while reporting that the reconstructed stereo audio sounded correct.
Full-frame signal analysis of both the final encode and original 60 Hz OBS
capture proved the encode was not responsible: the source contains isolated
exact-black frames interspersed through otherwise continuous movie images.
The one-second contact sheet at source time 210.2 seconds is
`UNEEDED images/phase4-v162-flash-window-210s.png`.

An internal host-frame capture then proved this was a runtime presentation
defect rather than Windows capture behavior. With `DISPFB1` still selecting
page 0 and `PMODE=0x8005`, the host presenter intermittently reported
`sourceFbp=272`, producing an almost-black buffer with stray texture data in
`UNEEDED images/phase4-v162-internal-tick1198.png`. The cause was the generic
CPU-presenter heuristic that replaced a legitimately black page-0 display with
the first nonblack draw-context framebuffer. This is the same invalid page-272
fallback already identified and removed from the disposable v93/v100 staging
work, but it had remained in the canonical runtime and was later synchronized
back into the staged tree.

Canonical and staged `gs_cpu_backend.cpp` now honor the registered DISPFB page
when no exact tracked full-screen-copy source is active. The valid preferred
source path used by movie copies remains intact. A new regression constructs a
black page-0 DISPFB and a nonblack page-272 draw context: it failed before the
fix and now passes. Focused `PS2GS` is 80/80 in
`build/phase3/ps2x-tests-v162-gs-black-dispfb-fixed.log`; the pre-fix failure is
retained in `build/phase3/ps2x-tests-v162-gs-black-dispfb-before-fix.log`.
Both runner and test targets link successfully. A post-fix live trace records
512/512 presentations with `sourceFbp=0` and zero page-272 substitutions:
`build/phase3/phase4-v162-flash-presentation-fixed-r1.log`.
Because the user-observed flashes were most conspicuous later in the opening,
a second natural replay omitted Start/Triangle and traced VSync ticks
8,000-8,511. All 512 late-movie presentations again remain exactly
`displayFbp=0`, `sourceFbp=0`, `preferred=0`; 476 distinct frame hashes prove
the check covered advancing imagery rather than a stalled frame. Evidence:
`build/phase3/phase4-v162-flash-late-movie-fixed-r1.log`.

The audio report is useful subjective evidence for the PCM reconstruction but
does not replace PCSX2 A/V-timing validation. The next action is a natural
post-fix movie capture/oracle timing comparison, followed by the remaining
ordinary asynchronous-read and cold-boot save gates. Phase 4 remains
incomplete.

### Phase 4 v162 ordinary CD command deferral checkpoint (2026-08-31)

The ordinary EE libcdvd path no longer exposes read data synchronously. A
runtime-backed `sceCdRead` stages the selected user-owned DATA.CVM/disc range,
keeps the command busy for the deterministic DVD transfer interval, publishes
the guest bytes only from the scheduled completion, wakes a blocking
`sceCdSync(0)`, and reports busy through nonblocking `sceCdSync(1)`. Registered
`sceCdCallback` handlers are queued with `SCECdFuncRead` only after that
completion. Reinitialization and `sceCdBreak` invalidate the exact pending
generation rather than allowing stale completion data to land later.

Focused `PS2RuntimeIO` coverage is 14/14 in
`build/phase3/ps2x-tests-v162-async-cd-read.log`. The regression proves that
the destination remains untouched after command acceptance and that the
nonblocking synchronization call reports an active read. A live New Game-route
dispatch trace then proved this is the actual Haunting Ground loader path, not
an unused generic service: `sceCdRead@0x00110380` receives hundreds of DATA.CVM
requests from `0x001D9100`, and the corresponding `sceCdSync@0x0010F948`
sequence reports busy before completion. The 80-second replay continued beyond
9,000 DMA submissions and 2,500 GIF packets without a missing target or read
deadlock. Evidence:
`build/phase3/phase4-v162-cd-binding-trace-r1.log`.

The physical host read now also runs behind the pending operation on a detached,
ownership-bounded worker. The worker receives only an immutable resolved host
path/range and private byte vector; it never touches guest memory, CD globals,
or scheduler state. A release/acquire completion flag publishes the result, and
the EE scheduler waits for both the deterministic media deadline and host-I/O
completion before it copies bytes or wakes the guest. Cancellation leaves a
retired worker harmless because its generation can no longer publish.

Both Windows targets rebuild successfully. `PS2RuntimeIO` remains 14/14 in
`build/phase3/ps2x-tests-v162-background-cd-read.log`, and the flash regression
remains covered by `PS2GS` 80/80 in
`build/phase3/ps2x-tests-v162-flash-regression-after-cd.log`. A fresh live
background-I/O replay preserves the observed `sceCdRead`/busy/busy/complete
sequence and advances beyond 4,800 DMA submissions and 1,400 GIF packets with
no loader deadlock: `build/phase3/phase4-v162-background-cd-live-r1.log`.
Ordinary `sceCdRead` now meets both host and guest asynchronous semantics on the
demonstrated Haunting Ground path. Phase 4 remains incomplete pending natural
movie A/V oracle timing and game-created save/cold-boot validation.

### Phase 4 v162 SNDDRV PCM reconstruction and asynchronous pacing (2026-08-31)

The Haunting Ground SNDDRV playback refill path now reaches a real portable
stereo output stream. Live transfer tracing separates two previously conflated
address spaces: voice-transfer RPC payloads are Sony ADPCM sample-bank data
written to SPU2 RAM, while the paired opcode-`0x100` playback cursors address
guest-visible IOP/SIF rings. Reading the latter through `IopHost::readGuest`
produces nonzero channel data. Format classification rejects Sony ADPCM for
those rings (only random-rate header plausibility and severe clipping after
decode) and identifies little-endian PCM16: early raw peaks progress naturally
from 9 to 1,133, 2,535, and higher without decoding. At the configured
`0xBB80` rate this is 48 kHz stereo PCM.

Canonical and staged runtime trees now share a bounded Raylib `AudioStream`
path that interleaves the two PCM16 channel rings. A reusable Sony ADPCM block
decoder remains in place for actual VAG/sample-bank content and has a split-
versus-contiguous predictor-history regression. The host seam returns each
refill's exact playback duration. Completion is scheduled on the EE scheduler
rather than acknowledged synchronously or blocked on wall time: an early next
mailbox transfer is retained, the prior callback generation becomes ready
after the scheduled sample duration, and the deferred transfer is then
reprocessed. This fixes both the original thousands-of-refills-per-second
runaway and the first wall-clock-delay attempt's one-chunk producer deadlock.

Focused regressions pass after the asynchronous correction:

- `PS2IopSubsystem`: 13/13 in
  `build/phase3/ps2x-tests-v162-snddrv-async-pacing.log`
- `PS2RuntimeExpansion`: 41/41 in
  `build/phase3/ps2x-tests-v162-async-audio-runtime.log`

The live replay
`build/phase3/phase4-v162-snddrv-pcm-paced-async-r1.log` advances beyond 1,400
refill submissions without a deadlock or multi-second queue. A diagnostic
30-second, 48 kHz stereo capture is exactly 1,440,000 frames; FFmpeg reports
peak -4.44 dBFS and RMS -18.34 dBFS, proving non-silent, non-clipped content:

- `build/phase3/snddrv-v162-pcm-paced-async-first30s.s16le`
- `build/phase3/snddrv-v162-movie-sample-30s.wav`
- `build/phase3/snddrv-v162-movie-sample-30s.mp3`

This establishes decoded stream content and bounded scheduling, not yet
oracle-validated A/V synchronization. The immediate action is to record the
natural New Game opening through the first playable scene with matching video
and output audio, compare movie-frame/audio cadence against PCSX2, and then run
the requested fresh startup dispatch/GS profile. Phase 4 remains incomplete;
ordinary asynchronous reads and game-created cold-boot save reload are still
open after movie/audio validation.

### Phase 4 keyboard steering and black-shadow triage (2026-08-31)

The first playable ruins area now accepts direct keyboard control without
editing the diagnostic pad file. `ps2_pad.cpp` keeps the arrow keys as the
digital D-pad, maps WASD to the full-range left analog stick, and retains the
existing face/shoulder/Start/Select mappings. Keyboard input is no longer
disabled merely because a gamepad is connected. The opt-in environment
`PS2X_PAD_ALLOW_KEYBOARD_WITH_REPLAY` also merges keyboard state with the
deterministic replay, allowing a repeatable boot followed by interactive
steering. Canonical and staged sources are byte-identical, both runner and test
targets link, and focused `PadInput` regressions pass 17/17 in
`build/phase3/ps2x-tests-v162-keyboard-pad.log`.

The user-identified tall black character-shaped ray in the second exterior
camera is confirmed absent from the walkthrough/PCSX2 presentation and remains
a rendering defect. A focused v162 experiment replaced the explicitly
non-bit-exact host `sceVu0DropShadowMatrix` with the exact verified-ELF owner
`0x0010E3C0-0x0010E550`; the generated build was clean (11,055 functions,
10,847 recompiled bodies, 208 bindings, 390,107 resumable entries, and zero
critical report counts). However, a source-PC trace through the first gameplay
scene recorded no entry to `0x0010E3C0`, disproving that binding as the source
of the visible ray. The speculative config change was therefore removed rather
than retained. Evidence:

- `build/phase3/phase4-v162-exact-shadow-entry-trace-r1.log`
- `build/phase3/phase4-v162-exact-shadow-replay-r1.log`
- `UNEEDED images/v162-shadow-exact-matrix-r1.png`

Interactive UI automation can deliver discrete key presses but not sustained
key-down state reliably enough for this slow input loop; the new keyboard path
is still directly usable by a person, while deterministic/live-state input
remains available for held diagnostic movement. The immediate graphics action
is to reproduce the exact second-camera frame with the known movement sequence,
capture its PATH1 shadow packet, and compare its primitive/vertex and GS state
against PCSX2. Phase 4 remains incomplete.

### Phase 4 v161 exact GS image-upload owners restore Fiona (2026-08-31)

A focused first-scene dispatch trace supersedes the provisional conclusion
that the guest never submits Fiona's palettes. Owner `0x001BFF30` calls
`0x001C02A0` six times for the complete character texture/palette set. The
first three calls carry palette sources `0x00899B80`, `0x008A9F80`, and
`0x008BA380`; the next three carry `0x00919B80`, `0x00929F80`, and
`0x0093A380`. Those are the exact already-loaded sources identified by the
VRAM differential. Evidence:
`build/phase3/phase4-v160-first-scene-resource-upload-calls-r1.log`.

The first divergent layer is therefore the two host SDK bindings, not the
camera, actor movement, disc loading, or guest upload scheduler. Verified-ELF
owner `0x0010CCB8-0x0010CE9C` constructs a complete 0x60-byte GIF/DMA image
packet in guest memory, whereas the host `sceGsSetDefLoadImage` binding wrote a
different 12-byte private descriptor consumed only by the paired host
`sceGsExecLoadImage` binding. v161 removes both bindings and recompiles the
complete verified-ELF `sceGsSetDefLoadImage` and analyzed
`sceGsExecLoadImage@0x0010CEA0-0x0010D01C` owners, allowing their exact packet
and DMA behavior to run through the existing runtime. This is not a palette
injection or state force.

v161 standalone recompilation passes with 11,054 functions, 10,846 recompiled
bodies, 208 runtime bindings, 390,107 resumable entries, and zero skips,
decode failures, unhandled instructions, correctness-critical fallbacks,
failures, or errors. Generated output is staged with the newest-ten retention
guard. The Windows unity build and link complete successfully.

Two v161 replays prove the exact replacement is live. The first traces all 12
operations (six image uploads and their six palette uploads) entering and
returning from the recompiled `0x0010CCB8`/`0x0010CEA0` owners without a missing
target or DMA deadlock. The second captures the same first-scene route at VSync
3,100. Fiona is visibly present in the coherent ruins scene, and the previously
zero palette pages are populated:

- `0x003F3800`: 1,022 nonzero bytes, SHA-256
  `68877bb0ed9cc0ea1b08f1f257c1948061a5360fdfb4b7415a24dad926b7f060`
- `0x003F3C00`: 1,021 nonzero bytes, SHA-256
  `d21f1ac3fb6e4c984d31a738422661af2167af42038b5fda6b476a76de0db538`
- `0x003F4000`: 1,022 nonzero bytes, SHA-256
  `fefcf6d2f5fafda6811a87fcacd5ff4f04b02ab2900e84f38e6ebcb723b4484d`

Evidence:

- `build/phase3/phase4-v161-first-scene-exact-loadimage-r1.log`
- `build/phase3/phase4-v161-first-scene-exact-loadimage-capture-r1.log`
- `build/phase3/native-v161-first-scene-exact-loadimage-vram-r1.bin`
- `UNEEDED images/v161-first-scene-exact-loadimage-r1.png`

Focused regressions pass: `PS2Memory` 52/52 and `PS2RuntimeExpansion` 40/40 in
`build/phase3/ps2x-tests-v161-ps2memory.log` and
`build/phase3/ps2x-tests-v161-runtime-expansion.log`. The broad `GS` filter
matched no suite and is not counted as evidence. The diagnostic scene route
uses Start/Triangle to skip movies, so it does not establish natural movie/A-V
behavior. Phase 4 remains incomplete. The next action is to validate real
movement and the first room transition from this corrected scene, then continue
the remaining natural movie/audio, asynchronous-read, and cold-boot save gates.

### Phase 4 v160 Fiona palette-upload blocker localized (2026-08-31)

The corrected ruins scene retains the same camera and background composition as
the PCSX2 2.6.3 oracle, and all 1,519 current native PATH1 geometry packets can
render Fiona correctly when replayed with the oracle's GS resource state. This
rules out a missing actor draw, movement failure, or camera-transition defect.
The remaining visual failure is exact: Fiona's indexed draws select T8H texture
bases `0`, `1024`, and `2048` with CT32 palettes at CBP `16184`, `16188`, and
`16192`, while native GS VRAM pages `0x003F3800`, `0x003F3C00`, and
`0x003F4000` are zero. Replacing only those oracle palette bytes with native
zeros reproduces Fiona's complete disappearance in the otherwise-correct oracle
frame:

- `build/phase3/hybrid-v160-native-path1-native-fiona-clut.fixture`
- `UNEEDED images/hybrid-v160-native-fiona-clut-frame3.png`

A fresh post-long-chain PATH2 capture confirms this is current behavior rather
than stale pre-fix evidence. The one-time scene resource burst at wall time
approximately 52.443-52.648 seconds uploads CT32 palette pages only through CBP
`16168`; there are no transfers to CBP `16184`, `16188`, or `16192`, and the
fresh 4 MiB VRAM snapshot keeps all three pages zero. Evidence is:

- `build/phase3/phase4-v160-first-scene-post-long-chain-path2-r1.bin`
- `build/phase3/native-v160-first-scene-post-long-chain-vram-r1.bin`
- `build/phase3/phase4-v160-first-scene-transition-path2-r1.bin`

The source asset data is not missing. Unswizzling the oracle palettes back to
their linear 16-by-16 CT32 upload form finds byte-exact copies in native EE RAM
at `0x00899B80`, `0x008A9F80`, and `0x008BA380`; the next three omitted palette
sources are likewise present at `0x00919B80`, `0x00929F80`, and `0x0093A380`.
Thus disc/file loading is not the cause. Disabling the GIF native image and
packed-chain fast paths also leaves the same pages zero and Fiona absent, ruling
out those optimized PATH3 handlers:

- `build/phase3/phase4-v160-first-scene-generic-gif-r1.log`
- `build/phase3/native-v160-first-scene-generic-gif-vram-r1.bin`
- `UNEEDED images/v160-first-scene-generic-gif-r1.png`

The immediate blocker is the guest-visible resource-upload scheduling/state
that never submits these already-loaded palettes. The next action is to trace
the per-frame VIF1/GIF upload builder and its completion/cache state at the
52.443-second burst, compare the governing fields against PCSX2, and correct
only the first demonstrated runtime divergence. Do not inject oracle palette
bytes or force GS VRAM writes. Phase 4 remains incomplete.

### Phase 4 v160 long VIF1 DMA-chain correction restores complete scene geometry (2026-08-30)

A fresh post-fix PATH1 capture localized the remaining first-scene corruption
to exactly two omitted geometry packets per steady frame. PCSX2's oracle frame
contains 1,519 PATH1-style geometry packets. Native emitted 1,517 in three
batches of 568, 582, and 367 packets; the missing oracle packets were index 568
(1,360 bytes) and index 1,151 (1,072 bytes), exactly at the ends of the first
two large VIF1/VU1 batches.

The retained scene RDRAM dump proves the cause. Each large VIF1 source chain is
11,798 valid DMAtags long, but the generic DMA-chain flattener silently stopped
after an arbitrary 4,096-tag ceiling. In the first chain, tag index 4,096 at
`0x00700440` carries `MSCNT` in its transferred upper 64 bits; the old loop
stopped after index 4,095, suppressing the final VU1 continuation and XGKICK.
The second large batch failed through the same boundary, while the shorter
third batch was unaffected.

Canonical and staged `ps2_memory.cpp` now traverse a finite source chain to its
architectural terminator and guard malformed loops by detecting an exact
repeated state (`TADR`, `ASR0`, `ASR1`, and `ASP`) instead of truncating valid
chains. A focused regression places `MSCNT` beyond 4,096 valid tags. Verification:

- `PS2Memory`: 52/52 passing in
  `build/phase3/ps2x-tests-v160-dma-long-chain.log`
- `PS2GS`: 79/79 passing in
  `build/phase3/ps2x-tests-v160-long-dma-chain-gs.log`
- `PS2RuntimeExpansion`: 40/40 passing in
  `build/phase3/ps2x-tests-v160-long-dma-chain-runtime.log`

The rebuilt zero-boundary replay now emits 1,519 packets per steady frame in
batches of 569, 583, and 367. From the first stable frame onward, all 1,519
packet sizes and GIFtag words match the PCSX2 fixture with zero structural
differences. Evidence is:

- `build/phase3/phase4-v160-first-scene-long-dma-chain-fix-r1.bin`
- `build/phase3/phase4-v160-first-scene-long-dma-chain-fix-r1.log`
- `UNEEDED images/v160-first-scene-long-dma-chain-fix-r1.png`

The captured native frame is a coherent textured ruins scene; the prior heavy
fragmentation is gone. This replay used Start and Triangle to skip movies and
therefore is diagnostic scene evidence only, not natural movie/A-V evidence.
Phase 4 remains incomplete. The next actions are to advance from the corrected
New Game scene to the first controllable gameplay checkpoint, then validate the
two opening movies without skip input and compare their audio/video clocks to
PCSX2 before completing asynchronous ordinary reads and game-created save/cold-
boot reload gates. No runner or PCSX2 process is active.

### Phase 4 v160 exact `sceGsPutDrawEnv` binding and remaining scene-stream divergence (2026-08-30)

The runtime `sceGsPutDrawEnv@0x0010C8B8` binding incorrectly treated the guest
draw-environment buffer as eight raw register pairs. Verified-ELF disassembly
shows that the buffer begins with a GIFtag and that the SDK helper submits the
complete tag-plus-payload packet. Canonical and staged runtime sources now
validate the guest range, decode `NLOOP`, and submit exactly `(NLOOP + 1) * 16`
bytes through the normal GIF parser. A direct packet regression passes as part
of `PS2GS` 79/79 in:

- `build/phase3/ps2x-tests-v160-putdrawenv-r1.log`

The deterministic replay proves the corrected binding is live, but its VSync
3,650 image remains the same coherent night sky with a mostly black foreground:

- `UNEEDED images/v160-first-scene-putdrawenv-r1.png`
- `build/phase3/phase4-v160-first-scene-putdrawenv-r1.log`

Raw VRAM inspection shows that native FBP `0x000` contains the sky and a dark
foreground while FBP `0x110` contains landscape and tree content in its lower
portion. This initially suggested a double-buffer/display-page error. A fresh
PCSX2 2.6.3 PINE oracle run disproved that hypothesis: the live double-buffer
object at `0x007F1FF0`, including display packets `0x00001400`/`0x00009400` and
draw-page fields `0x00080110`/`0x010A0000`, matches native byte-for-byte. No
display-page override is warranted.

A later native capture at VSync 4,300 shows continued scene evolution and much
more landscape/tree geometry, but the composition remains fragmented and does
not match the oracle Fiona/ruins scene:

- `UNEEDED images/v160-first-scene-late-r1.png`
- `build/phase3/phase4-v160-first-scene-late-r1.log`

The remaining visual blocker is therefore upstream in the current VIF1/VU1 GIF
stream rather than the GS backend or page selection. The next action is a fresh
post-fix PATH1 packet capture segmented at frame/swap boundaries, followed by
comparison with the oracle's 2,542-packet frame. The comparison will identify
where native truncates, restarts, or diverges after the previously aligned 568
geometry packets. Phase 4 remains incomplete; natural A/V synchronization,
genuinely asynchronous reads, game-created saves with separate-process cold
reload, and Tier 1 platform verification are still open. No runner or PCSX2
process is active.

### Phase 4 v160 exact `sceGsSetDefClear` binding removes heavy scene corruption (2026-08-30)

The post-opening white/gray diamond was traced below packet ordering and host
presentation to GS local-memory state. A one-shot native VRAM capture at VSync
3,650 showed the active Z24 buffer at ZBP `0x0A0` uniformly initialized to
`0xFFFFFF`. Replaying the PCSX2 scene fixture through the same CPU GS backend
produces the correct ruins/Fiona image; its initial depth buffer is zero and its
four replayed frames populate 53,482 distinct depth values between `0x000000`
and `0x032189`. With the game's greater-depth test, the native all-maximum
buffer rejected essentially all scene geometry.

The actual cause was the empty runtime binding for verified-ELF function
`sceGsSetDefClear@0x0010C7B0`. The guest `sceGsSetDefDBuffDc` implementation
calls this bound SDK helper to construct its six A+D clear pairs. Leaving the
96-byte object uninitialized preserved a maximum Z value in both clear
vertices. The original 260-byte U.S.-ELF routine was disassembled directly and
the binding now reproduces its exact register/stack ABI, signed rectangle
coordinates, RGBA packing, stack-supplied 32-bit Z value, TEST disable/restore
pairs, register IDs, and return value `6`. Canonical and staged sources are
synchronized, and a byte-exact regression passes as part of `PS2GS` 78/78 in:

- `build/phase3/ps2x-tests-v160-setdefclear-r1.log`

The exact deterministic replay now renders a coherent textured night sky at
the former corruption checkpoint with no white diamond or yellow corners:

- `UNEEDED images/v160-first-scene-setdefclear-r1.png`
- `build/phase3/phase4-v160-first-scene-setdefclear-r1.log`

The foreground remains mostly black instead of matching the full PCSX2 ruins
scene, so the visual gate is improved but still open. The immediate next action
is a second post-fix VRAM capture at the same tick, comparison of active frame
FBP `0x110` and ZBP `0x0A0` against the oracle fixture's final VRAM, and then a
trace of the first remaining frame/depth divergence. Phase 4 remains
incomplete; natural A/V synchronization, asynchronous reads, game-created
saves with separate-process cold reload, and Tier 1 platform validation remain
open. No runner or PCSX2 process is active.

### Phase 4 v160 GIF stream reassembly verified; earlier scene setup now targeted (2026-08-30)

The corrected libvu0 matrix order eliminates the former VU1 overflow: a fresh
post-fix replay produced zero `vu1-bad-stq` events, and the first 568 native
PATH1 geometry packets align structurally with the PCSX2 oracle with STQ
differences around `1e-9` to `1e-8` and XYZ differences of at most one integer
unit. Geometry generation is therefore no longer the active visual blocker.

The next exact stream defect was that PATH2 DIRECT submissions can end after a
complete GIF packet plus the 16-byte tag of the following packet. The GS parser
is submission-local, so it consumed that tag and discarded the following
payload. `GifArbiter` now retains incomplete streams independently per GIF path
and emits only complete tag-plus-payload prefixes. The VIF1 interpreter feeds
the arbiter the exact continuation bytes instead of applying its legacy
synthetic-IMAGE-tag workaround. Canonical and staged runtime sources are
synchronized. `PS2Memory` passes 51/51, including a byte-exact split-PACKED
regression and both existing IMAGE continuation cases, and `PS2GS` passes
77/77. Logs are:

- `build/phase3/ps2x-tests-v160-gif-stream-reassembly-r2.log`
- `build/phase3/ps2x-tests-v160-gif-stream-reassembly-gs-r1.log`

The zero-skip deterministic route still renders the white/gray diamond at the
same VSync-3,650 comparison point in
`UNEEDED images/v160-first-scene-gif-reassembly-r1.png`, so this correction is
necessary but not sufficient. A post-arbiter capture proves the repair is live:
all 141,520 emitted GIF tags are structurally complete, with zero malformed
records, and the formerly absent 208-byte `0x100000000000800c` PACKED block is
now delivered 12,432 times. The delivered tag topology closely matches the
oracle; the remaining likely divergence is one-time PATH2 resource/setup state
submitted immediately after the Start-then-Triangle movie skip, before the
first delayed capture window. An opt-in `PS2X_GIF_ARBITER_DUMP` now supports
delay, duration, byte-limit, and path filters so that this boundary can be
captured without retaining hundreds of MiB of already-validated PATH1 data.

The immediate next action is a PATH2-only post-arbiter capture beginning at the
movie-to-scene transition (approximately 48 seconds), followed by an exact
comparison of the initial A+D and IMAGE setup sequence against
`build/phase3/oracle-v160-first-scene-gs-replay.fixture`. Phase 4 remains
incomplete; natural movie A/V validation, asynchronous reads, game-created
saves with separate-process cold reload, and Tier 1 platform validation remain
open. No runner or PCSX2 process is active.

### Phase 4 v160 libvu0 matrix-order correction; post-opening corruption still open (2026-08-30)

An isolated PCSX2 2.6.3 slot-1 oracle checkpoint now provides the exact first
ruins scene without relying on the previously misrouted title demonstration.
Targeted PINE dumps are:

- `build/phase3/oracle-v160-first-scene-chain-r1.bin`
- `build/phase3/oracle-v160-first-scene-s1-r1.bin`
- `build/phase3/oracle-v160-first-scene-s4-r1.bin`

The native and oracle scene objects have the same vtables (`0x00469A60` and
`0x0046C770`) and the same object transform at `0x00FC72C0+0x90`:
identity plus translation `(128.053375, 56.732971, 12.991027)`. Camera-object
inputs through `0x01961500+0x14C` are identical apart from one-to-four ULP
rounding differences, while the cached matrix beginning at `+0x150` diverges
sharply. Direct multiplication against the oracle output proves the libvu0
ABI order: `sceVu0MulMatrix(dst, arg1, arg2)` must calculate `arg2 * arg1`.
The previous runtime calculated `arg1 * arg2`; its aggregate absolute error
against the oracle matrix is approximately 4.38 million, versus approximately
1.57 from single-precision evaluation in the corrected order.

Canonical and staged `VU.cpp` now use `mulVuMatrix(m1, m0, out)`, and the
non-commuting unit test explicitly verifies argument-2-times-argument-1. Both
Windows targets rebuild and `PS2VU0Math` passes 45/45 in
`build/phase3/ps2x-tests-v160-vu-mul-order-r1.log`.

This correction is necessary but does not by itself clear the visual blocker.
The same zero-skip replay captured tick 3,600 to
`UNEEDED images/v160-post-opening-mul-order-fix-r1.png`; it still shows the
white/gray diamond and yellow corners. The next action is to capture the
corrected live VIF1 chain and compare its matrix words byte-for-byte with the
oracle dump. If those now match, continue at the already-localized VU1 PATH1
STQ producer; if they do not, trace the first remaining matrix-chain
divergence. Phase 4 remains incomplete. No runner or PCSX2 process is active.

### Phase 4 v160 post-opening renderer exclusion and VU1 transform blocker (2026-08-30)

The corrected Start-at-440, Start-at-520, Cross-at-600 route reaches the real
New Game opening. Start at read 1,255 followed by Triangle at 1,263 skips that
movie, but the following in-engine scene is rendered as a stable white/gray
diamond with yellow corners instead of the PCSX2-oracle ruins scene. The guest
continues running and repeatedly submits scene work; this is not a hang or a
movie-decoder regression.

A portable PCSX2 2.6.3 GS dump of the correct first ruins scene was converted
to the local `P2GSR001` replay fixture and replayed through the native CPU GS
backend. `PS2GS` passes 78/78 and the native replay produces the correct Fiona
and ruins frame in `UNEEDED images/oracle-v160-replayed-native-frame3.png`.
The original dump is
`build/pcsx2-oracle/snaps/Haunting Ground_SLUS-21075_20260830155111.gs.zst`;
the ignored local fixture is
`build/phase3/oracle-v160-first-scene-gs-replay.fixture`. This directly excludes
the GS renderer and presentation-page selection as the corruption source.

An opt-in native GIF packet dump then captured 65,242 loader packets. More than
20,000 packets byte-match the oracle stream and the control/resource topology
matches at roughly 0.79 sequence ratio, but native VU1 PATH1 geometry contains
invalid STQ values up to `FLT_MAX`; the oracle geometry contains none. The
first bad-packet micro trace identifies the common producer precisely:
`XGKICK` issues at VU1 micro-PC `0x270`, the packet finishes at `0x288`, and
the transformed Q component has saturated. Disassembly of the dumped 16 KiB
microprogram shows that `DIV` computes `1.0 / transformed_w`; the bad packets
are therefore downstream of a zero transformed-W input, not GS interpretation.
Evidence is:

- `build/phase3/phase4-v160-post-opening-gif-packets-loader-r1.bin`
- `build/phase3/phase4-v160-first-scene-bad-stq-trace-r1.log`
- `build/phase3/v160-first-scene-bad-stq-vu1-code.bin`

The next verification is narrowly scoped to live VIF1 UNPACK formats feeding
this microprogram. Comparison with PCSX2's hardware-validated VIF unpack path
found that the runtime currently leaves absent V2/V3 lanes stale, whereas PS2
V2 writes `xyxy` and V3 follows the V4 unpack path for its indeterminate W
lane. That mismatch can produce the observed zero transformed-W values. An
opt-in, behavior-neutral `PS2X_VIF1_UNPACK_TRACE` is being used to prove the
exact live format before changing semantics. Phase 4 remains incomplete;
audio synchronization, asynchronous reads, game-created saves/cold reload,
and Tier 1 host validation remain open after this visual blocker.

### Phase 4 v160 exact SNDDRV callback-pair correction and complete opening progression (2026-08-30)

The corrected real-menu route is now demonstrated end to end: bounded boot
movie skips through controller read 210, Start at read 440 to leave the
Demonstration attract sequence, Start at read 520 to enter the real menu, and
Cross at read 600 to select New Game. No Start or Triangle input occurs after
Cross in the natural validation run.

The remaining real-opening frame-0 stall was caused by two exact defects in the
game-scoped SNDDRV transfer service. First, the service retained the initial
descriptor pair's audio-ring bases after the guest switched handles. Second,
it incorrectly returned the same callback objects for both descriptor pairs.
A guest read/write trace established the complete handle table:

- `0x01F20020` / `0x01F20060` map to callback records
  `0x003D4DD8` / `0x003D4DEC`, whose queue objects are
  `0x003D3FE0` / `0x003D4010`.
- `0x01F200A0` / `0x01F200E0` map to callback records
  `0x003D4E00` / `0x003D4E14`, whose queue objects are
  `0x003D4040` / `0x003D4070`.

Canonical and staged runtime/profile code now accepts only those two exact
handle pairs, keeps ring bases scoped to the active handles, permits a rebase
only when no callbacks are pending, and emits the corresponding pair's callback
records. Unknown handles and same-handle out-of-window requests remain rejected.
`PS2IopSubsystem` passes 13/13, including ordinary, wrapped, pair-switch, and
rejection coverage, in
`build/phase3/ps2x-tests-v160-snddrv-callback-pairs.log`.

The zero-skip-after-Cross replay
`build/phase3/phase4-v160-snddrv-callback-pairs-replay-r1.log` proves that the
real opening is no longer stuck at frame 0. MPEG stream 2 advances naturally
through frame 6,270, approximately 3 minutes 29 seconds at 30 fps, before the
decoded stream ends. There are 210 every-30-frame captures under
`UNEEDED images/v160-snddrv-callback-pairs-every30-stream2-frame*.ppm`.
Representative frames 300 and 3,000 are visually coherent and frame 6,270 is
the expected terminal black frame; no heavy corruption is present in those
checkpoints. The runner was stopped after the stream ended and no process is
active.

This closes the demonstrated real-opening frame-retirement stall, not Phase 4.
Audible output and A/V synchronization still require direct validation, the
post-opening state must be classified and advanced into interactive gameplay,
ordinary disc/file reads remain synchronous, and game-created save plus
separate-process cold-boot reload validation remains open. The immediate next
action is a diagnostic replay that uses the user-confirmed Start-then-Triangle
movie skip only after stream 2 begins, captures the post-opening screen/state,
and continues the New Game path without waiting through the full movie.

### Phase 4 v160 Demonstration-route correction and stopped checkpoint (2026-08-30)

Visual movie corruption remains corrected, but a final contact-sheet review
disproved the latest route classification: the post-boot streams carry the
on-screen `Demonstration` label and are the attract-mode chain, not the real New
Game intro. The earlier bounded boot replay with Cross at read 260 must not be
used as New Game evidence. A requested every-five-decoded-frame progress video
was verified as a 640x448 H.264 MP4 (50.466 seconds, 1,514 frames, 3,586,160
bytes) at
`UNEEDED images/haunting-ground-v160-progress-every5.mp4`. It shows cold launch,
an inserted known menu checkpoint, and the clean Demonstration streams; it is
silent and is not A/V-sync evidence.

The portable pad diagnostic bridge now also accepts the gated
`PS2X_PAD_LIVE_FILE` option. A numeric active-high mask remains supported;
`pulse:<buttons>:<generation>` emits an exact three-read pulse, and
`skip:<generation>` emits the user-confirmed three-read Start pulse followed by
neutral reads and a three-read Triangle pulse. Canonical and staged
`ps2_pad.cpp` are synchronized, both Windows targets link, and `PadInput` passes
16/16 in `build/phase3/ps2x-tests-v160-pad-live-pulse.log`. The neutral control
file is `build/phase3/pad-live-v160.txt`.

The corrected route experiment omitted Cross at read 260. Demonstration stream
1 became active, and a single Start pulse at controller read 919 stopped it and
returned to the real title screen; capture:
`UNEEDED images/v160-corrected-start-to-menu-live.png`. Later manually issued
Start pulses arrived after the short title idle window and instead stopped the
next Demonstration streams, so the real menu was not reached in this final run.
Evidence is `build/phase3/phase4-v160-corrected-start-to-menu-r1.log`.

No runner or PCSX2 process is active. The immediate next action is one fully
pre-scheduled replay: retain the bounded boot skips through read 210, pulse
Start near read 440 to leave Demonstration, pulse Start again near read 520 to
enter the menu before attract timeout, and pulse Cross near read 600 on the
default New Game selection. Capture and visually verify the real menu before
accepting the first post-Cross movie as New Game; any frame labeled
`Demonstration` is disqualifying. Then continue toward gameplay and the still
open audio-sync, asynchronous-read, and cold-boot save gates.

### Phase 4 v160 VIF1 movie-corruption correction (2026-08-30)

The newly reported heavy movie corruption is corrected at its first proven
divergence. PCSX2 and native captures established that `sceMpegGetPicture`
produces the expected 512x448 vertical-strip-major frame layout. A paired native
capture then proved that the decoded EE buffer was clean while the bytes reaching
GS local memory were corrupt; GS CT32 writes themselves round-tripped with zero
pixel mismatches.

The exact defect was reusable VIF1 continuation state. Haunting Ground's movie
upload chain places `NOP` and `DIRECT 0x700` in each REF tag's transferred upper
64 bits before the referenced 0x700-QW strip. The previous trailing-IMAGE
workaround treated all subsequent VIF input as raw GIF image bytes, so it copied
those eight VIF-code bytes into the image and left the stream misaligned. VIF1
now tracks an unfinished DIRECT payload separately from an unfinished GIF IMAGE
payload: VIF codes are parsed normally, only DIRECT payload bytes are forwarded
as image continuation, and genuinely truncated DIRECT payloads still resume on
the next input chunk. Canonical and staged runtime code are synchronized.

Focused `PS2Memory` verification passes 49/49, including two regressions that
model the game's intervening TTE `NOP`/`DIRECT` words. Live evidence is in
`build/phase3/phase4-v160-vif1-direct-continuation-live-r1.log` and
`build/phase3/ps2x-tests-v160-vif1-direct-continuation-r2.log`. The decoded
guest buffer and GS upload payload both hash to
`1B64A75650FA58B10F2C3B214012EB7C0E00C579323070A176F52295127B4203`;
the clean GS-local-memory capture is
`UNEEDED images/native-v160-gs-movie-texture-vif-fix-r1.png`.

This closes the demonstrated corruption defect, not Phase 4. A subsequent
no-input attract-mode replay advanced the formerly blocked cathedral stream
from frame 0 through frame 2,940, then advanced two additional MPEG streams
through at least frames 720 and 810. Evidence is
`build/phase3/phase4-v160-vif-fix-natural-movies-r1.log`.

The corrected New Game route was then repeated using only the previously
oracle-validated bounded boot skips (`Start`/`Triangle` through read 210), Cross
at reads 260-263, and no later input. It reached Cross at 13.507 seconds,
decoded stream 1 through frame 2,700, naturally entered stream 2 through frame
600, stream 3 through frame 2,700, and stream 4 through frame 600 before the
bounded validation run was stopped. Representative every-300-frame evidence is
under `UNEEDED images/v160-vif-fix-correct-route-every300-stream*.ppm`; the live
display capture `UNEEDED images/v160-vif-fix-correct-route-live.png` is clean.
The replay log is `build/phase3/phase4-v160-vif-fix-correct-route-r1.log`.

The user-confirmed movie skip path is `Start` followed by `Triangle`. A narrow
replay proved that applying this pair at reads 440/448, after stream 1 starts,
stops that sequence and allows stream 2 to begin naturally about 31 seconds
later. The first attempted pair at reads 360/368 was too early and therefore
had no effect. Evidence is
`build/phase3/phase4-v160-vif-fix-skip440-r1.log`. The immediate next action is
to use bounded, stream-aligned skip pairs to reach and classify interactive
gameplay, then compare audio timing against PCSX2. Synchronized audio
validation, genuine asynchronous reads, and separate-process save reload
remain open.

### Phase 4 v160 partial SNDDRV block and natural cathedral playback (2026-08-30)

The corrected natural New Game replay uses Start/Triangle only through
controller read 208 to skip the boot movies, Cross at reads 260-263, and no
Start or Triangle afterward. This supersedes the older recurring-`0x4008`
route for natural opening validation.

Narrow provider and interface traces established the cathedral starvation
order without modifying guest state. Presentation clock `0x010823F4` stops at
provider poll 65, MPEG source position `0x003BFCEC` at poll 80, cathedral
record-2 fields `0x003C2944`/`0x003C2968` at poll 102, and ring cursor
`0x003D01F4` at poll 108, while the global middleware sample counter continues.
The position method `0x001ED248` and write/completion method `0x001ED2A0` stop,
but the `0x001ED2D0 -> 0x001E70D0 -> 0x001E6910` status path continues to
return active. Evidence:
`build/phase3/phase4-v160-opening-clock-provider-inputs-r1.log` and
`build/phase3/phase4-v160-cathedral-interface-delegates-correct-route-r1.log`.

Gated `PS2X_SNDDRV_MAILBOX_TRACE` diagnostics then found the first exact
failure. After 4,393 accepted mailbox generations, the game submits two valid
final partial playback descriptors:
`{0x100,0x01F200A0,0x0007C9C0,0x4C0}` and
`{0x100,0x01F200E0,0x00080AC0,0x4C0}`. The game-scoped profile rejected them
because it required 0x80-byte request alignment. Fresh PCSX2 oracle evidence
independently shows the same 0x4C0 cathedral work chunk advancing. Canonical
and staged `capcom_snddrv_transfer.cpp` now accept the demonstrated 0x40-byte
granularity; the exact 0x4C0 case is covered by the focused test. Both Windows
targets link and `PS2IopSubsystem` passes 13/13 in
`build/phase3/ps2x-tests-v160-snddrv-partial-block.log`. Diagnostic evidence is
`build/phase3/phase4-v160-snddrv-mailbox-cadence-r1.log` and
`build/phase3/phase4-v160-snddrv-mailbox-raw-r1.log`.

The first zero-skip natural replay
`build/phase3/phase4-v160-partial-block-fix-natural-r1.log` proves this removes
the former frame-46 retirement stop. Cathedral stream 1 decodes continuously
through terminal black frame 2,940, with every-30-frame captures under
`UNEEDED images/opening-v160-partial-block-fix-stream1-frame*.ppm`.

The next event-thread stop was exact MPEG producer completion rather than
another frame-release failure. `build/phase3/phase4-v160-sequence-end-dma-state-r1.log`
shows cathedral sequence end at picture 2,960 with channel 4 still active
(`CHCR=0x105`, `QWC=0x58`). As the final queued pictures retire, `QWC` drains
to zero and the guest explicitly clears `STR` (`CHCR=0x5`) rather than reaching
a terminal chain tag. The host therefore never ran its terminal-DMA completion
hook and parked the next empty `sceMpegGetPicture` call.

Canonical and staged MPEG runtime code now treats only the demonstrated
combination of an observed sequence end, an empty decoded queue, and an
inactive IPU-to DMA producer as authoritative completion. An active chain may
still contain another sequence, so sequence end alone remains insufficient.
The focused runtime suite passes 40/40, including the new stopped-producer
regression, in `build/phase3/ps2x-tests-v160-mpeg-inactive-eof.log`.

The corrected natural replay
`build/phase3/phase4-v160-inactive-dma-eof-natural-r1.log` leaves the former
13,073/3,392 DMA/GIF endpoint, advances through 33,662/9,964, and naturally
passes two full cathedral/post-cathedral sequence ends without Start or
Triangle after New Game. The first completed sequence reaches picture 2,967;
the next reaches picture 746, and both resume rendering afterward. This proves
natural movie EOF and the downstream transition are no longer blocked. The
next executable action is to capture and classify the post-second-sequence
screen/state, then verify audible audio timing against PCSX2 before following
the New Game path into interactive gameplay. No runner remains active at this
checkpoint.

### Phase 4 v160 delayed SNDDRV callbacks and natural movie progress (2026-08-30)

A fresh PCSX2 2.6.3 oracle run resolved the remaining callback-timing
difference. The outgoing two-record playback request remains visible for one
mailbox generation; the following generation replaces it with the four exact
incoming callback records previously identified. In particular, the oracle
shows outgoing `{0x100,0x0006FD58,0x0007C640,0x400}` and
`{0x100,0x0006FD6C,0x00080740,0x400}` immediately before the wrapped incoming
records. The state-2 work metric is `0x20` in both native and oracle and is not
the divergence. Evidence:
`build/phase3/oracle-v160-cathedral-middleware-predicates-r1.jsonl`.

Canonical and staged `capcom_snddrv_transfer` now retain one exact pending
callback batch and return it on the next valid playback transaction. The first
valid request returns an empty response rather than exposing callbacks in the
same generation. Focused tests cover the initial empty response, ordinary
delayed callbacks, and delayed wrapped callbacks. Both runner and test targets
link, and `PS2IopSubsystem` passes 13/13 in
`build/phase3/ps2x-tests-v160-snddrv-delayed-callback.log`.

This correction removes the cathedral frame-release stop without forcing any
guest state: middleware naturally reaches state 3, both readiness slots become
one, stream state reaches 4, and the red cathedral sequence decodes beyond
frame 30. Captures are
`UNEEDED images/cathedral-v160-delayed-callback-stream1-frame0.png` and
`UNEEDED images/cathedral-v160-delayed-callback-stream1-frame30.png`; replay
evidence is
`build/phase3/phase4-v160-snddrv-delayed-callback-replay-r1.log`.

The recurring `0x4008` input is Cross plus Start and contaminated later movie
validation. Its read-450 Start pulse lands after the next dark-stone sequence
begins and explains that run's four-frame teardown. A replay with Start pulses
ending at read 390 leaves this later sequence active, proves it reaches
middleware state 3 and stream state 4, and decodes frames 0 through 3, but no
further decoded-frame retirement occurs even while host/vsync and DMA/GIF
continue. The current blocker is therefore the second movie instance's
release-selector/callback path, not cathedral startup, the audio-queue state-2
predicate, or an input-induced teardown. Evidence:
`build/phase3/phase4-v160-snddrv-delayed-callback-natural-r1.log` and
`build/phase3/phase4-v160-second-stream-diagnostic-r1.log`. The next executable
action is an exact call/slot trace of allocator `0x0024E698`, selector
`0x002577B0`, and release callback `0x0024E130` for the working cathedral and
stalled second instance, with no Start or Triangle after read 390.

### Phase 4 v160 SNDDRV callback-mailbox correction (2026-08-30)

Fresh isolated PCSX2 2.6.3 profiling proves the complete four-record command
batch exactly matches native apart from its dynamic IOP handle. The complete
batch is `{8,handle,0,0}`, `{4,handle,0xBB80,0}`,
`{9,handle,0,0xFFFFFFF1}`, and `{9,handle,1,0xF}`. Evidence:
`build/phase3/oracle-v158-cathedral-transport-payload-0x60-r1.jsonl`.

The first strict native implementation exposed a new exact failure at the
response parser: `0x001E4E40` reached the indirect call at `0x001E4EB4` with a
null target while parsing outgoing records such as
`{0x100,0x01F200A0,0x79A40,0x980}`. A read/write watch proved
`0x01F200A4` was never initialized; this was not a missing function boundary,
TLS overwrite, scheduler failure, or MPEG failure. The HLE service had
incorrectly acknowledged the EE-to-IOP request buffer in place, causing the EE
parser to consume an outgoing IOP descriptor as an incoming callback object.
Evidence: `build/phase3/phase4-v160-snddrv-handle-object-watch-r1.log`.

The PCSX2 oracle captures both sides of the demonstrated conversion. Two
outgoing opcode-`0x100` descriptors become incoming opcode-zero callback
records targeting exact EE objects `0x003D4E00` and `0x003D4E14`. A request
crossing either 16 KiB PCM ring boundary is split into tail and wrapped-base
records, yielding four callbacks. For example, the oracle returns
`{0,0x003D4E00,0x7D640,0x400}`, `{0,0x003D4E00,0x79A40,0x400}`,
`{0,0x003D4E14,0x81740,0x400}`, and
`{0,0x003D4E14,0x7DB40,0x400}`. Evidence:
`build/phase3/oracle-v159-cathedral-response-objects-r1.jsonl`.

Canonical and staged game-scoped SNDDRV services now perform only this exact
conversion, retain the observed ring bases, and reject malformed/out-of-range
records. Focused tests cover ordinary and wrapped batches; both Windows targets
link and `PS2IopSubsystem` passes 13/13 in
`build/phase3/ps2x-tests-v160-snddrv-callback-conversion.log`. The clean live
replay `build/phase3/phase4-v160-snddrv-callback-conversion-replay-r1.log`
eliminates the `JALR 0` failure and continues DMA/GIF progress beyond 3,900/
1,700. Boot stream 0 still reaches frame 180 and cathedral stream 1 reaches
frame 0, but stream 1 has not yet reached frame 30.

Post-callback tracing now proves the incoming callbacks execute naturally via
`0x001E4EB4 -> 0x001E41B0` for the exact callback objects `0x003D4040` and
`0x003D4070`. Their cursor advances (`0x79A40 -> 0x7A3C0 -> 0x7AD40 ...`) and
the callback returns success (`v0 == 3`). Native nevertheless stops advancing
the cathedral MPEG source position at `0x003BFCEC == 0x9200`; the PCSX2 oracle
advances the same field in `0x500` increments and begins advancing presentation
time shortly afterward. Evidence:
`build/phase3/phase4-v160-post-callback-state-r1.log` and
`build/phase3/phase4-v160-post-callback-clock-r1.log`.

The earliest current stop is decoded-frame retirement rather than callback
conversion or decoding. Cathedral fills three effective frame slots
`0x01083960`, `0x01083A48`, and `0x01083B30` to state two; the fourth observed
candidate at `0x01083C18` remains unused. Allocation owner `0x0024E698`
continues to run and returns zero once those three slots are occupied, while
cathedral release path `0x002577B0` and release callback `0x0024E130` are never
selected. Boot-stream releases do occur, so release machinery is not globally
absent. Evidence:
`build/phase3/phase4-v160-frame-release-after-callback-r1.log`.

A root-object selector trace records stable cathedral fields
`0x01082830=5`, `0x01082834=1`, `0x0108287C=1`,
`0x0108350C=0x00459EE0`, and `0x01083510=3`; the first three already match the
existing PCSX2 oracle. Evidence:
`build/phase3/phase4-v160-cathedral-release-selector-r1.log`. The immediate
action is to capture the final two fields and the table at `0x00459EE0` in an
isolated PCSX2 cold boot, then trace the first divergent event-class-6/channel-2
method selection. Do not force a callback, clear frame slots, or advance movie
or audio clocks.

### Phase 4 v156 response-mailbox correction (2026-08-30)

The v151 category-4 worker sleep was caused by a proven-invalid legacy patch,
not by a missing scheduler wake. Exact native/PCSX2 transport comparison first
showed that PCSX2 constructs the SNDDRV response payload at `0x003D5080`, while
native constructed the same record with a null payload. A narrow constructor
trace then proved that owner `0x001E5008` correctly computes
`s0 == 0x003D5080`, but entered `0x001DA5D0` with `a1 == 0` because
`config/recomp.base.toml` replaced delay-slot instruction `0x001E507C` with a
NOP under the unfounded label "Potential self-modifying code." The verified
U.S. ELF word is `0xAE304DC8` (`sw s0, 0x4DC8(s1)`), which publishes the
response payload pointer. No runtime state was forced.

The invalid patch was removed and v156 was generated from the verified ELF.
Its report remains 11,054 functions, 10,844 recompiled bodies, 210 runtime
bindings, 390,097 resumable entries, zero skipped/decode/unhandled/
correctness-critical failures, and zero errors. The generated function now
emits the exact store in the branch delay slot. Artifacts:
`build/phase3/functions.effective-v156.csv`,
`build/phase3/recomp.effective-v156.toml`,
`build/phase3/functions.provenance-v156.csv`,
`build/phase3/recompiled-v156`, and
`build/phase3/recompile-v156.log`.

The post-correction replay
`build/phase3/phase4-v156-restored-response-mailbox-replay-r1.log` confirms the
native record now uses payload `0x003D5080` and tail `0x003D58C0`, matching the
PCSX2 EE layout. Both response and command records cycle through
`0x001DA7F0`; deterministic pad reads advance naturally past the former
read-183 stop through reads 210, 481, 720, and 745. DMA/GIF continue beyond
1,800/900 instead of parking at 436. This resolves the v151 scheduler blocker.
The 720 Start and 745 Triangle inputs make the latter portion a skip-path smoke
test only; they are not evidence of natural movie completion. The immediate
next validation is a cold replay with neither Start nor Triangle during movie
playback, followed by exact cathedral frame/presentation-state comparison.

The natural v156 replay reaches the second/cathedral MPEG stream, completes its
middleware propagation through state 4, and decodes frame 0, but does not yet
reach frame 30. The first exact native command-mailbox record at that stop was
captured in `build/phase3/phase4-v156-first-snddrv-command-r2.log` as
`{8, 0x01F20000, 1, 0}` with count one. The PCSX2 payload oracle submits the
same opcode/argument shape with its own dynamic IOP handle. After implementing
that destination-specific transaction, the next native pending record was
captured in `build/phase3/phase4-v157-post-opcode8-command-r1.log` as
`{5, 0x01F20000, 0, 0}`; the PCSX2 timeline also consumes that exact shape.

The conservative SNDDRV compatibility service now accepts only empty
heartbeats plus those two proven one-record forms at command destination
`0x000B2780`. It validates the exact count, destination, opcode, nonzero handle,
argument, reserved field, and mailbox size, tracks accepted start/control
transactions, and still refuses null payloads, unrelated destinations, and all
uncharacterized records. Canonical and staged sources are synchronized, both
Windows targets link, and `PS2IopSubsystem` passes 13/13.

Zero-input decoded-frame replays
`build/phase3/phase4-v157-opcode8-cathedral-every30-r1.log` and
`build/phase3/phase4-v158-opcode5-cathedral-every30-r1.log` prove that completing
the two command generations is necessary but not sufficient: stream 0 still
captures frames 0 through 180 at 30-frame intervals, stream 1 captures frame 0,
and stream 1 still produces no frame 30. The blocker has therefore narrowed to
the IRX periodic response worker/status records, not another unacknowledged
start/control command, MPEG decoding, or the resolved v151 scheduler defect.
No movie/audio/scheduler state was forced.

The follow-up transition trace
`build/phase3/phase4-v158-periodic-response-transition-r1.log` distinguishes
production from consumption. Native does produce two concrete response records
`{0x100, 0x01F200A0, 0x00079A40, 0x980}` and
`{0x100, 0x01F200E0, 0x0007DB40, 0x980}`, but their response generation remains
odd at `0x104D`. At the same checkpoint, the command side has advanced to a
four-record batch with odd generation `0x1045`; its first record is
`{8, 0x01F20040, 0, 0}`. The next blocker is therefore exact bidirectional
multi-record handling. The calibrated payload trace
`build/phase3/phase4-v158-four-record-command-payload-r2.log` captures the
complete batch as `{8,0x01F20040,0,0}`, `{4,0x01F20040,0xBB80,0}`,
`{9,0x01F20040,0,0xFFFFFFF1}`, and `{9,0x01F20040,1,0xF}`. The next action is
to map this exact four-record batch to the PCSX2 batch and implement its
demonstrated response-consumption path. Do not blanket-ack count-two/count-four
payloads.

### Phase 4 natural boot-movie EOF correction (2026-08-30)

The user clarified that Start followed by Triangle skips movies. Any replay
containing that sequence is therefore a skip-path test and is not evidence of
natural movie playback, completion, teardown, or A/V synchronization. Natural
movie validation must omit both buttons. The earlier recurring `0x4008` pulses
were also contaminated because `0x4008` is Cross plus Start.

A genuinely no-input pre-correction cold replay in
`build/phase3/phase4-v153-oracle-matched-unskipped-r2.log` decodes and presents
128 clean frames (numbered 0 through 127) from the first boot movie, then parks
with the main thread still in the MPEG picture wait and the category-4 worker
at `0x001CB440`. No scheduled pad input was reached. The guest does not perform
the movie's natural end-of-stream completion and teardown.

`build/phase3/phase4-v153-natural-eof-thread-trace-r2.log` proves why: after
frame 127, thread 1 remains at `0x002500DC` in typed MPEG wait reason 6 while
DMA/GIF counters freeze at 1161/406. This direct IPU path never calls
`sceCdStStart` or `sceCdStRead`, so the old CD-producer EOF path cannot announce
completion. The runtime now combines two real conditions instead: the MPEG
elementary stream has seen its sequence end and DMAC channel 4 has completed
the corresponding terminal payload. Only then does it drain decoder delay,
mark the stream ended, and complete the typed waiter. A sequence-end marker by
itself remains insufficient.

Canonical and staged runtime/test sources are SHA-256 identical, both Windows
targets link, and `PS2RuntimeExpansion` passes 38/38. The new regression proves
that sequence end alone reports incomplete and terminal IPU-to DMA completion
then reports complete. Evidence:
`build/phase3/ps2x-tests-v153-ipu-eof.log`.

The clean post-correction replay
`build/phase3/phase4-v153-natural-eof-fix-replay-r2.log` no longer parks after
frame 127. It wakes thread 1, continues DMA/GIF progress, restores movie-manager
pointer `0x003B7238` (matching the PCSX2 oracle after natural teardown), and
constructs the next movie object. Focused entry tracing in
`build/phase3/phase4-v153-second-movie-entry-trace.log` proves a fresh
`sceMpegCreate` handle `0x010848B0`, updates active handle `0x001717BC`, and
serves that handle through `sceMpegGetPicture`; this is not reuse of the ended
`0x0100B2B0` handle. A zero-input display capture at tick 1500 is the intact red
cathedral scene in
`UNEEDED images/phase4-v153-natural-post-eof-tick1500.png`. It proves continued
presentation of the fresh handle's first picture, not continued animation or
natural completion of the cathedral sequence.

An additional zero-input cold boot captured the first boot MPEG at exact
decoded-frame intervals of 30. Frames 30, 60, 90, 120, 150, and 180 are intact
CAPCOM-logo animation checkpoints in
`UNEEDED images/boot-movie-every30-frame{30,60,90,120,150,180}.png`. This run
also reproduced that natural playback continues beyond frame 127 and reaches
frame 180. The capture is a visual transport/decode check, not yet an A/V-sync
oracle comparison. Evidence:
`build/phase3/phase4-v153-boot-movie-every30.log`. A gated
`PS2X_CAPTURE_MPEG_FRAME_INTERVAL` diagnostic produced the sequence from the
decoder's real frame index without modifying guest state.

Exact per-stream capture now corrects an earlier overstatement about the second
boot/cathedral MPEG. In
`build/phase3/phase4-v153-cathedral-every30-eofscope-r1.log`, stream 0 reaches
frames 0, 30, 60, 90, 120, 150, and 180, but fresh stream 1 produces only
`UNEEDED images/cathedral-every30-eofscope-stream1-frame0.ppm`; no stream-1
frame 30 exists. A stale global CD-EOF flag was found to be incorrectly
applicable to fresh direct-IPU handles and is now scoped to actual CD-demux
input. The focused ownership regression and complete `PS2RuntimeExpansion`
suite pass 39/39 in
`build/phase3/ps2x-tests-v153-mpeg-eof-ownership.log`, but the live replay proves
that correction is not the cathedral blocker.

The narrow hardware/control traces
`build/phase3/phase4-v153-cathedral-ipu-seam-r1.log`,
`build/phase3/phase4-v153-cathedral-mpeg-feed-r1.log`, and
`build/phase3/phase4-v153-cathedral-control-r4.log` localize the remaining stop.
The fresh handle `0x010848B0` receives valid channel-4 spans, finds its sequence
header, keeps `decoderFailed == false`, and enters `sceMpegGetPicture` four
times with `streamEnded == false`. Its only presentation wait is a correct
one-VSync deadline (`1172 -> 1173`), after which the continuation resumes. The
guest then stops issuing picture calls after pictures 0 through 3 without
calling `sceMpegIsEnd` or `sceMpegDelete`. A bounded asynchronous-refill
experiment did not advance the presented frame count and was reverted; no
forced wake, movie state, or unproven runtime behavior remains. The immediate
movie action is therefore to identify the producer/thread/event dependency
after the fourth `sceMpegGetPicture` return and compare that exact scheduler
record with PCSX2.

A new gated `PS2X_TRACE_MOVIE_STATE` diagnostic records changed-only snapshots
of the exact loader, producer, ring, subordinate, middleware, handle,
readiness, stream, and movie-manager fields. It does not modify guest state.
`build/phase3/phase4-v153-opening-state-chain-r2.log` proves that both boot
movie objects publish length `0x001B6CCC`, fill the ring, advance
middleware 1 -> 2 -> 3, assert both handles and readiness slots, advance the
stream 2 -> 4, and begin clean decoding. It does not prove full second-movie
animation: the latest exact capture and control trace supersede that inference.
After New Game Cross, the old object is torn down but no replacement opening
object reaches that chain.

Focused construction tracing from the older skip-contaminated path in
`build/phase3/phase4-v153-opening-construction-trace-r2.log` narrows the first
missing transition further. Movie constructor `0x002425F8` is invoked for the
sixth time while Cross is active and enters helper `0x00242908`, but that
invocation does not reach the normal `0x00242930 -> 0x001D25C0` object-creation
call seen in successful boot constructions. The immediate gate is the value
returned by `0x00252060` from lifecycle word `0x003EA14C`; its alternate path
reads singleton pointer `0x003EA150` through `0x00242978`.

The existing PINE oracle probe now includes those two fields. Fresh isolated
PCSX2 evidence in
`build/phase3/oracle-v153-opening-manager-globals.jsonl` shows the opening
reconstruction approximately 16 ms after the old object clears while
`0x003EA14C == 0` and `0x003EA150 == 0x003B7238`. Native stable snapshots also
show lifecycle state zero, but its pointer later holds `0x003B72FC`; sub-frame
native write traces subsequently showed transient lifecycle writes
and a singleton pointer that remains on `0x003B72FC` after skipped playback,
whereas PCSX2 returns to base `0x003B7238` after natural completion. Because
the native runs did not naturally complete the boot movie, this divergence is
not yet evidence of a constructor or pointer defect. Do not inject either value
or force construction.

The clean title/menu replay in
`build/phase3/phase4-v153-clean-title-newgame-opening-r3.log` omits Triangle,
uses Start only at the demonstrated title timing, and reaches the real main
menu. Capture `UNEEDED images/phase4-v153-clean-newgame-tick2000.png` shows that
the two Up presses wrap the default New Game selection to Load Game; they are
therefore not part of the correct New Game replay. The next executable action
is Start at the demonstrated title prompt followed by Cross on the default New
Game selection, with no Triangle and no further Start while a movie is active.
Then compare the fresh second handle's picture count and manager/UI fields with
PCSX2 and validate the real opening movie without Start or Triangle during
playback.

Two additional diagnostic cold boots parked at DMA count 17 in worker sleep
`0x001CB440`, while identical reruns progressed. This intermittent early-boot
reliability issue is tracked separately and must be resolved before Phase 4
reliability can pass; it is not treated as evidence about the opening request.
The first corrected Start-then-Cross replay
`build/phase3/phase4-v153-clean-newgame-opening-r4.log` also reproduced that
DMA-17 race before any pad read, so it is not behavioral evidence for New Game
or movie playback.

Focused failed/successful scheduler comparison now localizes that race before
movie or input handling. Both paths are identical through the large initializer
owner `0x00169680` and its 206 successful `0x0016B2B0` calls. In failed boots,
thread 1 eventually returns to PC zero with DMA/GIF fixed at 17/17; VBlank IRQ
invocations continue independently but the ordinary event/DMA loop never
starts. In successful boots, the same VBlank route
`0x001AAC30 -> 0x001CC5B0 -> 0x001CC488` reaches the event callback at
`0x001CC51C`, the event-runtime gate returned by `0x001CBBE8` is live, and DMA
continues immediately. Evidence:
`build/phase3/phase4-v153-dma17-thread-snapshot-r1.log`,
`build/phase3/phase4-v153-dma17-invocation-trace-r1.log`,
`build/phase3/phase4-v153-dma17-loader-dispatch-r1.log`, and
`build/phase3/phase4-v153-dma17-loader-owner-r1.log`. A gated,
behavior-neutral `PS2X_TRACE_EE_INVOCATIONS` diagnostic now records queued,
attached, standalone, and completed guest invocations plus PC-zero dormancy.
Cold failed/successful read/write traces subsequently prove `0x003B82C8`
remains zero in both outcomes and is never written on this path; it is not the
race discriminator. The next scheduler comparison is the main-thread exit or
dormancy transition after the VBlank-owned flags used by `0x001BED00`, not that
event-runtime word. Do not keep or force any scheduler state directly.

### Phase 4 cathedral PCM-service divergence (2026-08-30)

The fresh cathedral MPEG is not currently progressing beyond its first
presented picture. Exact IOP allocation tracing proved that the game's first
IOP heap allocation is based at `0x00070E00`: both native and PCSX2 use offset
`0x8C40`, so the first audio descriptor must be `0x00079A40`, not the earlier
synthetic `0x04008C40`. Canonical and staged runtime sources now use the real
HG IOP heap domain while retaining private IOP backing, and a regression proves
that equal-numbered IOP and EE addresses do not alias. The corrected native
descriptor exactly matches PCSX2. This constant is verified for this HG boot
profile; it should become profile-configurable before being treated as a
universal PS2 runtime default.

The correction passes `PS2RuntimeExpansion` 39/39 and its new private-backing
SIF test. `PS2SifDma` currently passes 15/16: the remaining failure is the
existing `sceSifGetOtherData backfills zero sound-status sums for later banks`
case, which reports unhandled SID 1/RPC `0x12` and remains under investigation.
Evidence is in
`build/phase3/ps2x-tests-v153-iop-heap-real-domain-r2.log`.

The cold replay
`build/phase3/phase4-v153-cathedral-iop-heap-real-domain-r1.log` decodes boot
stream 0 through at least frame 180, constructs cathedral stream 1, naturally
propagates middleware 1 -> 3, handle/readiness 0 -> 1, and stream state
1 -> 2 -> 3 -> 4, then presents only cathedral frame 0. The visually inspected
capture is
`UNEEDED images/cathedral-iop-heap-real-domain-stream1-frame0.png`. No
cathedral frame-30 capture exists yet and boot-stream frames must not be
reported as cathedral frames.

The presentation-clock arithmetic is completely accounted for. Native reads
source position `0x4000`, audio cursor `0x2000`, and older-ring adjustment
`0x2000`, correctly producing `0x4000 - (0x2000 + 0x2000) == 0`; the zero clock
is a consequence rather than a clock-formula defect. A later exact v155
reservation trace supersedes the earlier conclusion that the PCM input rings
empty and stop being replenished. At the stable cathedral checkpoint, both
16 KiB PCM rings are full: readable `+0x0C == 0x4000`, writable `+0x10 == 0`,
cursor `+0x18 == 0`, and size `+0x20 == 0x4000`. All 85 observed full/wrapped
reservations per ring at `0x001E3318` are correct. For example, ordinal 566
returns `v0 == 0x4000`, publishes descriptor base `0x01078A80` and length
`0x4000`, and decrements readable bytes to zero. The apparent zero length in
an earlier exit snapshot was register `a1` after the function's `mfhi`, not the
descriptor length.

Fresh isolated PCSX2 2.6.3 evidence in
`build/phase3/oracle-v153-cathedral-pcm-rings-exact-r1.jsonl` still establishes
the expected continuing audio progress, but it must not be interpreted as
proof of native input starvation. The exact caller census remains useful:
`0x001CE0F0` is called only at `0x001CE1E4`, its owner only at `0x001CE50C` and
`0x001D5244`, and the live path is the former. The PINE probe includes the
`0x003B36C4` work-state/handle/function/chunk fields.

The earlier interpretation of that comparison is superseded. In
`build/phase3/oracle-v153-cathedral-work-record-r2.jsonl`, PCSX2 advances the
same source position from zero through `0x00034000` in five seconds while the
work record continues completing variable `0x100`-`0x500` chunks. Native
instead reaches exactly `0x00004000` after 14 work submissions and remains
there. `build/phase3/phase4-v153-cathedral-source-ring-r1.log` proves the
native large transport ring also reaches a stable state at that boundary;
the upstream service continues to run. The v155 evidence below localizes the
stall later, at downstream destination-node availability, rather than proving
data-production starvation. It is still not a stopped scheduler or bad
presentation-clock calculation.

A behavior-neutral `PS2X_DISPATCH_TRACE_OBJECT` filter is now present in the
canonical and staged dispatch profilers, and the Windows runner relinks. It
allows exact ring-object profiling without exhausting limits on unrelated
vtable traffic. The first filtered run proves ring `0x003D01E8` continues to
receive writes from `0x00244290`, while PCM packet ring `0x003D01A8` receives
only 11 writes: nine at `0x001CD820`, one at `0x001CD9A8`, and its
initialization write at `0x0024199C`. The proposed demultiplexer profile is
superseded by the exact downstream reservation evidence below. The unrelated `PS2SifDma` later-bank
compatibility failure is scoped to the `slus_201.84` profile and is not
evidence for this verified HG path. Do not force mixer calls, ring callbacks,
the clock, or movie state. Evidence:
`build/phase3/phase4-v153-cathedral-mixer-caller-gate-r1.log`,
`build/phase3/phase4-v153-cathedral-audio-status-fields-r1.log`, and
`build/phase3/phase4-v153-cathedral-ring-write-object-filter-r1.log`.

The next exact queue differential initially appeared to supersede the
provisional demultiplexer action above. A live SNDDRV trace in
`build/phase3/phase4-v154-snddrv-live-r1.stderr.log` shows that the cathedral
starts its already-loaded 16 KiB SPU2 region with command functions
`0x00160000` and `0x00240000`; both native and PCSX2 return status zero. The
separate SID `0x77777778` transfers are the initial bank population at SPU2
destinations `0x000DAAC0` through `0x0017AAC0`, not recurring cathedral PCM
refills.

The first interpretation of the queue writer profile was incorrect: stale
node-pool contents were mistaken for a full active queue. The exact vtable and
head-pointer trace supersedes that conclusion. At the cathedral stall,
`0x003D4070+0x18` (`0x003D4088`) is zero and `0x001E3F20` returns zero, proving
the active data queue is empty. A v155 transfer-stage trace then superseded the
provisional upstream-starvation conclusion. `0x001D9C80` successfully reserves
the full PCM input through `0x001E3318`, but its destination write reservation
through `0x001E4010` returns a zero-length descriptor. The normal `0x980`
transfer therefore becomes zero after selecting the smaller length and rounding
to the verified `0x40` alignment. The fixed downstream node/free capacity is
exhausted or held by the asynchronous audio path even though the active data
queue is empty. This is downstream backpressure, not an invalid PCM reservation
and not yet evidence of demultiplexer starvation. Evidence:
`build/phase3/phase4-v154-audio-queue-position-writes-r1.log`,
`build/phase3/phase4-v154-audio-queue-consume-object-r1.log`,
`build/phase3/oracle-v154-cathedral-audio-cursor.jsonl`,
`build/phase3/phase4-v155-cathedral-ring-wrap-reservation-r1.log`, and
`build/phase3/phase4-v155-cathedral-pcm-submit-alignment-r1.log`.

Exact SNDDRV.IRX import analysis also rules out the proposed direct cursor
model: the module registers asynchronous transfer and SPU2 interrupt callbacks
but does not import `sceSdGetAddr`. No IOP/SPU2 behavior change is warranted
from the present evidence. The exact queue vtable is based at `0x003D3FA8`;
its relevant methods are `0x001E4010` (write reservation), `0x001E4318`
(commit/update), `0x001E41B0` (read reservation), and `0x001E3F20` (query).
The downstream service `0x001E1DA0` reads these endpoints. Its call at
`0x001DA250` is only a record-state transition: it clears record `+0x3C` and
writes one to byte `+1`; it is not the hardware submission itself. The
record byte is a persistent enabled state and is cleared by `0x001DA260` only
during stop/reconfiguration; it is not a per-buffer completion flag.

Further source review rules out a stuck SIF-DMA ID in the `0x003C3C50`
transfer-record pool. `0x001D9FF8` checks pending record byte `+2` with
`sceSifDmaStat` and `0x001D9C80` retires and commits the preceding source and
destination descriptors before submitting another copy. The runtime's
`sceSifSetDma` performs that copy synchronously and `sceSifDmaStat` correctly
reports it retired. At the cathedral stop, both PCM source queues remain full
while the destination reservation is zero, so this completed EE-to-IOP copy
mechanism is not itself waiting on a transfer ID.

The next profiled layer is the separate shared command/status transport at
`0x003C4470` and `0x003C44B4`. A behavior-neutral trace in
`build/phase3/phase4-v155-audio-playback-record-trace-r1.log` records their
complete layouts and repeated service calls. The first record owns response
parser `0x001E4E40` and command builder `0x001E4EE0`; the second owns
`0x001E09C8` and `0x001E0A28`. Both records remain pending at sequence one in
the bounded native trace. Because those callbacks cover more than movie audio,
this is a narrow transport-completion lead, not permission to increment a
counter or fabricate a response.

The corresponding isolated PCSX2 2.6.3 oracle run is now recorded in
`build/phase3/oracle-v155-cathedral-transport-records-r1.jsonl`. In five
seconds, both live transport sequences advance from `0x533` through `0x715`
(more than 240 distinct values), the audio/destination queue sizes drain and
refill, the audio cursor reaches `0x3000`, and the cathedral source cursor
reaches `0x0019D800`. Native instead leaves both paired sequences at one while
continuing ordinary EE DMA/GIF work. This decisively localizes the native
cathedral backpressure to absent IOP-side SNDDRV command consumption and
response production, not `sceSifDmaStat`, MPEG decoding, or a lost EE scheduler
wake.

The PINE probe previously labeled fixed addresses as transport sequences even
though the transport payload/tail allocations are dynamic. It now has an
explicit `--dereference-transport` mode which follows each record's buffer and
tail aliases, masks cached aliases to physical RAM, and captures the first
`0x40` bytes. The corrected one-second oracle artifact is
`build/phase3/oracle-v155-cathedral-transport-payload-r1.jsonl`. It proves that
word 15 in both live tails advances from `0x533` through `0x589`. The response
payload is substantive rather than an acknowledgment: its final first 16 words
are `2,0,0,0, 0x100,0x6FD58,0x7C640,0x400, 0x100,0x6FD6C,0x80740,0x400,
0,0,0,0`, while the command payload contains live opcode/object records. The
native counterpart in
`build/phase3/phase4-v155-native-transport-payload-r1.log` shows a valid empty
initial command heartbeat, then both tail sequences remaining at one. Empty
delta content is therefore valid, but the real IOP worker must still service
and acknowledge every submitted generation.

The exact disc module `Haunting Ground (USA)/MODULES/SNDDRV.IRX` has SHA-256
`3B8E76B6FCAA1B3FBE59848A6094BD230D5FE9643496E3AFF5F3B969730404BF`.
Offline PS2Recomp analysis extracted 219 functions and 3,904 relocations into
`build/phase3/snddrv-irx-analysis-v155.toml`; the log and disposable annotated
output are `build/phase3/snddrv-irx-analysis-v155.log` and
`build/phase3/snddrv-irx-recompiled-v155`. SID `0x77777777` registers the real
handler at linked `0xA7B4`; RPC functions `0x00160000` and `0x00240000` route
to linked functions `0x6BC8` and `0x7994`. More importantly, linked function
`0x5568` is now proven to construct one `sceSifSetDma` descriptor, call the
`sifman` ordinal-7 import, and poll ordinal 8 (`sceSifDmaStat`). Its periodic
worker at `0x9F78` produces changed response fields and returns them through
the EE destination established by the driver. This is the source-informed
implementation seam; the existing generic SID-zero-return stub does not run
that worker and has no direct-SIF-transfer consumer.

The immediate action is to map the `0x9F78` worker's command-buffer and response
record layout against the EE builders/parsers, then implement that demonstrated
protocol as a dedicated game-profile IOP service with focused unit tests. Do
not merely advance word 15: the response carries real queue/cursor state needed
to release destination capacity. Do not clear an EE queue, recycle nodes
directly, force stream state, or synthesize movie-clock progress.

### Phase 4 exact MMIO and IPU chain-resume correction (2026-08-30)

The clean v153 generation exposed and corrected a reusable recompiler defect.
At guest `0x0024F178` and `0x0024F17C`, exact local LUI/ORI address propagation
proved loads from channel-4 `CHCR` (`0x1000B400`) and `MADR` (`0x1000B410`),
but the analyzer's conservative `isMmio/mmioAddress=0x10000000` annotation
overrode those exact hints. Generated v151 code consequently read both values
from `0x10000000`. `instruction_translator.cpp` now uses the conservative
annotation only when no exact local address exists. A deliberately conflicting
code-generator regression verifies that generated code retains `0x1000B400`.
The full CodeGenerator suite remains 57/58 because its pre-existing
working-directory-sensitive VU enum test cannot locate `instructions.h`; the
new MMIO regression itself passes.

The corrected v153 generation contains 11,054 functions: 10,844 recompiled
bodies, 210 runtime bindings, and 390,097 resumable entries, with zero skipped
functions, decode failures, unhandled instructions, correctness-critical guest
fallbacks, failures, or errors. The generated owner at `0x0024F108` now reads
the exact B400/B410 registers. Canonical and staged sources are synchronized,
the Windows runner links, and the clean replay no longer produces the earlier
implausible `0x00080076/0x000CD538` restored channel state.

Stateful eight-QWC IPU input pacing then revealed a separate resume defect:
after the game force-stopped channel 4 mid-REF payload and restored its
guest-visible `MADR/QWC/TADR`, the runtime reset its internal tag-progress bit,
treated TADR as the current tag, and skipped the restored payload remainder.
The first observed losses were 80 QWC at `0x00CE2F00` and 120 QWC at
`0x00CE3480`; FFmpeg damage diagnostics followed immediately. A chain start
with nonzero restored QWC now marks that payload in progress and drains it
before fetching TADR. The focused resume regression proves exact ordering,
register progression, and terminal completion; all 37
`PS2RuntimeExpansion` tests pass.

`build/phase3/phase4-v153-ipu-resume-replay.log` is the first cold replay with
clean decoder input at this checkpoint: zero damaged-slice, missing-motion-
vector, invalid-macroblock, missing-target, unimplemented-stub, or error
diagnostics. Every observed pause/resume advances by exactly eight QWC instead
of skipping the remainder. The replay delivered Cross at reads 481-484 and the
previously requested Start at 720-724 followed by Triangle at 745-749. The user
clarified on 2026-08-30 that Start/Triangle are movie-skip inputs. They are
therefore useful only for a skip-path test and must not be cited as evidence
that a movie progressed or completed naturally.

Time-indexed captures now prove that the corrected transport can feed a clean
boot movie through the complete decode/presentation path. Decoded MPEG frame 3
is the intact CAPCOM/dog silhouette in
`UNEEDED images/phase4-v153-decoded-frame3.png`, and host display tick 790 is
the intact movie/cathedral frame in
`UNEEDED images/phase4-v153-display-tick790.png`. The earlier red, tiny,
diagonal post-Triangle capture
`UNEEDED images/phase4-v153-ipu-resume.png` is a later loading/game display and
is not evidence of an MPEG upload or presentation defect.

A corrected cold replay containing boot pulses and New Game Cross only (no
Start or Triangle) is recorded in
`build/phase3/phase4-v153-opening-unskipped.log`. It decodes the expected 12
boot-logo frames, but no new MPEG frames begin after Cross through host tick
4500. The active movie blocker is therefore earlier than decode and host
presentation: the real opening request is not naturally reaching its MPEG
decode phase. The next executable action is a changed-only trace of the exact
opening loader, producer, ring, subordinate, middleware, handle, readiness, and
stream fields, followed by a PCSX2 comparison at the first divergent state.
Do not force any movie/request state. CRI audio/timing, saves, PCSX2 checkpoint
comparison, and Tier-1 host validation also remain open.

### Phase 4 MPEG wait and IPU-to source-chain progress (2026-08-29)

The v151 `0x001CB440` observation was not a demonstrated lost worker wake.
Thread snapshots at the stable checkpoint show main thread 1 blocked at
`0x002500DC` with typed wait reason 6 (`Mpeg`) after entering
`sceMpegGetPicture` at `0x002A5A48`; the category-4 worker is merely the only
remaining runnable thread. Static review also shows `0x0024C140-0x0024C2F8`
is image/texture buffer logic and `0x00246CB0` is an error-reporting helper,
not the scheduler state mutation previously hypothesized.

The exact missing hardware seam is DMAC channel 4 (RAM to IPU). The guest's
`0x0024F408` path starts the transfer directly through `0x1000B400`, while the
native runtime previously consumed compressed data only through high-level
MPEG add-bitstream calls. A first runtime correction publishes normal-mode
MADR/QWC spans to the existing MPEG decoder; an exact regression test passes,
but the cold replay proved this game uses source-chain mode instead:
`CHCR=0x176`, `QWC=0`, and `TADR=0x00D55400` at the first movie transfer.
The runtime now walks standard source-chain tags and publishes each exact
payload span without fabricating a picture, state transition, or wake. The
canonical and staged PS2Recomp trees remain synchronized, both Windows targets
link, and all 35 `PS2RuntimeExpansion` tests pass, including normal and END-tag
IPU-to transfers.

`build/phase3/phase4-v152-ipu-chain-replay.log` demonstrates real forward
progress: `sceMpegGetPicture` returns eleven times, pad reads naturally pass the
old 183 stop and reach Cross at 481/484, and main thread 1 is no longer left in
the MPEG wait. This is not yet a correctness result. FFmpeg reports damaged
MPEG slices, and later channel-restoration writes show implausible normal-mode
state (`MADR=0x00080076`, `QWC=0x000CD538`) after the source chain was drained
synchronously. The immediate blocker is therefore implementing exact IPU FIFO
pacing and DMAC chain pause/resume state, not scheduler wake ownership.

PCSX2 2.6.3 source review establishes the relevant hardware model used for the
next correction: the IPU input FIFO accepts at most eight QWC; an active
IPU-to chain advances `MADR` and decrements `QWC` only by the accepted amount;
the current tag remains in progress while payload QWC remain; decoding resumes
the DMA after FIFO consumption; active `CHCR` rewrites do not restart the
channel; and `STR` clears with channel-4 completion/interrupt only after the
terminal payload completes. This is technical-reference evidence, not a claim
that the native runtime already implements or passes that behavior. The next
action is a focused stateful channel-4 implementation plus register/ordering
tests, followed by a clean PCSX2 checkpoint comparison. Do not treat the
read-484 advance or damaged decoded output as movie correctness.

### Phase 4 strict gap batch and live resume owner (historical, 2026-08-29)

This section records the v150/v151 discovery sequence. Its final scheduler-wake
inference is superseded by the MPEG-wait/IPU-to evidence in the section above;
it is retained so the boundary proof and versioned artifact history remain
reproducible.

The v150 map batch-restores all 114 executable-gap owners still reported by
`build/phase3/gap-function-audit-v147.json` after filtering against v149. Each
entry passed the audit's conservative delay-slot-aware proof; speculative
indirect destinations remain excluded. The validated v150 map contains 11,054
functions, and recompilation produced 10,844 bodies plus 210 runtime bindings
and 390,094 resumable entries with zero skipped functions, decode failures,
unhandled instructions, correctness-critical guest fallbacks, or errors. The
Windows runner linked after a required second CMake build picked up newly
generated unity groups, and all 34 `PS2RuntimeExpansion` tests pass.

The clean deterministic v150 replay no longer stops at omitted indirect method
`0x0023A6E0`. It instead exposed a distinct false analyzer end: owner
`0x0024C140-0x0024C164` falls through to internal scheduler resume PC
`0x0024C164`, which was not registered and leaves the category-4 worker parked
at `0x001CB440`. Exact verified-ELF traversal proves the complete owner is
`0x0024C140-0x0024C2F8`: 110 instructions are reachable, every conditional
branch is contained, its two direct tail exits restore the stack and target
mapped `0x00246CB0`, its only JAL targets mapped `0x0024DB28`, and its sole
ordinary return includes the delay slot before four padding words. v151 records
that exact override, recompiles the same 11,054 functions into 10,844 bodies
plus 210 bindings, and increases resumable entries to 390,097 with zero skips,
decode failures, unhandled instructions, correctness-critical guest fallbacks,
or errors. The Windows runner links and all 34 focused runtime tests pass.

The clean v151 replay verifies natural continuation beyond `0x0024C164`: the
v150 missing-target diagnostic is absent and DMA progresses from 434 to 436.
Execution still parks in category-4 worker sleep at `0x001CB440` before pad read
210, now without any missing-target diagnostic. Filtering all 115 candidates in
the v147 strict report against the v151 effective map confirms zero remain
unmapped. At that checkpoint the worker view suggested scheduler wake/state
behavior and motivated the next trace. The later typed-thread snapshot
disproved that inference by identifying main thread 1's MPEG wait; the current
interpretation and action are recorded above.

Roadmap progress is tracked by exit criteria, not a speculative percentage.
The Phase 2 isolated clean-input proof consumes only the verified user ELF plus
the approved checkout inputs
`config/functions.analysis.csv`, `config/recomp.base.toml`,
`config/functions.manual.toml`, and `config/game.toml`; it does not invoke
Ghidra or read a Ghidra project. The merge emits 10,660 non-overlapping
functions (3,965 approved Ghidra-export records and 6,695 reviewed manual
records), the effective PS2Recomp TOML, and a per-boundary provenance CSV.
The executable-byte ledger classifies all 3,650,048 bytes: 2,155,036 code,
40,828 padding, 1,381,064 investigated-gap, and 73,120 file-backed data bytes.
Direct-call, direct-target, pointer-data, boundary, invalid-manual-set, and
gap-leaf audits all report zero unresolved candidates; eight direct targets are
intentional runtime bindings. The standalone recompilation processes all
10,660 functions and reports 10,461 recompiled, 199 stubs, zero skipped, zero
decode failures, zero unhandled instructions, zero correctness-critical guest
fallbacks, and zero errors. The Python regression suite has 38 passing tests,
including malformed TOML, hash mismatch, overlaps, duplicates, invalid
handlers, deterministic ordering, provenance, and classification gates.
The proof was replayed from a directory containing none of the workspace's
`build/analysis` state and no Ghidra project; it reproduced the complete byte
ledger and the same successful 10,660-function recompilation.

Phase 3's final v119 Windows x86-64 proof uses the corrected v88 map and a clean
generated runner with no hand-edited generated behavior. A verified-ELF audit
fixed three render callbacks whose earlier half-open bounds omitted their MIPS
delay-slot instructions. The resulting Phase 3 map contains 10,729 non-overlapping
functions (3,959 standalone-analysis and 6,770 reviewed manual records), with
zero uncovered direct targets, pointer candidates, boundary candidates,
invalid manual sets, or gap-leaf candidates. All 3,650,048 executable bytes are
classified: 2,159,636 code, 41,364 padding, 1,375,928 investigated-gap, and
73,120 file-backed data bytes. The latest linked Phase 4 v145 map processes
10,935 functions (10,725 recompiled and 210 runtime bindings), with zero skips,
decode failures, unhandled instructions, correctness-critical guest fallbacks,
failures, or errors, and records 389,841 resumable entries. Its executable-byte
coverage is 70.0729%, with zero overlaps or uncovered direct targets/calls.
v137-v140
successively restored the complete live scheduler dispatcher at
`0x0039C4C0`, four omitted methods in active vtable `0x00472700`, the complete
effect-update owner at `0x0019C600`, and the complete effect-command owner at
`0x0019F1E0`. The v140 replay reached read 481 at 25.437 seconds, passed those
owners, and at 32.481 seconds reached the analyzer's false end at `0x002C0A3C`.
Exact ELF review proves complete vtable-referenced object-behavior owner
`0x002C0A30-0x002C1758`: all 842 instructions are reachable, all 163 direct
branches are contained, its ten-entry computed dispatch is bounded internally,
and it has one return plus delay slot. Clean v141 also restores the only two
uncovered sibling tail adapters in that exact vtable family. Its standalone
recompilation processes 10,930 functions (10,720 recompiled and 210 runtime
bindings), records 389,827 resumable entries, and reports zero skips, decode
failures, unhandled instructions, correctness-critical guest fallbacks,
  failures, or errors. The v145 Windows runner builds, links, and cold-boots.
Exact `sceVu0Normalize` differential work also corrected
the reusable runtime to normalize XYZ only and clear W, matching the PS2 oracle
bit-for-bit at the live post-New-Game loader gate. Targeted verified-ELF tracing
now proves native calls the high-level `OPENING.SFD` owner `0x0012CB30`, passes
exact string address `0x0044E888` to `0x002B6D10`, and successfully opens it
through file-service method `0x0016B1F0`, which returns handle `0x0082A940`.
The remaining movie blocker was narrowed to an inherited false instruction
patch: v142 replaced real pool-capacity store `sw v1, 4(s0)` at `0x00259F20`
with NOP. v143 excludes that patch, writes capacity 8 at `0x003EA2A4`, returns
a real decoder slot from `0x0025A0D0`, executes `sceMpegCreate`, and returns a
nonzero decoder from `0x00238BF0`. The same replay then exposed omitted live
MPEG callback `0x00241580` through `0x00243148`. Exact live-record tracing
found all four uncovered callbacks at `0x00241560`, `0x00241580`,
`0x002415A0`, and `0x002415C0`; v144 includes their complete reviewed bounds.
Cross at read 481 and the corrected Start then Triangle
sequence at reads 720 and 745 were delivered successfully, but actual movie
presentation and A/V synchronization are not yet proven.
The former unhandled SNDDRV SID `0x77777778`, command class `0x00120000`, does
not recur.

The last controller blocker was a libpad2 result-pointer interpretation error.
`scePad2Read` receives the address of the result member at `0x004F1550`; the
surrounding game object begins at `0x004F1540`. The game callback therefore
reads the active-low word at result offsets 0/1, not 16/17. The reusable bridge
now writes buttons at result offsets 0/1, right-stick X/Y and left-stick X/Y at
offsets 2..5, and pressure bytes at offsets 6..17 for button bits 4..15. The
previous omission of the four stick bytes left them at zero, which the game
correctly interpreted as full left/up and converted into held mask `0x0009`;
that was the cause of the repeating menu navigation. A temporary generated
callback trace proved the
computed game output changed from `0xFFFF` to the injected `0x4008`; that trace
was removed before v119 was generated and linked.

v119 cold-booted from the verified ELF with combined Cross+Start pulses at
reads 0, 30, ..., 450 and an exclusive end of 451. Exactly 16 pulses were
logged, none afterward. The process remained responsive through a further
25-second post-cutoff idle at the real `NEW GAME / LOAD GAME / OPTIONS` menu.
`build/phase3/run-v119.stderr.log` contains no
`guest-branch:missing-target`, `Unimplemented PS2 stub`, fatal, error,
temporary, or fallback diagnostic. The local-only final framebuffer is
`UNEEDED images/phase3-final-menu-v119.png` (SHA-256
`4aa6c75bf2dcc9b823aa5510d82ede3e2f1c90fa7554e6bbb9fb50ca1827034b`);
its source PPM SHA-256 is
`3dc0829872d48c0d193d6c1c81466d3612242f8e117f604c6204c76143d19dba`.
This satisfies all three Phase 3 milestones on the available Windows x86-64
host. Linux x86-64 and macOS arm64 execution remain explicit platform-specific
verification blockers because those Tier 1 hosts are not available locally;
they remain carried with the existing Phase 1 CI task rather than reopening the
game-logic milestone.

The standalone Ghidra export plus reviewed ELF boundary and missing-function
waves produces a zero-unhandled-instruction recompilation. The generated
Windows runner builds, initializes OpenGL 3.3/audio, loads the verified ELF,
enters at `0x00100008`, installs its early syscall/TLB state, runs the static
initializers, constructs core objects, initializes SIF RPC, and loads twelve
observed IOP modules through MCSERV. A game-scoped CRI DTX profile now handles
the formerly blocking `sid=0x90000200`, `rpc=0x2` transport creation. Successive
v33-v42 cold boots exposed and passed live omissions at `0x001DB790`,
`0x0020E9A0`, and the `0x001C2930-0x001C2B64` vtable cluster. The v42 runner is
superseded by the linked and cold-booted v43 runner. Its 10,616-function map
covers 58.4898% of executable
bytes with zero overlaps, decode failures, or unhandled instructions. A stale
registration-time IRQ stack pointer was proven to overwrite a suspended main
thread frame; the runtime now isolates IRQ invocations on scheduler-owned async
stacks. After that fix, a bounded 15-second cold boot keeps both guest threads
alive and emits sustained GIF/GS traffic with no missing recompiled target. The
game-scoped CD-search service now returns the oracle-verified DATA.CVM disc
record and registers its exact extracted-file LBN range for bounded sector
  reads. That exposed and corrected the severely truncated 7,024-byte loader at
  `0x00169680-0x0016B1F0`. The game-scoped SNDDRV SID `0x77777777` compatibility
  service forwards its still-partially-characterized command stream through the
  portable audio seam. A bounded v100 cold boot reaches and reconstructs
the 640x448 MPEGSoftdec/ADX splash correctly through T8 upload, CSM1 CLUT
expansion, CT24 rasterization, and the final host presentation buffer. The
reusable `sceGsSetDefDispEnv` stub now initializes the complete five-qword
display environment, so both game double-buffer pages carry `PMODE=0x8005` and
the staged runner reaches the same intact splash after removal of the PMODE-zero
presenter fallback. Clean v57 contains 10,628 recompiled functions (3,966
analyzer plus 6,662 manual entries), restores the active event-runtime
initialization path, creates all seven observed guest threads, and naturally
processes the early one-sector read wave. A PCSX2 differential then disproved
the suspected interrupt-return scheduling exception: at the five-sector wait,
the oracle kept main at priority zero and all four engine workers at priority
one, while native had left those workers at 8, 16, 18, and 25. The actual defect
was the reusable `ChangeThreadPriority` syscall returning `KE_OK` instead of the
previous priority. The game saves that return and later uses it to restore the
temporary priority change; native therefore tried to restore priority zero,
received `KE_ILLEGAL_PRIORITY`, and retained the temporary priorities. v61
returns the prior priority for both normal and interrupt-safe variants. Its
strict-priority, no-pump cold boot naturally issues the five-sector read at tick
240, then the 328-, 66-, 98-, and 141-sector reads, and sustains execution to
tick 1440 with 227 DMA and 121 GIF submissions. The address-specific pump
dependency is removed without changing scheduler policy; gameplay is not yet
established.

## Verified local inputs

- Workspace: `C:\Users\johnn\Documents\ChatGPT\HG`
- Game serial/executable: `SLUS_210.75`
- `SYSTEM.CNF`: version 1.01, NTSC
- ELF SHA-256: `3b374d53a499d2c17b205274ee9eb34280768f294f970ebf6ae6731f6a2dacb8`
- ISO SHA-256: `3f8c658eee1b35147672c597c0dbe8e36552172abf83571c52753eec9a50bcca`
- Local PCSX2 oracle: 2.6.3
- Local game dump, ISO, BIOS files, PCSX2 distribution, modules, and memory-card
  directories exist and are intentionally untracked/ignored.

## Environment findings

- Git is available.
- Visual Studio Community 2022 17.14 is installed with MSVC 19.44, bundled CMake
  3.31.6, Ninja, and Windows SDK 10.0.26100. The tools are not on the default
  shell `PATH`, so local commands currently use their absolute paths.
- Working Python runtimes are available (bundled 3.12 and local 3.14 discovered
  by CMake).
- Ghidra 12.1.3 is installed outside the repository under the user tool cache;
  its official archive SHA-256 was verified as
  `93a5d11a9ad510622acaaf908c556a7b9b764d338e78a7567f3689bf5081fd54`.
  Analysis uses JDK 21.0.8. The local Ghidra project and input hardlink are also
  outside the repository.

## Decisions

- Ghidra is allowed as an offline analysis aid and may export function facts.
  The actual assembly-to-C/C++ recompilation step must consume only the verified
  ELF and standalone config/map files; it may not require or query a Ghidra
  project. Use PS2Recomp's native analyzer as a complementary coverage check.
- Keep human corrections in a build-scoped manual TOML overlay and generate the
  effective PS2Recomp config/map. Do not hand-edit generated analyzer output.
- Treat Windows x86-64, Linux x86-64, and macOS arm64 as initial Tier 1 targets;
  design portable interfaces for more targets and promote them after validation.
- Use OpenGL for rendering and keep it behind a renderer boundary.
- Retain only the newest ten disposable `build/phase3/recompiled-vN` generated
  trees. Preserve versioned maps, provenance reports, recompilation logs, and
  replay logs as compact evidence. The guarded staging-tool option requires an
  exact newest `recompiled-vN` sibling before pruning.
- PCSX2 may be used as both a guest-visible oracle and an open-source reference.
  Keep the result a focused static recompilation, track exact source provenance,
  preserve required notices, and avoid untracked verbatim copying.

## Immediate next actions

1. Implement a cached/native block executor for the demonstrated hot VU1
   microprogram starts `0x288` and `0x60`. Differentially compare vector and
   integer registers, ACC/Q/P, flags, stalls, branches, memory writes, and
   XGKICK packet order against the scalar interpreter before live enablement.
2. Compile the exact seven-block, 71-pair captured CFG together with its PATH1
   timing points. Remove repeated per-pair hazard scans and queue traversal only
   where the analyzed schedule proves the same issue/commit cycles; retain the
   scalar path for every unmatched hash, unsupported state, or failed guard.
3. Run the complete `PS2VU1` hazard/timing suite after each change, then repeat
   the same live profile and compare normalized VU cost, VIF time, controller
   cadence, XGKICK packet bytes/order, and rendered output.
4. Audit the OpenGL hybrid backend's source/destination alias predicate,
   cross-thread state, and display-thread work budget. Retain ordered CPU
   fallback and explicit readback for every unsupported or overlapping state.
5. Re-run the deterministic gameplay route without profiling. Require an
   approximately 29.97-update/s cadence, coherent frames, and zero flashing or
   missing geometry before declaring the speed gate complete.
6. Resume remaining Phase 4 gates only after practical gameplay timing:
   natural movie/A/V oracle validation, genuine asynchronous ordinary reads,
   game-created portable/endian-stable saves, and separate-process cold reload.
7. Convert stable oracle facts into automated differential checkpoints and run
   the Tier 1 build matrix; record unavailable Linux/macOS hosts explicitly.

## Known blockers and risks

- The user's active priority blocker is gameplay throughput. The live OpenGL
  raster subset is fast enough for the submitted workload, while scalar VU1
  execution consumes 90.5% of the matched VIF window. Correct native/cached VU
  block execution and PS2-style stage overlap are required; skipping draws,
  guest updates, timing, flags, stalls, or packets is not acceptable.
- Later channel-restoration writes observed after the synchronous drain contain
  implausible state (`MADR=0x00080076`, `QWC=0x000CD538`). Their cause must be
  resolved through hardware/oracle comparison, not by masking them or ignoring
  selected transfers without evidence.
- Opening video decode does not establish audio correctness. CRI/ADX/SNDDRV
  synthesis, mixing, timing, and A/V synchronization remain incomplete.
- Game-created saves and separate-process cold-boot reload remain unproven even
  though memory-card protocol/build work exists.
- Linux x86-64 and macOS arm64 execution are not available locally and remain
  explicit Tier 1 verification blockers. Windows-only success cannot close the
  cross-platform exit criterion.
- Exact portability still depends on alternatives for host SIMD assumptions and
  complete CI coverage. Accuracy also depends on incomplete PS2 hardware areas,
  especially GS, VIF/VU, DMA ordering, and interrupt timing.
- Missing-target diagnostics and temporary compatibility services are discovery
  tools, not correctness. Every reachable target and guest-visible service must
  have an evidence-backed boundary or implementation; no forced scheduler,
  movie, readiness, or save state is permitted.
- Disc assets, ELF/ISO/BIOS inputs, PCSX2 binaries, generated recompiled game
  code, traces, saves, and captures must remain ignored and local.

## Work log

- 2026-08-27: Began Phase 4 controller/New Game work. Corrected the libpad2
  payload to include the four analog-axis bytes; their zero-filled values had
  produced a permanent full-left/full-up state and menu auto-cycling. PadInput
  is now 16/16, neutral cold boot holds on NEW GAME, and a deterministic single
  Cross at controller read 481 dispatches the New Game path. Added two reviewed
  high-confidence ELF boundaries, `0x002C9470-0x002C9480` and
  `0x003A1990-0x003A19B0`; clean v93 recompilation processes 10,731 functions
  (10,524 recompiled and 207 bindings) with no skips, decode failures,
  unhandled instructions, critical fallbacks, failures, or errors. A v92
  baseline reached read 481 in approximately 30.2 seconds; v93 reached it in
  30.59 seconds after removal of a redundant pad-buffer mutex/copy from every
  guest dispatch, ruling that refresh out as the main startup bottleneck while
  retaining scheduler and overlapping-read refreshes. The next live stop at
  `0x003A07E8` proved the analyzed `0x003A07D0` function was truncated at its
  first EE-specific SQ. Added a high-confidence override through the verified
  JR-ra/delay-slot epilogue at `0x003A0F88`; clean v94 retains 10,731 functions
  and increases resumable entry coverage from 298,369 to 298,863. v94 then
  passed that body and called vtable slot `0x00122A30` from `0x003A0B04`.
  Verified the complete frameless initializer through its tail jump and delay
  slot at `0x00122AC0-0x00122AC4`; clean v95 processes 10,732 functions
  (10,525 recompiled and 207 bindings), with 298,901 resumable entries and no
  skips, decode failures, unhandled instructions, critical fallbacks, failures,
  or errors. v95 then reached a direct call from `0x0012265C` to the omitted
  floating-point helper at `0x0031C338`; its verified branches converge on the
  JR-ra/delay-slot epilogue at `0x0031C3B8-0x0031C3BC`. Clean v96 processes
  10,733 functions (10,526 recompiled and 207 bindings) with 298,903 resumable
  entries and all critical report counts still zero. v96 then directly called
  the omitted `0x0031BAF0` normalizer from `0x0031C3AC`; verified its complete
  classification/polynomial body through the common return and stack-restore
  delay slot at `0x0031BD7C-0x0031BD80`. Clean v97 processes 10,734 functions
  (10,527 recompiled and 207 bindings), has 298,904 resumable entries, and keeps
  every critical report count at zero. v97 then indirectly called the omitted
  frameless interval method at `0x00121B40` from `0x00122840`; verified its
  contained branch graph and return/delay slot at `0x00121C48-0x00121C4C`.
  Clean v98 processes 10,735 functions (10,528 recompiled and 207 bindings),
  retains 298,904 resumable entries, and keeps all critical counts at zero.
  A deterministic-unity-bucket experiment did not shorten live-boundary
  iterations because every generated source includes the global generated
  declaration header; each map addition therefore invalidates every object even
  when bucket membership is stable. The experiment was reverted and the faster
  original batch/PCH configuration restored. Removing the generated header from
  the PCH exposed two obsolete runner-local header copies left by old staging;
  the staging tool now removes only those exact shadows. Its focused regression
  suite passes 3/3 and preserves unrelated runner support headers.
  v98 next called vtable target `0x00122090` from `0x001229E4`. Verified the
  complete seven-member copy-adapter family surrounding that slot, bounded by
  already mapped methods at `0x00122010`, `0x00122030`, and `0x001220A0`.
  Clean v99 processes 10,742 functions (10,535 recompiled and 207 bindings),
  retains 298,904 resumable entries, and keeps every critical report count at
  zero. v99 then entered `0x0033D9F0` from `0x001B5EC0` and stopped at its
  truncated analyzed end `0x0033DA0C`. Verified the complete 0x2D0-byte
  stream/resource setup frame through its register restores and return/delay
  slot at `0x0033E294-0x0033E298`; all 17 direct callees are already mapped.
  Clean v100 retains 10,742 functions (the override replaces its short analysis
  record), 10,535 recompiled functions, 207 bindings, and zero critical report
  counts while increasing resumable entries to 299,459. v100 then entered the
  render-packet setup at `0x002699D0` and stopped at the first unsupported EE
  save at `0x002699E4`; verified its complete `0x3A0`-byte frame through the
  return/delay-slot pair at `0x0026B16C-0x0026B170`. Clean v101 retains 10,742
  functions, 10,535 recompiled functions, and 207 bindings with all critical
  counts at zero while increasing resumable entries to 300,972. Its single-Cross
  replay reaches read 481 at 30.800 seconds and next stops at `0x00130B08`,
  inside the analyzed `0x00130AF0` state dispatcher. Verified the complete
  `0x190`-byte frame through `0x001364EC`; all 257 direct calls resolve to 33
  mapped targets. Clean v102 increases resumable entries to 306,732 and clears
  that dispatcher. It next called the omitted `sceGsSetDefLoadImage` boundary
  at `0x0010CCB8`; v103 records the existing runtime binding explicitly through
  the verified `0x0010CE9C` end. v103 then called the two-instruction vtable tail
  adapter at `0x003854B0`; clean v104 processes 10,744 functions (10,536
  recompiled and 208 bindings) with zero critical report counts and restores
  that thunk through `0x003854B8`. Its replay reaches read 481 at 30.378 seconds
  and next calls omitted stream adapter `0x0021AF90` from `0x003A0558`; the v105
  boundary through `0x0021AFBC` is verified and ready for regeneration.
- 2026-08-27: Added opt-in exclusive/inclusive guest-dispatch profiling via
  `PS2X_DISPATCH_PROFILE_SECONDS` and DMA-stage timing via `PS2X_DMA_PROFILE`.
  A 10-second v104 cold-boot profile attributes 2.920 seconds to 285 calls of
  `sceDmaSend@0x0010D6E8`; the next-largest measured body is 0.218 seconds.
  Per-stage DMA evidence shows the cost is inside the CHCR write's synchronous
  GIF/VIF processing (typically 15--24 ms per submission), not the explicit
  follow-up drain or interrupt handlers. Corrected the reusable DMA wrapper to
  emit the ELF-verified CHCR values `0x105` for `sceDmaSend` and `0x101` for
  `sceDmaSendN`, removing an incorrect TIE bit, and wired the existing tested
  native GIF image/packed fast paths before generic fallback. The sampled game
  chains do not match those narrow fast paths (`native_gif=no`), so this is a
  correctness fix and diagnostic result, not yet a startup-speed resolution.
  PS2Memory remains 47/47. An experiment removing the generated global function
  declarations include was reverted after compilation proved tail adapters call
  peer generated functions directly; no failed emitter change remains.
- 2026-08-27: Clean v105 processes 10,745 functions (10,537 recompiled and 208
  bindings), records 306,743 resumable entries, and advances NEW GAME from the
  prior stream-service failure to omitted object-initialization adapter
  `0x00224380`. Clean v106 processes 10,746 functions (10,538 recompiled and 208
  bindings), records 306,762 resumable entries, and restores the verified
  `0x00224380-0x002243CC` body. Its deterministic replay reaches Cross read 481
  at 30.986 seconds and next calls `0x00225750` from `0x0017933C`. Verified-ELF
  inspection identifies that target as one member of fifteen uniform
  two-instruction a0-adjustment tail thunks at `0x00225680-0x00225768`, plus an
  otherwise-unmapped sibling initializer at `0x00224330`. v107 restores the
  complete family rather than discovering each slot separately. Its standalone
  recompilation is clean at 10,762 functions (10,554 recompiled and 208
  bindings), 306,782 resumable entries, and zero critical report counts. The
  v107 Windows runner rebuild was still compiling generated unity sources when
  the handoff was requested.
- 2026-08-27: v107 linked and its deterministic NEW GAME replay reached Cross
  read 481 at 30.738 seconds, then exposed omitted target `0x0020C380` from
  `0x001200C8`. Exact ELF inspection recovered the complete 29-member
  `a0 -= 0x0C` adjustment-thunk table at `0x0020C1D0-0x0020C398` and three
  otherwise-unmapped direct targets at `0x001FB230`, `0x001FBA20`, and
  `0x001FBA50`. Clean v108 processes 10,794 functions (10,586 recompiled and
  208 bindings), records 306,792 resumable entries, and has zero critical
  report counts. Its interrupted incremental build was recovered by compiling
  the missing final unity unit; no authored or generated sources were lost.
  Fresh dispatch and transfer profiling reproduced `sceDmaSend@0x0010D6E8` as
  the dominant guest body and localized its cost to CPU rasterization inside
  GIF-arbiter drain. Repeated 176-byte packets submit a 640x448 textured sprite:
  CT32 and T8 neutral MODULATE cases cost 14.7--21.3 ms through the generic
  per-pixel path. New opt-in `PS2X_TRANSFER_PROFILE` and
  `PS2X_GS_DRAW_PROFILE` diagnostics isolate transfer stages and slow draw
  state. A tightly guarded CT32/T8-to-CT24 linear-sprite path hoists invariant
  tests, coordinate wrapping, and T8 palette lookup without skipping GS work;
  those draws now cost 3.8--4.4 ms. A separately guarded neutral-black CT24
  output path removes texture sampling entirely and falls below the profiler's
  0.5 ms threshold. The next post-change aggregate removed the former 90-draw,
  1.409-second zero-alpha signature. Cached T4 palette/coordinate sampling keeps
  the generic alpha/depth write path and reduces the 512x512 sprite from about
  19.5 ms to 8.35 ms; cached T8 sampling keeps MODULATE/ALPHA_1 behavior and
  reduces active blended sprites from about 11.7 ms to 3.6--3.8 ms. Focused
  CT32-to-CT24, zero-alpha KEEP, T4 depth, and T8 blend regressions pass, and
  the full suite is 447/447. Final deterministic replays reach read 481 at
  24.673 and 25.040 seconds and the unchanged next target `0x002D69FC` at
  27.002 and 27.260 seconds
  (`build/phase3/phase4-new-game-map-v108-gs-fast-complete.log` and
  `build/phase3/phase4-new-game-map-v108-gs-fast-complete-repeat.log`). This is
  a repeatable 5.7--6.1-second, approximately 19% startup improvement; further
  optimization remains open.
- 2026-08-27: v109 extended the verified `0x002D69E0` NEW GAME object-update
  routine through its real return at `0x002D6AF4`. Its optimized replay reached
  controller read 481 at 24.911 seconds, passed the former `0x002D69FC` stop,
  and exposed indirect target `0x003A1B60` at 27.119 seconds
  (`build/phase3/phase4-new-game-map-v109.log`). Verified-U.S.-ELF inspection
  identifies that target inside a complete 29-entry adjustment-thunk table at
  `0x003A19B0-0x003A1B78`; every member tail-jumps with `a0 -= 0x40` in its
  delay slot and excludes two following padding words. A direct-target audit
  found 28 targets already owned and one complete omitted sibling at
  `0x0039A5B0-0x0039A5D8`. v110 records the whole family and sibling, validates
  3,951 analysis plus 6,873 manual functions, and recompiles 10,824 functions
  as 10,616 generated bodies plus 208 runtime bindings with 306,862 resumable
  entries and zero critical report counts. Staging/link/replay remains next.
- 2026-08-27: v110-v113 advanced the deterministic NEW GAME path through four
  independently verified boundary waves. v110 passed the 29-entry adjustment
  table and exposed the truncated resource setup at `0x00209390`; v111 restored
  that routine through `0x002097D4` and exposed the large command dispatcher at
  `0x002029B0`. Its 219-entry primary and eight-entry secondary tables target
  only its verified body, which returns at `0x00208064`; v112 restores it through
  the stack-delay slot at `0x00208068`. v112 then proved existing shared-tail
  cluster `0x001FFA70` was missing its real selector front at `0x001FF9E0`; the
  corrected v113 owner contains all 219 selector entries and reaches its shared
  return at `0x001FFB60`. v113 reaches controller read 481 at 24.961 seconds and
  advances to omitted vtable adapter `0x002A8590` at 27.215 seconds
  (`build/phase3/phase4-new-game-map-v113.log`). Exact ELF review identifies its
  sibling `0x002A85B0` and shared worker `0x002A8730-0x002A888C`; these are
  recorded for v114 but not yet generated. The loader path is active, but no new
  disc/CRI transition was emitted before this boundary. The MPEGSoftdec/ADX
  splash remains the only reconstructed movie-like presentation; the actual
  opening movie and audio/video synchronization are not yet working or claimed.
- 2026-08-27: v114-v116 passed three further deterministic NEW GAME boundary
  waves. v114 restored adapters `0x002A8590-0x002A85A4` and
  `0x002A85B0-0x002A85C4` plus their shared worker
  `0x002A8730-0x002A888C`; v115 extended the live seven-way token-state routine
  from its false end at `0x001214FC` through the verified return delay slot at
  `0x0012172C`; and v116 extended the 102-way object-command dispatcher from
  `0x001FC760` through its sole return delay slot at `0x001FF9D8`. Clean v116
  processes all 10,827 functions as 10,619 generated bodies plus 208 runtime
  bindings, records 316,070 resumable entries, and reports zero skips, decode
  failures, unhandled instructions, correctness-critical fallbacks, failures,
  or errors (`build/phase3/recompile-v116.log`). Its cold-boot replay reaches
  controller read 481 at 24.911 seconds and advances to the next false end at
  `0x00121388` at 27.165 seconds
  (`build/phase3/phase4-new-game-map-v116.log`). The finished diagnostic runner
  was stopped after capture. No intro movie, synchronized A/V, new gameplay
  load, or save/cold-boot reload success is claimed.
- 2026-08-28: v117-v119 passed three additional deterministic NEW GAME
  boundary waves. v117 extended analyzed token-stream owner
  `0x00121380-0x00121388` through its verified return delay slot at
  `0x001214E4`, after validating all 12 table entries and direct branches.
  v118 restored the file-backed vtable tail thunk `0x00166140-0x00166148`, and
  v119 restored indexed callback adapter `0x002B11A0-0x002B11C8`. Clean v119
  processes 10,829 functions as 10,621 generated bodies plus 208 runtime
  bindings, records 316,160 resumable entries, and reports zero skips, decode
  failures, unhandled instructions, correctness-critical fallbacks, failures,
  or errors (`build/phase3/recompile-v119.log`). Its replay reaches controller
  read 481 at 25.225 seconds, passes both restored adapters, and exposes the
  next analyzed false end at `0x00201BA8` at 27.435 seconds
  (`build/phase3/phase4-new-game-map-v119.log`). Exact ELF review proves that
  owner continues to `0x002029B0`; the v120 correction is recorded but not yet
  generated. The recurring SNDDRV `sid=0x77777778`, `rpc=0x120000` service remains
  unresolved, and no intro movie, synchronized A/V, gameplay load, or save
  round-trip success is claimed.
- 2026-08-28: v120-v122 advanced through two larger false boundaries and a
  conservative omission batch. v120 extended analyzed record-command owner
  `0x00201B90-0x00201BA8` through its return delay slot at `0x002029AC`. A
  minimum-density verified-ELF pointer scan found 60 remaining file-backed
  code-like targets; the strict missing-pointer audit admitted 51 exact
  return-bounded owners and rejected nine ambiguous candidates for v121
  (`build/phase3/data-function-pointers-v120.json` and
  `build/phase3/missing-pointer-functions-v120.json`). v121 passed the prior
  boundary and reached false end `0x00175440`; the delay-slot-aware boundary
  audit proved owner `0x00175430-0x00175DE0` and recorded it for v122
  (`build/phase3/function-boundaries-v121.json`). Clean v122 processes 10,880
  functions as 10,672 generated bodies plus 208 runtime bindings, records
  319,834 resumable entries, and has zero skips, decode failures, unhandled
  instructions, correctness-critical fallbacks, failures, or errors. Its
  replay reaches read 481 at 25.220 seconds, passes the restored dispatcher,
  and exposes omitted indirect target `0x001225A0` at 27.551 seconds
  (`build/phase3/phase4-new-game-map-v122.log`). Exact ELF review proves the
  four-instruction tail adapter ends at `0x001225B0`; clean v123 recompilation
  processes 10,881 functions as 10,673 generated bodies plus 208 bindings and
  is staged. At the user's request its Windows unity build was paused cleanly
  after `unity_276_cxx.cxx`; rerunning the normal build command resumes from
  existing objects. The recurring SNDDRV transfer RPC
  remains unresolved; no actual opening movie, synchronized A/V, gameplay load,
  or save/cold-boot round trip is claimed.
- 2026-08-28: v123-v125 resumed and passed three more exact boundary waves.
  v123 restored the four-instruction vector tail adapter
  `0x001225A0-0x001225B0`. Instruction-aware review then removed three v121
  instruction-word collisions, restored their complete owners, admitted five
  exact file-backed callbacks, and batched the 29 uniform indexed adapters for
  v124. Its replay reached the false end at `0x0039EAB8`; v125 extended owner
  `0x0039EAB0` through its return delay slot at `0x0039FD24` and also admitted
  all 32 directly referenced strict boundary-audit results. Clean v125 processes
  10,915 functions as 10,705 generated bodies plus 210 runtime bindings, records
  353,222 resumable entries, and reports zero critical counts. Its cold-boot
  replay reaches read 481 at 25.339 seconds and the next false end
  `0x00174934` at 27.680 seconds. Exact review records owner
  `0x00174920-0x0017542C` for v126. The other 54 permissive indirect-only
  candidates have neither mapped direct inbound references nor file-backed
  pointer evidence and remain excluded.
- 2026-08-28: Corrected `sceMcGetDir` for nonpositive `maxent` in both canonical
  and staged runtimes. It now accepts the command with result zero before path,
  card-state, directory-table, or host-filesystem access, matching the official
  PS2SDK `_McGetDir` loop. A missing-path sentinel regression covers `-1` and
  zero and passes. The full runtime suite reports 447/448 passing; the sole
  failure is the pre-existing working-directory-sensitive generated-header
  readability case, while the new memory-card case passes explicitly in
  `build/phase3/ps2x-tests-v125-mc-getdir.log`.
- 2026-08-28: v126-v128 continued the deterministic NEW GAME boundary proof
  and replaced the recurring unhandled SNDDRV transfer with an evidenced
  implementation. v126 extended live callback owner
  `0x00174920-0x0017542C`; its replay then reached uncovered direct call
  `0x0019A2B0`. A fresh direct-target audit admitted exactly five complete
  targets for v127, whose replay reached read 481 at 25.184 seconds and then
  false end `0x0016D6E0` at 27.458 seconds
  (`build/phase3/phase4-new-game-map-v127.log`). Exact ELF review extended the
  40-way dispatcher `0x0016D6D0` through its sole return delay slot at
  `0x0016F414` for clean v128: 10,920 processed, 10,710 recompiled, 210
  bindings, 355,817 resumable entries, and zero critical report counts
  (`build/phase3/recompile-v128.log`). Separately, exact SNDDRV.IRX analysis
  established that SID `0x77777778`, class `0x00120000`, copies request words
  2-4 from SIF IOP staging RAM to SPU2 RAM through `sceSdVoiceTrans`, returns
  `0/-1/-2/-3`, and uses request word 5 to select the live 100-us polling
  completion path. The game-scoped service now performs that bounded copy,
  uses explicit little-endian request/response handling, and schedules the
  NOWAIT guest callback after the response becomes visible while retaining the
  client's busy state. Both the service transfer/status test and SIF callback-
  order test pass; the full suite is 449/450 with only the known working-
  directory generated-header failure
  (`build/phase3/ps2x-tests-v127-snddrv-deferred-r2.log`). The v127 cold boot
  no longer logs the former SID `0x77777778` unhandled event. Audible output,
  opening-movie playback/A-V synchronization, gameplay load completion, and
  save cold-boot reload remain unproven.
- 2026-08-28: v129-v131 advanced the deterministic NEW GAME proof through two
  further analyzer truncations. v129 restored both missing tail adapters in the
  active `0x00469D10` vtable family; its replay reached read 481 at 25.100
  seconds and stopped at false end `0x00187674`. v130 extended exact owner
  `0x00187650-0x00188274`, including its contained 11-entry table; its replay
  reached read 481 at 25.205 seconds and stopped at false end `0x0017AEE4`.
  v131 extends the complete frameless geometry owner through return delay slot
  `0x0017B058` and batches the sole uncovered target in active vtable family
  `0x0046AAB0`. Clean v131 processes 10,923 functions (10,713 recompiled and
  210 bindings), records 356,597 resumable entries, and has zero critical
  report counts; its Windows build is active. Memory-card work now carries all
  six `sceMcGetDir` arguments through MCSERV, performs the actual directory
  operation, encodes the 64-byte PS2 directory record explicitly little-endian,
  and verifies a 16-byte literal save payload across libmc cold reinitialization.
  The uncontended full suite is 450/451 with only the known generated-header
  working-directory failure
  (`build/phase3/ps2x-tests-v130-mc-endian-cold-r2.log`). Game-driven save
  creation and a separate-process cold boot reload remain required.
- 2026-08-28: v131-v133 advanced through two more live object-update boundary
  failures without introducing stubs. v131 replay reached read 481 at 24.852
  seconds, passed the restored geometry routine and active-vtable adapter, and
  exposed false end `0x001A12C0`; v132 restored exact owner
  `0x001A12B0-0x001A1858`, including its contained seven-entry table. v132
  replay reached read 481 at 25.150 seconds, passed that owner, and exposed
  false end `0x001869E4`; v133 restores exact owner
  `0x001869D0-0x0018764C`, including 181 contained branches and its ten-entry
  internal table. The v132 all-prologue audit also found exactly two newly
  direct-referenced conservative truncations, `0x00215130-0x00215D7C` and
  `0x0027AD80-0x0027B810`; both are batched into v133 after explicit
  delay-slot/control-flow review. Clean v133 processes 10,923 functions
  (10,713 recompiled and 210 bindings), records 359,221 resumable entries, and
  has zero critical report counts. Its Windows runner build is active.
- 2026-08-28: v133 replay reached read 481 at 25.257 seconds, passed all three
  v133 owner corrections, and reached omitted indirect method `0x0026B350` from
  active vtable `0x0046D7B0`. Exact review establishes frameless owner
  `0x0026B350-0x0026B47C`, including 12 contained branches and the return delay
  slot. Intersecting the remaining conservative all-prologue boundary report
  with pointer-dense file-backed ELF data established 32 more entries inside
  vtable/callback clusters; all retain the strict boundary auditor's contained-
  flow, single-frame, return/delay-slot, and neighbor guarantees. v134 batches
  those 32 owners with the live method. Clean v134 processes 10,924 functions
  (10,714 recompiled and 210 bindings), records 387,780 resumable entries,
  covers 69.7801% of executable bytes, and has zero critical recompiler counts,
  overlaps, uncovered direct targets/calls, newly direct-referenced boundary
  candidates, or uncovered pointer targets. Its Windows runner build is active.
- 2026-08-28: v134-v136 advanced through three more evidenced NEW GAME
  boundaries without stubs. v134 replay reached read 481 at 25.316 seconds and
  false end `0x002CF3AC`; v135 replaced that short analysis record and an older
  internal-label range with complete two-table owner
  `0x002CF3A0-0x002CF6E4`. v135 replay reached read 481 at 25.639 seconds and
  omitted callback `0x001B8910`; v136 restored complete callback-descriptor
  siblings `0x001B8910-0x001B8C88` and `0x001A8040-0x001A82F4`. Clean v136
  processes 10,925 functions (10,715 recompiled and 210 bindings), records
  387,791 resumable entries, covers 69.8402% of executable bytes, and has zero
  critical recompiler counts, overlaps, or uncovered direct targets/calls. Its
  replay reached read 481 at 25.659 seconds, passed both callbacks, and exposed
  false end `0x0039C4DC` at 29.530 seconds. Exact review prepares complete
  scheduler-dispatch owner `0x0039C4C0-0x0039C5BC` for v137 and rehomes old
  `0x0039C510` internal case labels using the six-entry table at `0x00464400`.
  The staging utility now has a guarded `--retain-generated-versions` option;
  five focused tests pass, and the first retention run removed 40 exact old
  generated trees (7,155,363,012 bytes), retaining v127-v136 while preserving
  maps, provenance reports, and diagnostic logs.
- 2026-08-27: Completed Phase 3. An opt-in overlap trace proved the registered
  libpad2 result member was `0x004F1550` while the game callback object began at
  `0x004F1540`; the runtime had incorrectly added a second 16-byte header and
  returned neutral input to the callback. Corrected buttons to result offsets
  0/1 and pressure channels to 2..17 in button-bit order, retained the 34-byte
  result and reusable asynchronous refresh, and kept normal input injection-free.
  PadInput is 14/14, PS2Memory 47/47, PS2SifRpc 16/16 (including DBCMAN), and
  all 38 Python tests pass. The broader canonical PS2IopSubsystem test binary
  remains 7/10 because its three Haunting Ground cases require the game profile
  installed only in the staged game-specific runtime; this known harness split
  does not affect the linked runner or focused DBCMAN proof. Corrected three
  live-render callback ranges to include their delay slots, regenerated v88,
  and cleared every config/coverage/pointer/boundary/gap/classification gate.
  Final patch-free v119 logs exactly 16 finite Cross+Start pulses through read
  450, reaches the real main menu, and stays responsive for a 25-second
  post-cutoff idle with no critical diagnostics. Linux/macOS execution remains
  an explicit host-availability carry-over.
- 2026-08-27: Implemented bounded `sceDmaRecvN` SPR-from semantics and a passing
  wraparound/register-completion regression. v105/v106 then reached the title
  and real main menu, exposing live indirect target `0x001C4CE8`; restored its
  three-function helper family plus direct closure `0x001D6108`. Fresh v87
  recompilation is clean at 10,729 functions. Removed the hand-edited generated
  pad callback and moved asynchronous libpad2 DMA restoration into reusable
  runtime paths. After the user observed repeating menu navigation, added the
  exclusive `PS2X_PAD_PULSE_END_READS` cutoff; focused PadInput remains 14/14.
  v109/v110 confirmed the cutoff (16 pulses, last at read 450) but stalled at
  Progressive Scan, which motivated the exact guest-read refresh. The rebuilt
  v111 verification was stopped early for this handoff.
- 2026-08-27: Corrected the libpad2 read ABI and added an opt-in deterministic
  Cross pulse for cold-boot diagnostics. Focused `PadInput` is 14/14 passing;
  the pulse now advances the Progressive Scan prompt, MPEG Sofdec splash, and
  content warning. The next startup boundary is `sceDmaRecvN` at
  `0x0023C178`; its diagnostic success path prevents immediate termination but
  does not yet supply movie/audio receive data, so Phase 3 remains open.

- 2026-08-25: Read root handoff skill; inventoried workspace; verified game ID,
  hashes, PCSX2 version, and initial tools; created governance, roadmap, status,
  and ignore files.
- 2026-08-25: User clarified that both PCSX2 and PS2Recomp source may be
  referenced. Replaced the black-box-only rule with provenance and license-aware
  source-reference rules.
- 2026-08-25: User allowed Ghidra for analysis but prohibited the recompiler from
  directly depending on a Ghidra project. Added a standalone metadata boundary
  and a no-Ghidra clean-build exit test.
- 2026-08-25: Pinned PS2Recomp commit `14b1e5cb39b4af7e6fc12f9a29fdc751efde49d7`
  as a submodule. Added the CMake/OpenGL runner skeleton, cross-platform presets,
  CI matrix, standalone manual-function merger, and eight passing config tests.
- 2026-08-25: Built the pinned analyzer/recompiler with MSVC. Native analysis
  found 4,036 initial functions, 571 SDK matches, 212 known stubs, 37 patches,
  and 9 jump tables. First recompilation generated 4,039 files/310,658,065 bytes
  but logged 28,134 unhandled reports in four giant, data-consuming heuristic
  functions. Recorded details in `docs/ANALYSIS_BASELINE.md`; a complete Ghidra
  function map is the next correctness gate.
- 2026-08-26: Installed and hash-verified Ghidra 12.1.3 outside the repository,
  analyzed the verified ELF headlessly with JDK 21, and exported 7,014 standalone
  function records. Hardened the merger for discontiguous bodies, one-byte
  undefined bodies, duplicate names, automatic-name precedence, and boundary
  clamping; 15 unit tests now pass.
- 2026-08-26: The normalized Ghidra map generated 7,017 files/78,282,206 bytes
  with zero unhandled instructions and 2,324 unresolved-indirect-control-flow
  warnings. Staged the ignored output into a disposable PS2Recomp source copy
  and successfully linked the full Windows OpenGL runtime.
- 2026-08-26: First cold boot exposed a missing owner for the local-link startup
  instruction at `0x0010008C`. Added an evidence-backed manual override extending
  the entry bootstrap through `0x00100218`; regeneration still reports zero
  unhandled instructions. The rebuilt runtime ran for a bounded 20 seconds with
  no stderr diagnostics after initializing OpenGL, audio, and the guest entry.
- 2026-08-26: Added executable coverage reporting. The effective map has no
  overlaps; the initial report found 6,087 indirect `JR`/`JALR` sites and 642
  direct executable targets in unmapped bytes, establishing a ranked discovery
  backlog rather than a completion percentage.
- 2026-08-26: Used an isolated PCSX2 2.6.3/PINE cold boot as a memory oracle to
  verify the early syscall-table base and installed handlers. Added reviewed
  `kCopy`, `kFindAddress`, `GetEntryAddress`, `EIntr`, and `InitTLBFunctions`
  boundaries; details are recorded in
  `docs/oracle/2026-08-26-syscall-table.md`.
- 2026-08-26: Learned a second missing-function class from cold boot: a generic
  loop at `0x00100AE0` calls 143 monotonically ordered startup functions through
  the table at `0x00469460-0x0046969C`. Added validated `[[function_tables]]`
  config expansion with malformed/non-monotonic tests. v9 traversed the table
  and advanced into ordinary startup routines.
- 2026-08-26: v10 and v11 fixed two more evidence-backed startup gaps at
  `0x00105A00-0x00105B30` and `0x0010B140-0x0010B1F0`. v11 then reached the
  truncated epilogue of `0x00100660`; its correction is staged for v12. The
  config merger now has 17 passing unit tests.
- 2026-08-26: Added exact `[[function_sets]]` and
  `[[missing_function_sets]]` waves plus ELF-backed boundary, data-pointer, and
  missing-target audit tools. Direct-call closure added 58 safely bounded
  functions and corrected `0x00143840-0x00143D18`; v24 passed the former
  `0x0010B2E0` blocker and stopped at indirect target `0x00103A30`.
- 2026-08-26: Extended pointer discovery beyond empty `JR ra` methods. Added
  1,534 short straight-line vtable methods and 8 manually reviewed complex
  indirect methods, then corrected the newly exposed `0x00180D60` epilogue.
  v27 generated 9,647 files/111,893,115 bytes with 3,164 warnings and zero
  unhandled instructions; cold boot passed `0x00103A30` and exposed the
  truncated scheduler-resumed function at `0x00106B10`.
- 2026-08-26: Added an all-prologue ELF audit so indirect-only analyzed
  functions are checked too. Recorded 2,316 conservative Ghidra boundary
  corrections plus the manually reviewed `0x00106B10-0x00106C3C` function.
  The v28 map validates 3,988 analysis and 5,656 manual functions with zero
  overlaps, zero remaining all-prologue boundary candidates, and zero invalid
  manual ranges. Recompilation generated 9,647 files/166,615,000 bytes with
  4,458 warnings and zero unhandled instructions.
- 2026-08-26: v28 cold boot passed the scheduler resume and reached game-side
  initialization before stopping at vtable target `0x002D45A0`. A bounded-CFG
  frameless-method audit added that target and 305 analogous non-overlapping
  methods to v29; the effective map now validates 3,988 analysis and 5,962
  manual functions. Twenty-three Python tests pass. The 317 residual pointer
  targets include shared-suffix/jump-table-label patterns and remain unmerged
  pending classification.
- 2026-08-26: Added a conservative effective-gap leaf-chain audit. It recovered
  650 frameless functions with no direct call or file-backed absolute pointer,
  including live `0x0020E7D0`, then direct-call closure added `0x00217370` and
  `0x0025FF80`. Added a separate NOP-tolerant stack-frame pass for intentional
  polling/retry loops; it corrected 17 functions including scheduler-resumed
  `0x001BC0F0-0x001BC220`.
- 2026-08-26: The v32 effective map validates 3,971 analysis and 6,631 manual
  functions (10,602 total), 58.1772% of the executable segment, zero overlaps,
  zero all-prologue/NOP-retry candidates, zero gap-leaf candidates, and zero
  invalid manual ranges. Recompilation generated 10,605 files/170,930,506 bytes
  with 4,488 warnings and zero unhandled instructions; the Windows OpenGL runner
  linked successfully. A 30-second cold boot reported no missing guest branch,
  initialized SIF RPC, and loaded five IOP modules before stopping on unhandled
  DBCMAN/DTX RPC `sid=0x90000200`, `rpc=0x2`.
- 2026-08-26: Added a project-owned Haunting Ground IOP profile that is injected
  only into the disposable runtime source copy, leaving the pinned PS2Recomp
  submodule clean. Its CRI DTX service claims `sid=0x90000200`; v33 passed DTX
  creation, initialized PS2RNA farther, emitted GS DMA/GIF traffic, and loaded
  twelve IOP modules. Added staging tests and a content-aware incremental mode.
- 2026-08-26: Failure-driven v33-v38 cold boots recovered five live omitted
  functions/methods at `0x001DB790`, `0x0020E9A0`, and
  `0x001C2930-0x001C2B64`. The next scheduler stop proved Ghidra had retained
  only six instructions of `0x001C1E00`; its verified body ends at
  `0x001C2928`. Direct-call closure added the adjacent
  `0x001C0D40-0x001C0F40` helper/adapter cluster. The v39 map validates
  3,970 analysis and 6,642 manual functions (10,612 total), covers 58.2912% of
  executable bytes, and has zero overlaps. Recompilation processed all 10,612
  functions with zero decode failures, zero unhandled instructions, and zero
  errors. Twenty-six Python tests pass.
- 2026-08-26: Linked and cold-booted v39. An isolated PCSX2/PINE experiment
  proved the launch descriptor is initialized to target `0x002D1E40` and later
  retargeted to `0x002D1B20`; native initialization copies the same three words
  bit-exactly. This disproved the earlier zero-descriptor blocker hypothesis.
- 2026-08-26: Traced the native main-thread loss to IRQ handlers reusing a stale
  registration-time stack pointer and overwriting the suspended frame below
  `0x001AAC40`. Updated the PS2Recomp scheduler to use its async invocation
  stack for IRQ handlers and added a passing frame-clobber regression. A
  15-second v39 cold boot then kept both threads alive, emitted sustained GS
  traffic, and reported no missing recompiled target. The next boot blocker is
  the unhandled `sid=0x80000597` request for `\DATA.CVM;1`.
- 2026-08-26: Characterized `sid=0x80000597`, function 0 with an isolated
  PCSX2/PINE cold boot. Its response is a 36-byte `sceCdlFILE`-style record whose
  LBA, size, name, and date match the ISO9660 `DATA.CVM;1` entry. Added a generic
  size-validating CD-search IOP service and exact Haunting Ground bindings;
  focused success and mismatched-data tests pass.
- 2026-08-26: Failure-driven v40-v42 boots recovered the live omitted function
  at `0x001ED3C8` and file-adapter targets `0x001E6198`, `0x001E61B0`, and
  `0x001EE6F8`. The v42 map validates 3,970 analysis and 6,646 manual functions
  (10,616 total), covers 58.2976% of executable bytes, and recompiles with zero
  overlaps, decode failures, unhandled instructions, or errors. A 20-second
  boot has no missing target, keeps two guest threads active, emits 17+ GIF
  packets, and exposes the next concrete gap: resolving the returned DATA.CVM
  LBN through `sceCdRead`.
- 2026-08-26: Added an exact, size-checked, overlap-rejecting LBN-to-extracted-
  file registration seam. Focused IOP and sector-offset tests pass, and v42 no
  longer reports unresolved LBN `0x00164972`. The resulting live scheduler
  target proved Ghidra had retained only 12 bytes of the 7,024-byte loader at
  `0x00169680`; v43 now covers it through its verified `0x0016B1F0` boundary,
  raising executable coverage to 58.4898% with zero overlaps or recompiler
  errors.
- 2026-08-26: Sampled the MODHSYN command/response buffers during an isolated
  PCSX2 cold boot and observed a stable zero response. Added a clearly marked
  Haunting Ground boot-compatibility service that forwards `sid=0x77777777`
  commands through the portable audio seam. A bounded 30-second v43 cold boot
  has no unresolved disc read, unhandled RPC, or missing guest branch target.
- 2026-08-26: Traced the next v43 stall to the game request at `0x003B4728`.
  The blocking loop at `0x00169384` only queries status through `0x001CA1F0`;
  the state-advancing path is the existing subsystem pump
  `0x001CA4C8 -> 0x001C9F10 -> 0x001C9E48`. The associated queue functions,
  including `0x001C8878` and `0x001C8960`, are present in the verified ELF and
  effective map, so this is not another omitted-function case. Instrumentation
  now targets EE interrupt and GS VSync registration/delivery to identify the
  callback that should execute the pump while the main thread blocks.
- 2026-08-26: The 5.85 GB function trace disproved a general interrupt-delivery
  failure. The registered INTC cause-3 path (`0x001AAC30 -> 0x001CC5B0`) keeps
  executing near the end of the trace, while the last loader-pump call occurs
  near byte 19.9 MB and the remainder is the `0x001CA1F0` status loop. Static
  review shows the global pump also calls the lower file manager through
  `0x001D1D88`; that manager is responsible for advancing the linked request at
  `0x003C2E50`. The next bounded experiment will inject the game's own
  `0x001CA4C8` pump as a separate, register-preserving guest invocation only
  while the main context is stalled at `0x00169384`. This is a diagnostic to
  identify the missing asynchronous scheduling edge, not a correctness fix.
- 2026-08-26: Removed six ElfAnalyzer data-as-code phantoms from the reproducible
  recompiler inputs (`0x00146130`, `0x00188C10`, and the four already documented
  spans beginning at `0x003A1860`). `config/game.toml` now names the exclusions,
  the merger validates and emits them, and all 30 Python tests pass. A clean
  ignored output directory generated 10,617 C++ files / 170,961,620 bytes; the
  Windows OpenGL diagnostic runner linked successfully. Do not regenerate into
  a nonempty directory: PS2Recomp leaves obsolete C++ files behind, which can
  contaminate CMake unity builds.
- 2026-08-26: The isolated `0x001CA4C8` pump experiment proved that merely
  scheduling the global pump is insufficient. More than ten million pump calls
  left both request `0x003B4728` and lower object `0x003C2E50` at byte state
  `01 02 00 00`; the requested sector count remains five. The lower state
  handler `0x001D1BA8` only finalizes state 2 through `0x001D17B0` when byte
  `+0x49` becomes one, and that completion byte never changes. The native window
  remains solid white, so visible boot has not been reached. The immediate next
  action is to trace the owner/callback that sets lower-object byte `+0x49` and
  compare that completion event against PCSX2; repeated pump injection is a
  diagnostic dead end and must not become a compatibility fix.
- 2026-08-26: A stricter isolated-pump invocation changed the earlier result:
  serializing queued asynchronous callbacks instead of nesting them on an
  already-active guest invocation eliminated `EE invocation stack space
  exhausted` and allowed the request chain to complete. The diagnostic pump is
  still game-address-specific and is not a correctness fix, but it proved the
  missing disc completion was an asynchronous scheduling edge rather than a
  missing CD byte write. The reusable scheduler change now drains queued
  invocations only when the running thread has no active invocation frames.
- 2026-08-26: The completed request chain read the original 5- and 2-sector
  requests, a 328-sector asset, and later 66-, 98-, 15-, 141-, 2-, 17-, 18-,
  and 129-sector requests. Failure-driven recovery added reviewed boundaries
  `0x0016BFB0-0x0016C530`, `0x002D4780-0x002D5130`, and
  `0x00380C00-0x00381BB0`. Clean v52 recompilation processed 10,616 functions
  with zero decode failures, zero unhandled instructions, and zero errors; all
  30 project Python tests pass.
- 2026-08-26: v52 sustained hundreds of DMA/GIF submissions and reached a
  640x448 host presentation. A temporary no-PMODE presenter changed the fallback
  from magenta to black, and bounded texture probes proved TBP0 8704 was zero.
  Transfer tracing found the cause in reusable VIF1 handling: DIRECT continuation
  detection inspected only the first GIF tag, while Haunting Ground emits setup
  A+D registers followed by a trailing IMAGE tag whose payload continues in the
  next VIF chunk. The generalized tag walker now detects that pending payload;
  a focused regression (`VIF1 DIRECT detects a trailing image tag after setup
  registers`) passes. Runtime uploads subsequently carried nonzero 1,024-,
  32,768-, and 286,720-byte payloads, texture probes became nonzero, and the
  native window displayed the first visible game-image fragments. The fragments
  remain corrupted; PATH2/GS format or CLUT state and guest PMODE are the next
  evidence-backed graphics blockers.
- 2026-08-26: v88-v93 isolated the corrupted first frame. A complete 286,720-byte
  T8 upload round-tripped through GS swizzled addressing with zero mismatches;
  the dumped index image, CSM1 CT32 palette expansion, CT24 sprite target, and
  final 640x448 host upload buffer all reproduce the MPEGSoftdec/ADX splash.
  The prior colored fragments were presentation-only: during the splash's
  expected black intermediate draw, the PMODE-zero diagnostic presenter searched
  other nonblack context frames and interpreted page 272 texture/render memory
  as the display. The disposable diagnostic presenter now keeps DISPFB1 page 0
  even when it is black. v93 records the intact host buffer at tick 248 with
  `displayFbp=0`, `sourceFbp=0`, and 25,947 nonblack pixels, with no page-272
  fallback. The Windows capture helper returned a blank OpenGL client surface,
  so the dumped host upload buffer is the visual verification artifact. This
  change remains confined to ignored `build/runtime-source`; it does not resolve
  guest PMODE or the game-address-specific pump dependency.
- 2026-08-26: v96-v100 closed the PMODE half of that diagnostic dependency.
  Caller tracing pinned both swaps to `0x001B8774`/`0x001B8730` and showed that
  the double-buffer pages at `0x007F1FF0` contained valid DISPFB/DISPLAY values
  but zero privileged fields. The reusable `sceGsSetDefDispEnv` stub had been
  preserving those zero/BSS fields even though the Sony routine consults
  `sceGsGetGParam`; it now initializes PMODE, SMODE2, DISPFB, DISPLAY, and
  BGCOLOR together. A new focused regression verifies `PMODE=0x8005`,
  `SMODE2=0x3`, width fields, and zero BGCOLOR after a reset. All 209 test cases
  report passed with zero reported failures; the Windows test process still
  exits with `0xC0000005` after the final reported case, so teardown remains to
  be diagnosed separately. v100 cold-boots with both swap pages at
  `PMODE=0x8005` after the staged PMODE-zero presenter fallback was removed, and
  dumps the intact splash at tick 248 from `displayFbp=0`, `sourceFbp=0`, with
  25,604 nonblack pixels. The remaining visible-boot dependency is the
  address-specific `0x001CA4C8` asynchronous pump injection.
- 2026-08-26: A new isolated PCSX2/PINE cold boot falsified the suspected
  VBlank callback edge. Guest word `0x003B83E0` remained zero while the real
  outer request at `0x003B4728` and lower object at `0x003C2E50` advanced from
  state 2 to state 3 and cleared, twice. Temporary native logging likewise
  observed no `sceGsSyncVCallback` or alarm registration. The retained bounded
  function trace narrows the natural path: after the diagnostic invocation
  first advances the outer request to state 3, the game calls
  `0x001C9800 -> 0x001D1560 -> 0x001D1720 -> 0x001CA4C8` itself. The remaining
  missing edge is therefore the initial completion for the request submitted
  through `0x001C9C50`, most likely in the CDVD/SIF completion seam, rather than
  periodic pump delivery. The reproducible oracle record is
  `docs/oracle/2026-08-26-loader-completion-callback.md`.
- 2026-08-27: v53 added six reviewed event/alarm thunks, but a no-pump boot
  registered no alarms and still stalled, falsifying the alarm hypothesis. v54
  recovered the category-4 and category-6 worker entries at `0x001CB3A0` and
  `0x001CB480`; diagnostics then showed that the game's `InitThread` helper at
  `0x0026CF78` had been incorrectly inherited as a return stub. The config
  merger now supports reviewed `exclude_base_stubs`, and clean v55 recompiles
  the real helper. The reusable scheduler now accepts the priority-zero EE idle
  thread created by libkernel; a focused C++ regression builds successfully.
- 2026-08-27: A verified-ELF writer audit of shared priority word `0x003BFBF8`
  found stores at `0x001CBE18` and `0x001CCA08`. The analyzer's inherited patch
  list had falsely replaced the active store at `0x001CBE18` with a NOP. The
  merger now validates `exclude_base_patches`, `config/game.toml` excludes that
  exact false patch, and a focused regression brings the config test suite to
  23 passing cases. Clean v57 restores the store and recompiles 10,628 functions
  with zero decode failures, unhandled instructions, or errors; its 10,629
  generated C++ files total 171,770,163 bytes.
- 2026-08-27: The v57 no-pump boot creates seven guest threads and naturally
  processes the early disc-read wave, eliminating the prior two-thread
  initialization failure. It later stalls on the five-sector request with main
  thread 1 running at `0x00169384`/priority 1 and category-4 thread 5 ready at
  the `SleepThread` return/priority 18. A staged-only diagnostic that grants
  bounded slices across ready priorities immediately completes that request and
  continues through later 328-, 66-, 98-, and 141-sector reads without invoking
  `0x001CA4C8` externally. The diagnostic scheduler change was reverted; the
  v60 wake probe further confirms that guest interrupt handler `0x001CB690`
  calls interrupt-safe `iWakeupThread` and makes category-4 thread 5 ready while
  main thread 1 remains current. The next correctness task is therefore to
  reproduce the real PCSX2 interrupt-return dispatch edge. See
  `docs/oracle/2026-08-27-loader-event-thread.md`.
- 2026-08-27: PCSX2 breakpoints at the exact five-sector wait and immediately
  after the guest `iWakeupThread` call showed main at priority zero and engine
  workers at priority one, falsifying the inferred interrupt-return exception.
  A breakpoint at `0x001CAFA4` then directly observed thread 4 receiving `v0=1`
  from `ChangeThreadPriority`. Generated code proves the game saves that result
  and feeds it to a later restore call. The runtime now returns the previous
  priority on success (and the kernel error on failure) for normal and
  interrupt-safe calls; its focused regression passes. The v61 strict-priority
  no-pump cold boot naturally completes the five-sector loader transition and
  all later 328-, 66-, 98-, and 141-sector reads observed in the earlier
  fairness experiment, sustaining 1,440 ticks without address-specific pump
  injection.
- 2026-08-27: Completed Phase 2. Classified live indirect target `0x001E4EE0`,
  closed the 314 remaining pointer-data candidates with 42 reviewed owner
  ranges plus dense internal-entry wrappers, closed all direct targets and
  conservative gap leaves, and added an all-byte classification gate. Promoted
  the approved standalone Ghidra CSV and analyzer base TOML into `config/`,
  added a per-boundary provenance report, and reran the pipeline without a
  Ghidra dependency. The final map has 10,660 functions and zero overlaps; all
  3,650,048 executable bytes are classified and every reachable-entry audit is
  clear. PS2Recomp completed with zero decode failures, unhandled instructions,
  correctness-critical fallbacks, or errors. All 38 Python tests pass; the new
  C++ dense pointer-entry regression builds and passes. A second isolated-input
  replay with no Ghidra project or prior analyzer outputs reproduced the same
  10,660-function, 3,650,048-byte result.
- 2026-08-27: Phase 3 began from the finalized Phase 2 inputs in a fresh staged
  runner. Bounded diagnostic cold boots first exposed the live 46-slot direct-J
  dispatch table at `0x00210610-0x002108E8`, its seven previously unowned tail
  targets, and the two container methods at `0x00168C80-0x001691B4`; all were
  recovered with exact ELF-backed, delay-slot-aware bounds. The resulting boot
  naturally reaches the earlier launch-descriptor checkpoint at `0x002D1B20`,
  begins sustained GIF/GS DMA, and then dynamically enters the scene callback
  table at `0x0046ADB0`. The next reviewed set restores its live method
  `0x001BE220-0x001BE47C` and adjacent tail thunk `0x001BE790-0x001BE798`.
  Recompilation now processes 10,717 functions (10,518 recompiled and 199
  runtime stubs) with zero skipped functions, decode failures, unhandled
  instructions, correctness-critical fallbacks, or errors. The unhandled
  SNDDRV `sid=0x77777778`, `rpc=0x120000` trace is observed repeatedly but has
  not yet blocked forward execution; the next executable action is to cold-boot
  the linked v73 runner and classify its first live stop.
- 2026-08-27: v74-v84 advanced through the controller and early scene setup.
  Seven reviewed libpad2 entry points now bind to a reusable portable socket,
  state, profile, and read bridge. Clean regeneration removed stale renamed C++
  files that had survived incremental output staging. A live vibration-profile
  gap at `0x002D25D8-0x002D2658` and four short vtable tail thunks at
  `0x00121960`, `0x001225C0`, and `0x001225D0` were recovered from exact ELF
  control flow. A PCSX2 differential proved graphics object `0x004F1920` must
  retain vtable `0x0046AC50`; the new guest-write watch found DBCMAN's unknown
  RPC stub echoing a packed request as a payload length and overrunning that
  object from destination `0x004F1782`. Unsupported DBCMAN calls now return an
  explicit empty payload, with a focused regression. v84 preserves the vtable,
  passes the formerly null indirect call, and reaches repeated draw-packet
  `cosf` call `0x0031C058`. The current effective map validates 3,959 analysis
  and 6,765 manual functions; the v84 recompilation processes all 10,724 with
  10,518 recompiled, 206 runtime stubs, zero skips, decode failures, unhandled
  instructions, correctness-critical fallbacks, or errors. See
  `docs/oracle/2026-08-27-graphics-vtable-initialization.md`.
- 2026-08-27: Phase 4 added a deterministic normal-controller replay source,
  configured as read-indexed, non-overlapping active-high button intervals in
  `PS2X_PAD_REPLAY`. Its focused `PadInput` suite passes all 16 cases, including
  malformed/overlapping input and isolation of the replay index to port 0,
  slot 0 (`build/phase3/phase4-pad-replay-tests.log`). A replay-only cold boot,
  with no diagnostic pulse injection, delivered 16 transitions and reached the
  real menu. `UNEEDED images/phase4-menu-replay-v4.png` has SHA-256
  `4aa6c75bf2dcc9b823aa5510d82ede3e2f1c90fa7554e6bbb9fb50ca1827034b`,
  byte-identical to the Phase 3 menu proof. This verifies the portable input
  source and deterministic delivery, but does not yet satisfy the menu-input
  exit criterion: isolated Up and Cross taps at reads 480 and 510 remained on
  `LOAD GAME` and began no new disc activity.
- 2026-08-27: Bounded pad-path diagnostics narrowed that Phase 4 blocker past
  transport. The normal backend delivered Up/Cross; libpad2 DMA exposed the
  corresponding active-low `0xffef`/`0xbfff` words at registered buffer
  `0x004F1550`; callback `0x001BE220-0x001BE47C` produced active-high
  `0x0010`/`0x4000` plus pressure 255 at output `0x004F1504`; and its normal
  reads returned zero to caller `0x002D47E4`. The remaining earliest-unblocked
  task is therefore downstream edge/event consumption in the per-frame input
  manager `0x002D4780-0x002D5130`, not controller mapping, DMA, or the libpad2
  result layout. The callback probes are generated-only diagnostics and must be
  removed before the next clean milestone build.
- 2026-08-27: The user reported that native startup is extremely slow relative
  to the normal opening and supplied `https://youtu.be/Uel67zFwdY8` as a visual
  reference. The current native replay takes roughly 145--155 seconds to reach
  controller read 500/the main menu. Scheduler inspection found that VBlank
  delivery requires both its 16.67 ms host deadline and about 4.9 million
  modeled EE cycles, while generated dispatch currently accounts only eight
  cycles plus sparse checkpoints. This can make the cycle gate dominate wall
  time. Existing VBlank coverage exercises an idle waiter only; the next
  executable action is a runnable-guest pacing regression followed by a
  narrowly scoped VBlank correction. A computed read watch also identified
  menu consumer PC `0x0012E9E4` reading `0x0047E37C`, but the watch did not log
  the returned value, so it does not yet prove that the consumer observed the
  input edge.
- 2026-08-27: Added a runnable-guest VBlank regression that failed under the
  prior dual host/cycle gate, then restricted host-deadline compensation to
  VBlank start/end and advanced modeled EE time at that boundary. The focused
  scheduler/interrupt suite now passes 11/11, including a second regression
  proving that a GS callback longer than one host frame is coalesced rather
  than duplicated ahead of resumed guest work
  (`build/phase3/phase4-vblank-coalescing-tests.log`). The first optimized
  Windows runner build reached controller read 510 in about 40 seconds versus
  135--155 seconds in Debug, establishing optimization as the dominant startup
  speed factor and VBlank under-accounting as a separate measurable defect.
  The exact menu reader at PC `0x0012E9E4` was then instrumented: object
  `0x00888440`, item `0x009C90E4`, selection 2 read edge `0x00000000`, although
  the preceding input-manager callback had produced Up/Cross edges. Duplicate
  GS callback coalescing removes unbounded callback starvation and passes its
  regression, but the real menu still samples after the edge has been cleared.
  The next executable action is to measure base-thread progress between GS
  callbacks and correct that scheduling/interleaving boundary. All
  `[pad-callback]`, `[pad-manager]`, `[pad-consumer]`, `[menu-input]`, and
  `[menu-action]` edits remain generated-only diagnostics and must be removed
  before a clean Phase 4 checkpoint.
- 2026-08-27: Value-aware reads at input edge word `0x0047E37C` established the
  exact field-ordering failure: the input manager writes a nonzero edge, its
  next field update clears it, and only then does the 30 Hz menu consumer read
  it. Shifting the deterministic Up and Cross pulses by one controller query
  made the ordinary game path read Up (`0x0010`), change the selection from
  `OPTIONS` to `LOAD GAME`, read Cross (`0x4000`), and dispatch action target `0x00130A40`
  (`build/phase3/phase4-menu-phase-aligned.log`). The first post-selection
  stop is therefore no longer input: call chain `0x00130A40 -> 0x00399920 ->
  0x00398850` reaches the analyzed function's false end at `0x00398860`.
  Verified ELF bytes show that Ghidra stopped after the four-instruction
  prologue at the first EE multimedia load; the same switch-driven stack frame
  continues to its restore/return epilogue at `0x00399900-0x00399918`, just
  before the next analyzed start at `0x00399920`. A build-scoped v78 boundary
  correction records that exact range. Extending the next live truncated state
  routine at `0x002BCAC0-0x002BD69C` then left the optimized runner responsive
  on the rendered `Load` memory-card slot screen for more than a minute with no
  missing target or fatal stub (`build/phase3/phase4-new-game-v90.log`). Visual
  inspection corrected the earlier menu-label inference: actual `NEW GAME`
  requires two Up transitions from the default `OPTIONS` selection. The next
  executable action is a cold boot with two phase-aligned Up pulses followed by
  Cross. Phase-independent menu-tap handling remains open.
- 2026-08-28: v137-v140 continued the deterministic NEW GAME boundary recovery
  without temporary success stubs. Exact verified-ELF review restored complete
  scheduler, active-vtable, effect-update, and effect-command owners; every map
  recompiled and linked with zero critical report counts. v140 processes 10,928
  functions, records 388,985 resumable entries, covers 69.9700% of executable
  bytes, and has zero overlaps or uncovered direct targets/calls. Its replay
  reached read 481 at 25.437 seconds and advanced to false end `0x002C0A3C` at
  32.481 seconds. The next v141 correction is exact owner
  `0x002C0A30-0x002C1758`, independently evidenced by five file-backed vtable
  slots and the live `0x0046F480` object table; all 842 instructions and 163
  direct branches were audited, including the ten-entry internal dispatch at
  `0x0045D280` and the final return delay slot.
- 2026-08-28: Clean v141 adds the complete live object-behavior owner plus two
  exact sibling tail adapters (`0x0034E5F0-0x0034E614` and
  `0x0035F9B0-0x0035F9DC`) that were the only uncovered code pointers around
  the five referencing vtables. Standalone recompilation processes all 10,930
  functions with 10,720 recompiled, 210 runtime bindings, 389,827 resumable
  entries, and zero critical report counts. Generated output is staged and the
  oldest retained generated tree was rotated; v141 has not yet been built or
  replayed.
- 2026-08-28: To bound disposable generated-source storage,
  `tools/stage_generated_runtime.py` gained guarded
  `--retain-generated-versions`. Its focused five-test suite passes. Staging now
  keeps the newest ten exact `build/phase3/recompiled-vN` trees and rotates one
  old tree on each iteration while preserving effective maps, provenance CSVs,
  and diagnostic logs. The initial safe cleanup removed 40 obsolete trees and
  reclaimed 7,155,363,012 bytes (about 6.66 GiB); later staging passes have
  continued the ten-version rotation.
- 2026-08-28: The staged v141 runner now builds, links, and cold-boots. Its
  deterministic NEW GAME replay reaches controller read 481 at about 25.4
  seconds and remains live beyond the former `0x002C0A3C` boundary with no new
  missing target, unhandled RPC, or reserved-VU failure. Correctness fixes made
  during this replay preserve the guest's non-MOD DMA CHCR bits (including VIF
  TTE), inject VIF tag data for every applicable chain tag, recognize GIF
  `IMAGE2`, and terminate oversized PATH1 XGKICK packets at the reserved VU
  boundary. Clean runtime verification passes 453/454 tests; the sole failure
  remains the known working-directory-sensitive `instructions.h` lookup.
- 2026-08-28: The post-NEW-GAME white diamond is now proven to be a defect, not
  a difficulty/input screen. Sparse second Cross pulses at reads 600 and 720
  were delivered without changing state, while the U.S. longplay oracle enters
  the opening presentation within roughly ten seconds of NEW GAME and shows no
  intervening difficulty prompt. Delayed dispatch, DMA, GS, and VU profiles
  show the white loop spending most wall time in synchronous VIF1/VU1 work;
  removing opt-in per-XGKICK trace overhead improved its diagnostic throughput
  without skipping work. No libmpeg binding is called in a 15-second white-loop
  window. A thread-state watch further shows only the main thread runnable;
  the remaining workers are stably asleep, suspended, or waiting on semaphore
  3, with no MPEG/external-I/O wait. The earliest unblocked action is therefore
  to trace the live `0x002C0A30` object state and its callers/condition results,
  identify the exact pre-movie completion condition that never changes, and
  correct the responsible runtime or service semantics before pursuing movie
  decode, A/V synchronization, and saves.
- 2026-08-28: Follow-up v141 tracing disproved two narrower explanations for
  the white transition. The scene-state word at `0x01512168` changes to 5 once
  at `0x002075C4`; handler `0x0019F8A0-0x001A0358` consumes it and resets it to
  0 at `0x001A0A3C`, so state 5 is not persistently stuck. The resource setup
  call at `0x00209390` then runs the deterministic per-frame type 1/2/3 pattern
  from `0x0039F6EC`, `0x0039F720`, and `0x0039F810`. A single object at
  `0x017F33F0` repeatedly installs exactly four scripts (`0x00404A20`,
  `0x00404CE0`, `0x003D6240`, and `0x003D6230`); only event `0x27 -> 0` and
  event `0x26 -> 1` are written after NEW GAME. The earlier 15-second
  diagnostic window represented only about 1.25 guest seconds because the
  synchronous GS/VIF white loop runs near five frames per wall second. A new
  extended replay (`build/phase3/phase4-v141-new-game-extended.log`) continued
  for about 155 wall seconds after read 481, exceeding the oracle's guest-frame
  transition budget, yet still produced no missing target, fatal signal,
  disc/file transition, or CRI/MPEG/movie call. The next diagnostic must trace
  the parent state machine at `0x0039EAB0` and the completion/external state
  consumed by recurring command handlers `0x00203028`, `0x002033EC`, and
  `0x002047A4`; do not reinterpret the repeated type 1/2/3 setup calls or the
  one-shot state-5 reset as the blocker.
- 2026-08-28: Deeper v141 control-flow tracing shows the two recurring
  opcode-`0x9E` actor waits complete and intentionally restart their background
  animation scripts. Control thread 1 advances through `0x00404F81` to
  `0x00404F98` and is then removed; the one-shot state-5 command is issued by
  the script at `0x00404800`, while event `0x26` remains the expected scene
  value 1. The scene owner at `0x01510C80` keeps its `+0x20` word at zero, but
  no evidence establishes that field as a completion gate. The parent object at
  `0x00888440` enters state 0 at `+0x44` and stays in its normal active-update
  path. Its `0x0039F86C` test of `0x0047E37C` is the live active-high controller
  word's Select bit, not a resource-readiness flag; forcing it would be an
  incorrect input patch. File-queue probes show the queue continues to be
  serviced and is not parked on an outstanding completion. These results move
  the immediate blocker away from the recurring script waits, parent-state
  value, controller Select handling, and a stuck file-queue request.
- 2026-08-28: The user clarified that the in-cutscene skip interaction is Start
  followed by Triangle. A clean v141 replay delivered Start alone
  (`0x0008`) at controller reads 720-723 and Triangle alone (`0x1000`) at
  reads 745-748 after NEW GAME Cross at reads 481-483. Start arrived at about
  58.1 wall seconds and Triangle at about 62.8 seconds; a further 15-second
  observation produced no disc/file, CRI/MPEG/movie, missing-target, or fatal
  transition. This corrects an earlier diagnostic whose masks contained extra
  bits. The sequence is retained for the real cutscene, but it cannot skip the
  present white loop because the movie/submenu state has not started. Evidence:
  `build/phase3/phase4-v141-clean-start-triangle.log`. The next action is to
  compare the scene owner/update state against a PCSX2 cold-boot oracle and
  trace the first input-independent divergence that prevents the opening movie
  request from being issued.
- 2026-08-28: A controlled PCSX2 cold-boot oracle now proves the real U.S.
  post-NEW-GAME route and corrects the white-loop interpretation. Win32-focused
  keyboard injection selected NEW GAME without falling into the attract demo;
  the third `FMV started` event is the opening movie. Pressing Start and then
  Triangle ended that third FMV immediately and entered `NOW LOADING`, after
  which the oracle reached the first in-engine scene. Reproducible evidence is
  in `build/phase3/oracle-v141-new-game-route.log`,
  `build/phase3/oracle-v141-new-game-route-events.jsonl`, and the screenshots
  under `build/pcsx2-oracle/snaps` timestamped 20260828124043 through
  20260828124149. The native v141 state observed in its white/diamond loop
  matches the oracle's *post-movie loader* globals and objects, including
  `0x0044E4D0`, `0x0044E4D8`, `0x0044E560`, `0x0044F818`, `0x0044F828`,
  `0x00888484`, and the zero completion byte at `0x017F50E4`. The native path
  therefore fails to present/start the FMV but has already entered loading; the
  Start/Triangle skip sequence cannot release that loader. A broad event-query
  trace exhausted its 512-entry budget. Subsequent exact control-flow review
  proved that the candidate calls at `0x0039F880` and `0x0039F8EC` are reached
  only after the active-high Select test at `0x0039F86C`; they are optional
  controller/menu handling, not input-independent loader-release gates. The
  byte at `0x017F50E4` is likewise initialized by the object's constructor and
  is not established as a changing completion flag. Do not force either path.
  The next action is to measure actual guest-frame progress through the loader
  and remove the demonstrated synchronous VIF1/GS bottleneck without skipping
  packets or draws, then recheck the oracle transition budget.
- 2026-08-28: The dominant blended T4 full-screen fade was still excluded from
  the indexed-texture cache by obsolete `ABE == false` and vertex-alpha-128
  guards. Broadening that cache only for the already-disjoint, neutral-RGB,
  FST/linear/T4 MODULATE case preserves the complete `WritePixel` alpha, depth,
  blend, and mask path while reducing the exact draw from roughly 20 ms to
  9.1--9.3 ms. Canonical and staged runtime/test sources are synchronized and
  the full Windows runtime suite passes 455/455
  (`build/phase3/ps2x-tests-v141-t4-fade-cache.log`). A whole-route replay now
  reaches read 481 in about 24.1 seconds, but post-Cross DMA/GIF throughput only
  improves marginally and no loader transition occurs by 167 wall seconds
  (`build/phase3/phase4-v141-t4-fade-cache-throughput.log`), so this is a valid
  startup improvement rather than the loader fix. An exact source-PC trace also
  proves the recurring type-3 resource vcall at `0x00209624` targets intentional
  zero-return leaf `0x00209800` every frame; the following object vcall at
  `0x0039F82C` preserves that zero. Thus `0x0039F834` is not a runtime-corrupted
  completion result and must not be forced. Evidence:
  `build/phase3/phase4-v141-type3-resource-vcall.log`. Continue at the first
  input-independent post-movie loader state that differs from the PCSX2 oracle;
  retain Start (`0x0008`) then Triangle (`0x1000`) for actual cutscene skips.
- 2026-08-28: Exact Start-then-Triangle replay was retained while the native
  post-movie loader gate was narrowed without forcing event state. Source-PC
  tracing proves `0x0039BB60` calls status helper `0x001770A0` once per loader
  frame; it returns `0xFF` because global manager `0x00A86780` has activation
  byte `+0x28 == 0`. The manager's `+0xC4` mode remains 0 and `+0x16C8` status
  remains 2. A guest-write watch proves constructor `0x00124DB0` writes the
  zero at `0x00124E10` immediately after NEW GAME, with no subsequent write
  through the verified Start/Triangle window. The normal activation helper
  `0x00125CC0` is not missing or broken: it is called for two subordinate
  resources (`0x00CB8B40` and `0x00CDABC0`) in the same load, but not for the
  manager. Only event bit 2 is set from `0x0039BBE4`; the three reviewed script
  paths that could set event bit 1 are not entered. These facts do not yet prove
  that the oracle activates this manager, so neither the activation byte nor
  event bit 1 may be forced. Evidence:
  `build/phase3/phase4-v141-resource-gate-trace.log`,
  `build/phase3/phase4-v141-resource-active-write.log`, and
  `build/phase3/phase4-v141-resource-activate-call.log`. A fresh PCSX2/PINE
  cold boot has deterministically reached the real opening FMV by waiting for
  menu object `0x009C90E4`, selecting index 0 with two Up pulses, and pressing
  Cross; the next action is to let that FMV end naturally and sample the exact
  manager/event fields at the first post-movie loader frame for a differential.
- 2026-08-28: The fresh PCSX2/PINE attempt did not enter the post-movie loader;
  after the intended opening presentation it looped into additional FMVs while
  loader globals remained uninitialized, so that run was stopped and is not
  being used as oracle evidence. The retained successful oracle instead exposed
  the first exact native divergence: scene record `0x017F4FF0` was finite
  (`0x3DBEBDA3`) in PCSX2 but `0xFFC00000` in v141. A native guest-write watch
  traced the NaN to `0x0021A228`. Exact instruction review then found a reusable
  recompiler defect: R5900 `SQRT.S` takes its operand from `ft`, but
  `FpuTranslator` generated `sqrt(fs)`; its `RSQRT.S` emission also omitted the
  `fs` numerator and used the wrong radicand. The two live words at
  `0x0021A1C8` and `0x0021A1D8` consequently read stale `f0` instead of `f6`
  and `f1`, propagating a false NaN through the loader. Canonical and staged
  translators now emit `fd = sqrt(ft)` and `fd = fs / sqrt(ft)`, with a focused
  code-generation regression. The full Windows suite reports 455/456; only the
  known working-directory-sensitive `instructions.h` lookup fails. Clean v142
  recompilation preserves 10,930 functions, 10,720 recompiled bodies, 210
  bindings, 389,827 resumable entries, and zero skipped/decode/unhandled/
  correctness-critical failures. Its linked cold replay reaches read 481 at
  24.780 seconds and writes `0x3DBEBD89` at `0x017F4FF0`, within the expected
  small timing drift of the retained oracle instead of NaN. Coverage is
  70.0642%, with zero overlaps and zero uncovered direct targets/calls. Exact
  Start (`0x0008`, reads 720-724) then Triangle (`0x1000`, reads 745-749) still
  produces no movie/service transition or missing target in the following 25
  seconds, so Phase 4 remains active. Evidence:
  `build/phase3/phase4-v141-scene-base-write.log`,
  `build/phase3/ps2x-tests-v141-r5900-sqrt.log`,
  `build/phase3/recompile-v142.log`,
  `build/phase3/function-coverage-v142.json`, and
  `build/phase3/phase4-new-game-map-v142.log`. The next action is to repeat the
  post-movie manager/gate trace under corrected v142 and use the first remaining
  oracle mismatch rather than forcing manager activation or event bits.
- 2026-08-28: The corrected v142 differential found and fixed a second reusable
  math defect: `sceVu0Normalize` had normalized XYZW, whereas this PS2 helper
  normalizes XYZ and clears W. The live input `(0, 0.2, 1, 1)` now produces the
  oracle's exact `(0, 0.196116, 0.980581, 0)` representation at `0x017F50C0`.
  Canonical/staged runtime sources and an in-place regression are synchronized;
  the full suite reports 456/457 with only the known working-directory-sensitive
  `instructions.h` failure
  (`build/phase3/ps2x-tests-v142-vu0-normalize.log`). A clean PCSX2 route and
  savestate establish that the real interaction is Start followed by Triangle.
  Native receives both exact masks, but byte `0x00888462` never latches because
  its setter requires the movie handler to be active. Wide native/oracle loader
  comparison now differs only in five timer-derived floats, a parent counter,
  and that movie Start latch; bulk manager-region differences are uninitialized
  oracle RAM and are not treated as required state. Internal framebuffer
  captures still show a corrupted post-New-Game transition rather than gameplay
  or a movie, so no presentation or A/V claim is made.
- 2026-08-28: Verified-ELF string/XREF review located `OPENING.SFD` at
  `0x0044E888` and its exact high-level owner `0x0012CB30-0x0012CCE8`.
  Targeted v142 replay proves this owner runs immediately after Cross and calls
  `0x002B6D10` with that exact path. Its file-service virtual call at
  `0x002B6DC0` reaches `0x0016B1F0` and returns valid handle `0x0082A940`.
  None of the reviewed movie-manager update/decode methods at `0x002B6510` and
  `0x002B6640-0x002B6BB0` execute afterward, and no `sceMpeg*` transition is
  emitted. Thus the opening request and file lookup work; the first remaining
  defect lies in manager registration/update after the successful open, before
  MPEG decode or the Start/Triangle submenu. Evidence:
  `build/phase3/phase4-v142-opening-launch-trace.log`,
  `build/phase3/phase4-v142-opening-file-call-trace.log`, and
  `build/phase3/phase4-v142-movie-method-trace.log`.
- 2026-08-28: v143 identified and removed a false inherited instruction patch
  at `0x00259F20`. Exact ELF word `0xAE030004` is the ordinary
  `sw v1, 4(s0)` that restores MPEG object-pool capacity 8 after the pool
  memset; v142 had emitted NOP and therefore made `0x0025A0D0` return zero.
  v143 cleanly recompiles 10,931 functions (10,721 recompiled, 210 bindings),
  records 389,840 resumable entries, reports zero critical failures, covers
  70.0656% of executable bytes, and builds/links/cold-boots on Windows. Its
  replay proves capacity 8 at `0x003EA2A4`, a real pool slot at `0x003EA2B8`,
  `sceMpegCreate` execution, and nonzero returns from `0x0023D828` and the real
  decoder constructor `0x00238BF0`. It also delivers NEW GAME Cross at read
  481, Start at 720, and Triangle at 745. The next exact boundary is callback
  `0x00241580` through `0x00243148`. A source-filtered trace captures its live
  record at `0x0100B414`: mapped `0x00241540` followed by uncovered
  `0x00241560`, `0x00241580`, `0x002415A0`, and `0x002415C0`. All four now have
  complete delay-slot-aware v144 metadata; the first three are tail adapters
  and the fourth is a fully reachable 46-instruction callback ending at mapped
  neighbor `0x00241678`. Evidence:
  `build/phase3/recompile-v143.log`,
  `build/phase3/function-coverage-v143.json`,
  `build/phase3/phase4-new-game-map-v143.log`, and
  `build/phase3/phase4-v143-mpeg-callback-record3.log`.
- 2026-08-28: Clean v144 adds the complete live MPEG callback record at
  `0x0100B414`: exact adapters `0x00241560-0x0024157C`,
  `0x00241580-0x0024159C`, and `0x002415A0-0x002415BC`, plus callback
  `0x002415C0-0x00241678`. Standalone recompilation processes all 10,935
  functions (10,725 recompiled, 210 runtime bindings), records 389,841
  resumable entries, and reports zero skipped functions, decode failures,
  unhandled instructions, correctness-critical fallbacks, failures, or errors.
  Coverage is 70.0729%, with zero overlaps and zero uncovered direct
  targets/calls. Generated output is staged with the guarded newest-ten
  retention policy. The requested handoff interrupted the Windows unity build
  cleanly after `unity_336_cxx.cxx`; v144 has therefore not linked, cold-booted,
  or replayed, and no compiler, linker, or runner process remains. Resume the
  existing build command, then replay NEW GAME Cross (`0x4000`, reads 481-484),
  Start (`0x0008`, reads 720-724), and Triangle (`0x1000`, reads 745-749) to
  verify callback execution and whether the real cutscene skip submenu becomes
  active before following the next exact movie/loading diagnostic.
- 2026-08-28: The v144 Windows unity build subsequently completed, linked, and
  cold-booted successfully. Stable watched title/LOOP_DEMO replays prove the
  movie outer object at `0x009C9140` and decoder at `0x003E893C` are constructed,
  and the real decoder status method `0x0023C9A8` runs each frame. Decoder state
  `0x003E8944` changes from zero to one at `0x0023B5EC` but initially never
  reaches state two. A PCSX2/PINE cold-boot comparison disproved the suspected
  `0x003E9F18` singleton blocker: that word remains zero in both oracle and
  native execution. The decisive differential is movie-service gate
  `0x003E88CC`, which is one in the oracle while v144 leaves it zero. Exact ELF
  inspection proves `sw v1, -0x7734(v0)` at `0x0023AC04`, the delay slot of the
  call at `0x0023AC00`, initializes this gate to one. An inherited
  `recomp.base.toml` patch had incorrectly replaced that real store with NOP.
  v145 removes only this false patch. It builds, links, retains clean 70.0729%
  coverage, and its replay now sets `0x003E88CC=1` and naturally executes
  `mwPlyExecSvrHndl` chain `0x00240D88 -> 0x00240E28 -> 0x002408B0 ->
  0x00240500`, matching the oracle rather than forcing state or synthesizing a
  singleton. That newly live route reaches omitted worker `0x002429C8` through
  JALR site `0x00254B58`. Exact bytes prove complete owner
  `0x002429C8-0x00242A84`; v146 adds it, recompiles 10,936 functions (10,726
  recompiled, 210 bindings) with 389,843 resumable entries and zero critical
  report counts, stages it under the guarded newest-ten policy, and enters the
  Windows unity build. Evidence:
  `build/phase3/oracle-v144-mwsfd-state-transition.jsonl`,
  `build/phase3/phase4-v144-decoder-callbacks.log`,
  `build/phase3/phase4-v145-mwsfd-registration.log`,
  `build/phase3/recompile-v146.log`, and
  `build/phase3/function-coverage-v146.json`.
- 2026-08-29: v146 completed the Windows unity build, linked, cold-booted, and
  replayed. It retains 10,936 functions (10,726 recompiled and 210 runtime
  bindings), 389,843 resumable entries, 70.0781% executable-byte coverage, and
  zero overlaps, uncovered direct targets/calls, or correctness-critical
  recompilation counts. The restored `0x002429C8-0x00242A84` worker now runs,
  but the opening stream object at `0x01081440` remains in state 2 at
  `0x01081488`. Its live inner pointer at `0x0108343C` resolves to
  `0x01084960`, which points to middleware object `0x003B72FC`. Native leaves
  that object's first word at `0x02030101` (state byte `+1 == 1`), so request
  handles 3 and 4 at `0x010828AC` and `0x01082920` remain zero and their slot
  readiness words at `0x01083500` and `0x01083544` never assert. A fresh
  PCSX2/PINE cold boot using the same verified U.S. build, two Up pulses, and
  Cross establishes the corresponding oracle word as `0x02030301`: state byte
  `+1 == 3`, both request statuses are 1, both readiness words are 1, and the
  stream state has advanced to 4. Object `+0x74` is zero in both executions,
  disproving it as the missing condition. Native write tracing shows only the
  legitimate initialization `0x001D2C74` changing the exact byte from 0 to 1;
  no later write occurs. A broad candidate trace was dominated by unrelated
  `0x001D17B0` objects and confirmed that the exact object enters
  `0x001D2BF0`, but none of the candidate state-3 routines executes for it.
  Do not force state 3 or readiness. The next action is to identify the absent
  middleware update/callback owner that legitimately performs the oracle's
  1-to-3 transition, then validate that native request and stream states advance
  naturally. Evidence: `build/phase3/phase4-v146-newgame-middleware-state-write.log`,
  `build/phase3/phase4-v146-newgame-request-status-writers.log`,
  `build/phase3/phase4-v146-middleware-transition-candidates.log`, and
  `build/phase3/oracle-v146-middleware-cold.jsonl`.
- 2026-08-29: A focused 55-second v146 replay traced middleware updater chain
  `0x001D1050`, `0x001D1010`, `0x001D0E08`, and `0x001D0F78` while delivering
  Cross at read 481 and the verified Start/Triangle skip sequence at reads 720
  and 745. None of the four functions executed. This localizes the defect above
  the per-object state machine: global updater `0x001D1050` is not scheduled at
  all, rather than its state-1 or state-2 branches returning the wrong result.
  Exact control flow shows `0x001D1050` is called unconditionally at
  `0x001CA9D0` inside owner `0x001CA928`, whose known direct callers are
  `0x001CA850` and `0x00242F20`. The next action is to trace this owner/caller
  chain and compare its registration/scheduling gate with the PCSX2 oracle;
  do not call the updater artificially. Evidence:
  `build/phase3/phase4-v146-middleware-updater-trace.log`.
### Phase 4 middleware scheduling correction and oracle timeline (2026-08-29)

- Replayed v146 with focused dispatch tracing for `0x00242F20`, `0x001CA850`, `0x001CA928`, `0x001CA9D0`, and `0x001D1050`. None executed; evidence is in `build/phase3/phase4-v146-middleware-owner-schedule.log`.
- Corrected the prior static call-graph interpretation: `0x001CA850` is teardown/refcount code and merely falls through to the adjacent function boundary in generated source; it does not call `0x001CA928`. The only demonstrated direct caller of `0x001CA928` is vtable method `0x00242F20`, at call site `0x00242F6C`.
- Located `0x00242F20` as the final slot of the vtable beginning at `0x00459748`. Generic table dispatch occurs through `0x00255DB0`, whose indirect call site is `0x00255E00`; the next upstream chain under review is `0x00246BD8 -> 0x00255D98 -> 0x00255DB0 -> 0x00255E00`.
- A broader native vtable trace (`build/phase3/phase4-v146-movie-file-vtable-trace.log`) showed the exact middleware object reaches state 1 and the stream reaches state 2 during menu boot, before the replayed New Game Cross. The per-frame `0x00242FB0` path returns zero because `0x01084A68` is zero.
- A fresh PCSX2 cold-boot timeline (`build/phase3/oracle-v146-middleware-boot-timeline.jsonl`) disproved that field as the blocker: `0x01084A68` remains zero in the oracle too. The oracle reaches middleware word `0x02030101` and stream state 2 at approximately 17.286 seconds, then naturally reaches `0x02030301` and stream state 4 at approximately 18.037 seconds.
- Current blocker: native never selects/invokes `0x00242F20` during the brief post-initialization menu window, so `0x001CA928 -> 0x001D1050` is never scheduled. Next executable action is to identify and compare the exact method-index/callback registration gate feeding `0x00255E00`, then restore only the demonstrated missing metadata or runtime/recompiler behavior.
### Phase 4 movie-object scheduler correction (2026-08-29)

- A 5 ms PCSX2/PINE probe sampled 5,000 points across the middleware transition (`build/phase3/oracle-v146-middleware-highrate.jsonl`). Root `+0x3628` (`0x01084A68`) remained zero in every sample, including the complete observed state-1-to-state-3 window. This disproves the previously suspected `0x00253570 -> 0x00256398(index 13) -> 0x00242F20` route as the oracle transition path.
- Static inspection also proves `0x001D1050` cannot update the target: it iterates eight `0x2F8`-byte objects starting at `0x003C06B0`, while the exact movie middleware object is `0x003B72FC`.
- The exact target is entry 2 of a 16-entry `0xC4`-byte array starting at `0x003B7238`. `0x001D3E58` walks that array, calls `0x001D5FA8` for active entries, and therefore passes exact object `0x003B72FC` naturally.
- `0x001D5FA8` dispatches target state 1 to `0x001D52D8`. That handler legitimately writes state 2 at `0x001D56B8`; state 2 subsequently dispatches to `0x001D5710`, whose `0x001D57C8` write advances the target to state 3.
- The immediate next action is a focused deterministic native trace of `0x001D3E58`, the exact array call/dispatcher, `0x001D52D8`, and its state-2 write/gates to locate the first absent execution or differing condition. No movie state or readiness value will be forced.
- A PCSX2 debugger UI attempt was abandoned when Windows raised the camera/privacy prompt already documented in the handoff. The prompt and privacy settings were not touched, and the oracle process was stopped.

### Phase 4 exact movie-ring producer differential (2026-08-29)

- Focused native tracing corrected the remaining scheduler assumption. `0x001D3E58` naturally visits exact movie object `0x003B72FC`; dispatcher `0x001D5FA8` naturally calls state-1 handler `0x001D52D8`. Its first unmet condition is subordinate object `0x003BFCC0` remaining in state 1. Evidence: `build/phase3/phase4-v146-movie-object-scheduler-trace.log`.
- The subordinate scheduler is also healthy: `0x001CE4D8 -> 0x001CE1A0 -> 0x001CD708` visits exact object `0x003BFCC0` continuously. Its state-1 path reaches ring methods `0x001E32B8`, `0x001E4708`, `0x001E34A0`, and `0x001E3660`, but every offered span is empty, so the legitimate state-2 write at `0x001CD9EC` is never reached. Native write tracing records initialization to state 1 and no forced or unexplained later write. Evidence: `build/phase3/phase4-v146-subordinate-state-writes.log`, `build/phase3/phase4-v146-subordinate-path-trace.log`, and `build/phase3/phase4-v146-subordinate-callsite-target-trace.log`.
- The first concrete native/oracle divergence is the exact source ring at `0x003D01A8`. Its constructor/static fields match, but native leaves all producer cursors and lengths zero. A fresh 5 ms PCSX2/PINE cold-boot probe shows the oracle beginning to fill the ring at approximately 0.439 seconds after New Game input (`+0x0C == 0x56A0`, `+0x10 == 0x072C`, `+0x14 == 0x56A0`), then advancing the subordinate from state 1 to 2 and middleware from state 1 through 2 to 3 by approximately 0.505 seconds. Evidence: `build/phase3/phase4-v146-subordinate-native-ring-snapshot.log` and `build/phase3/oracle-v146-movie-ring-probe.jsonl`.
- Added reproducible probe `tools/pine_movie_ring_probe.py`; it waits for the verified menu object, injects two Up pulses and Cross, and samples the middleware, subordinate, and ring objects without changing guest state beyond controller input.
- Exact construction flows through `0x001D25C0 -> 0x001D13D0 -> 0x001D1228 -> 0x001D1120`; the returned producer/driver object for this movie is `0x003C28B0`. The next action is to trace that exact object's natural updater and callback path and compare its first nonmatching gate with the oracle. No movie, subordinate, readiness, or ring state has been forced.
### Phase 4 exact producer identity and payload-length gate correction (2026-08-29)

- Corrected the exact movie file producer identity from `0x003C28B0` to
  `0x003C2910`. Its interface pointer is `0x003D01E8`, which is the second
  interface of the exact ring rooted at `0x003D01A8`.
- A focused native replay in
  `build/phase3/phase4-v146-exact-ring-space-return.log` proved that
  `0x001E3268` naturally returns `0x00224800` from interface word
  `0x003D01F8`. The method and its indirect dispatch are therefore working.
- The first rejecting comparison in `0x001D17B0` instead differs because the
  exact producer word at `0x003C292C` is zero natively. PCSX2 has
  `0x001B6CCC` there. This word is the payload byte count passed to
  `0x001D1E58`, not middleware state or a callback pointer.
- Native write tracing in
  `build/phase3/phase4-v146-exact-producer-callback-write.log` showed the full
  observed history of that word: `0x001D11F0` initializes it to the buffer
  capacity `0x00224800`, then `0x001D1E64` clears it during the movie object
  teardown path reached from `0x0023C880`. No later legitimate size-setting
  write occurs.
- This explains the downstream behavior without forcing state: native computes
  `0x224800 - 0x224800 < 0`, rejects the completed read, and clears producer
  completion byte `0x003C2912`; the oracle's positive payload byte count makes
  the same capacity gate pass and permits the ring to fill.
- `build/phase3/phase4-v146-movie-file-loader-path.log` confirms the generic
  archive/file-loader scheduler (`0x001DE5D8 -> 0x001DEF68`) is running, but the
  sampled native objects remain in their polling state and never reach
  `0x001DEC70`/`0x001DED54` for this producer.
- Immediate next action: identify and compare the movie-specific open/stat
  result and the exact call gate that should repopulate `0x003C292C`; do not
  inject the oracle's observed length.

### Phase 4 exact loader queue boundary correction (2026-08-29)

- The exact movie loader is `0x003C6A20`, attached to producer
  `0x003C2910`. Its native metadata matches the PCSX2 oracle: active byte 1,
  state byte 0, source pointer, total length `0x001B6CCC`, capacity
  `0x00224800`, and producer pointer are all present and consistent.
- Exact guest-write tracing of loader state byte `0x003C6A21` records only the
  pool clear and constructor clear at `0x001DE190`. It never transiently
  reaches state 1 or 2, never calls completion routine `0x001DEC70`, and
  therefore never publishes the already-known total length into producer word
  `0x003C292C`. Evidence:
  `build/phase3/phase4-v146-exact-loader-state-write.log` and
  `build/phase3/oracle-v146-movie-loader-probe.jsonl`.
- A focused deterministic replay of apparent public request wrappers
  `0x0023F6C8`, `0x0023F0B0`, and their `0x001DE2D0` queue call found no
  execution. Exact ELF scans also find no direct call or static pointer to
  those wrappers, so they are not treated as the live opening request path.
  Evidence: `build/phase3/phase4-v146-movie-initial-request-focused.log`.
- Static review located the demonstrated live construction path
  `0x002425F8 -> 0x00242908 -> 0x001D25C0`. After object construction,
  `0x002425F8` creates the five-entry movie callback record at owner
  `+0x35D4`, registers it through `0x001D2DB0`, enables the service through
  `0x00242D78`, and installs worker `0x002429C8`. The next executable action is
  to trace this live registration/enable sequence and the exact loader queue
  call, then compare the first differing gate with PCSX2. No loader state,
  producer length, ring length, or readiness value will be forced.

### Phase 4 exact movie read-submission boundary (2026-08-29)

- Fresh PCSX2/PINE sampling corrected two earlier interpretations. Loader state
  byte `0x003C6A21 == 0` is also the oracle's stable post-setup state, and the
  producer request byte at `0x003C2912` is transient: the oracle changes the
  producer word from `0x00010201` back to `0x00000201` after completion while
  retaining payload length `0x001B6CCC`. Neither byte is itself the blocker.
  Evidence: `build/phase3/oracle-v146-handle-status-prime.jsonl`,
  `build/phase3/oracle-v146-handle-status-object.jsonl`, and
  `build/phase3/oracle-v146-handle-status-record.jsonl`.
- Decoded the exact file-handle status route
  `0x001ED2D0 -> 0x001E70D0 -> 0x001E6910`. The internal request record is
  `0x007F81C8`; its halfwords at `+0x34`, `+0x36`, and `+0x38` are respectively
  `1,0,0` natively, while the oracle advances them from `1,0,1` to `1,1,2`.
  The native status method therefore legitimately returns pending forever.
  Evidence: `build/phase3/phase4-v146-handle-status-record-trace.log` and the
  oracle records above.
- The real read method is `0x001E6738`, reached through
  `0x001ED2A0 -> 0x001E7030`; it owns the legitimate request-completion writes.
  Ordinary pre-movie reads traverse this route and complete correctly, which
  rules out a general disc-backend failure. Evidence:
  `build/phase3/phase4-v146-request-read-method-trace.log`.
- Focused tracing of the exact opening producer shows only its two setup seeks
  through `0x001D7328`; it never calls read wrapper `0x001D7390`. The preceding
  ring-space call at source `0x001D19EC` correctly dispatches to
  `0x001E3268` and returns `0x00224800`. Producer `+0x40` is also
  `0x00224800`, but producer payload length `+0x1C` is zero, so the exact guest
  comparison computes `0 < 0` and exits before the ring-span method at
  `0x001D1A28` and read submission at `0x001D1AF8`.
  Evidence: `build/phase3/phase4-v146-exact-read-submission-chain.log` and
  `build/phase3/phase4-v146-exact-ring-space-return.log`.
- Current blocker: locate the missing legitimate publication of already-known
  loader length `0x001B6CCC` into producer `0x003C292C`. The sole primitive
  setter is `0x001D1E58`; its demonstrated callers and the live movie-specific
  gate are under review. Do not inject the length or force request, ring,
  middleware, or readiness state.

### Phase 4 executable-gap call-graph audit and v147 (2026-08-29)

- Expanded `tools/audit_gap_leaf_functions.py` from a frameless-leaf-only scan
  into a conservative delay-slot-aware CFG audit. It now recognizes
  stack-framed functions with multiple valid return/stack-restoring tail exits
  and records their direct calls and external tail targets. New regression
  coverage exercises multiple tail exits and rejects computed non-return jumps;
  all eight focused audit tests and all 27 function-config tests pass.
- The v146 effective map has zero unresolved direct calls/jumps from already
  mapped code, but the expanded gap audit found 99 structurally complete
  omitted functions whose inbound route may be indirect or dynamically
  installed. Reproducible report:
  `build/phase3/gap-function-audit-v146.json`. This distinction is important:
  direct-edge completeness does not prove that all indirect-only owners exist.
- Added `tools/audit_function_call_graph.py` to compose the mapped function
  graph with the conservative gap candidates. The v147 report records 26,922
  direct call/tail edges, nine intentional runtime-binding edges, zero
  genuinely unowned direct edges, and 19,000 indirect call sites. Six
  candidate-origin paths reach selected loader/read targets, but none reaches
  the demonstrated `0x001DEC70` completion owner or `0x001DE4A8` arm routine.
  Evidence: `build/phase3/function-call-graph-v147.json`.
- The audit recovered the exact adjacent movie-loader helper family
  `0x001CAD30-0x001CAD60`, `0x001CAE30-0x001CAEDC`, and
  `0x001CAEE0-0x001CAEE8`. The middle owner calls cleanup/queue/configuration
  helpers and tail-calls mapped `0x001CAC80`, which is the mapped caller of
  legitimate loader arming routine `0x001DE4A8`. v147 adds these three exact
  reviewed owners without stubs or state forcing.
- v147 validates and recompiles 10,939 functions (10,729 recompiled bodies and
  210 runtime bindings), records 389,846 resumable entries, reaches 70.0843%
  executable-byte coverage, and retains zero overlaps, uncovered direct
  targets/calls, skipped functions, decode failures, unhandled instructions,
  correctness-critical fallbacks, failures, or errors. Generated output was
  staged with the newest-ten numeric-version retention guard; the Windows
  runner completed its full unity rebuild and linked successfully.
- A v147 opening setup replay reached the exact loader/producer state while a
  noisy `0x001D17B0` target consumed the trace budget; none of the three new
  helpers, mapped `0x001CAC80`, or `0x001DE4A8` appeared before that point.
  A second quiet replay received Cross at read 481 but did not reproduce the
  New Game transition, so it is not used as negative behavioral evidence.
  The metadata is valid, but there is not yet evidence that this overload
  family is the live opening route or that v147 fixes the payload publication
  blocker. Evidence: `build/phase3/phase4-v147-loader-family-replay.log` and
  `build/phase3/phase4-v147-loader-family-focused.log`.
- The remaining v147 gap report contains 115 triage candidates and their
  outbound edges in `build/phase3/gap-function-audit-v147.json`. They must be
  ranked by live registration, pointer, or oracle evidence rather than added
  wholesale. The next action is to combine those omitted-origin edges with the
  complete mapped direct-call graph and prioritize indirect-only owners nearest
  the demonstrated `0x001DEC70 -> 0x001D1E58` completion route.
- A focused v147 replay corrected an apparent producer-field divergence in the
  earlier trace: exact producer `0x003C2910` does naturally execute
  `0x001D1BA8`, resumes after both seeks, and publishes `+0x10 = 0x03CA8000`
  and `+0x14 = 0x00007950`, matching the PCSX2 oracle. The earlier
  `0x7FFFF800`/`0x000FFFFF` sample was taken between the nested seek return and
  the caller's following stores. The real remaining divergence is still
  producer payload length `+0x1C`: native remains zero while the loader metadata
  and oracle producer both hold `0x001B6CCC`. Evidence:
  `build/phase3/phase4-v147-producer-resume-write-trace.log`.

### Phase 4 PMFHL runtime correction and first live gap candidate (2026-08-29)

- A fresh high-rate PCSX2/PINE Cross probe proved that the oracle publishes
  producer payload length `0x001B6CCC` immediately during movie construction,
  before later loader scheduling. Evidence:
  `build/phase3/oracle-v147-loader-input-transition.jsonl`.
- Native dataflow localized the zero length to
  `0x0023BB00 -> 0x0011F6F8 -> 0x0011F208 -> 0x0011F7A8 -> 0x0023C860`.
  The owner multiplies capacity `0x00224800` by the verified ELF double constant
  `0.8`; the expected truncated result is exactly `0x001B6CCC`. The software
  multiply was corrupted inside `0x0011F260` after its `MULTU`/`PMFHL.LW`
  sequences. Evidence:
  `build/phase3/phase4-v147-producer-size-caller-trace.log` and
  `build/phase3/phase4-v147-softdouble-size-trace.log`.
- Corrected the reusable PMFHL instruction family in canonical and staged
  PS2Recomp. PMFHL now uses both context accumulator pairs (`hi/lo` and
  `hi1/lo1`) and follows R5900 word/halfword lane order; PMTHL.LW likewise
  updates all four low words while preserving their upper halves. Three focused
  C++ regressions cover lane order, scalar MULTU reconstruction, and four-argument
  code generation. The complete `PS2RuntimeExpansion` suite passes 34/34.
- v148 recompiles the unchanged 10,939-function map with the corrected generator
  and retains zero skipped functions, decode failures, unhandled instructions,
  correctness-critical fallbacks, failures, or errors. Its focused replay now
  produces multiply low word `0xCCCCCCCC`, converts the result to
  `0x001B6CCC`, and passes that exact value through the legitimate producer
  setter. Evidence: `build/phase3/phase4-v148-pmfhl-movie-propagation.log`.
- The corrected startup movie path then exposed live indirect JALR target
  `0x0023A6E0` at source `0x00246194`. This target was already one of the 115
  conservative candidates in `build/phase3/gap-function-audit-v147.json`,
  demonstrating why the direct graph's zero unowned edges must be combined with
  runtime evidence for the 19,000 indirect sites. Exact ELF CFG review proves a
  complete stack-framed owner `0x0023A6E0-0x0023A818` with one return and seven
  mapped direct calls. v149 adds that owner without a stub or forced state and
  recompiles 10,940 functions (10,730 bodies, 210 bindings) with 389,853
  resumable entries and the same zero-error invariants. Clean replay evidence
  for the missing call is `build/phase3/phase4-v148-clean-replay.log`.
- At the v149 checkpoint, the next action was to finish its Windows link and
  replay through the restored indirect movie method. Verify that startup reaches controller read
  481 again, then trace the exact `0x003C2910` movie producer calculation and
  downstream ring/middleware propagation. Do not force any payload, request,
  movie, or readiness state.

### Phase 4 complete strict-gap batch and v151 boundary correction (2026-08-29)

- v150 added the remaining 114 conservative executable-gap candidates from the
  v147 report after exact filtering. Together with v149's `0x0023A6E0` owner,
  all 115 reported candidates are now mapped; direct calls/jumps were already
  complete. Speculative computed destinations remain excluded.
- The v150 replay exposed a false analyzer end at `0x0024C164`. Exact verified-
  ELF CFG review proved owner `0x0024C140-0x0024C2F8`, including all 110
  reachable instructions, contained branches, two mapped tail exits, return,
  and delay slots. v151 records that exact override.
- v151 contains 11,054 functions: 10,844 recompiled bodies and 210 runtime
  bindings, with 390,097 resumable entries and zero skips, decode failures,
  unhandled instructions, correctness-critical fallbacks, failures, or errors.
  Its replay passes the former `0x0024C164` stop and reaches DMA count 436.

### Phase 4 typed MPEG wait and IPU-to DMA source chain (2026-08-29)

- `build/phase3/phase4-v152-thread-snapshot.log` proves main thread 1 is blocked
  at `0x002500DC` with `EeWaitReason::Mpeg`, not waiting for the category-4
  worker. `sceMpegGetPicture` at `0x002A5A48` has no decoded frame because the
  runtime did not connect guest RAM-to-IPU DMA to its host MPEG decoder.
- Added a reusable IPU-to DMA callback seam in canonical and staged PS2Recomp,
  wired it to the already-existing MPEG playback state, and added exact normal-
  mode plus source-chain END-tag coverage. No picture, EOF, movie state, or
  scheduler wake is fabricated. Both Windows targets link and
  `PS2RuntimeExpansion` passes 35/35.
- The first corrected replay proved the live transfer is source-chain mode:
  `CHCR=0x176`, `QWC=0`, `TADR=0x00D55400`. Extending the standard chain walker
  to channel 4 made `sceMpegGetPicture` return eleven times and advanced pad
  reads naturally through 210 and Cross 481/484. Evidence:
  `build/phase3/phase4-v152-ipu-chain-replay.log`.
- The same replay produces FFmpeg damaged-slice diagnostics, so it is discovery
  progress rather than valid movie playback. Narrow tag tracing shows repeated
  REF tags with `QWC=0x80`; draining them synchronously outruns the PS2 IPU FIFO
  and corrupts later pause/resume register state. Evidence:
  `build/phase3/phase4-v152-ipu-tag-trace.log`.
- The next action at that checkpoint was to compare the first chain's FIFO
  pause/resume register timeline with PCSX2. The subsequent PCSX2 2.6.3 source
  review below now supplies the implementation semantics; a live checkpoint
  comparison remains required after the focused runtime tests.
- Audited all 17 first-party Markdown files after the v152 checkpoint. Updated the
  stable README, Phase 4 roadmap checks, current actions/risks, historical
  supersession notes, function-config staging policy, and provenance wording;
  governance/legal/handoff/template records required no running-status edits.
  Vendored PS2Recomp Markdown remains upstream-owned and was left unchanged.
  All local Markdown links resolve, canonical/staged authored sources match by
  SHA-256, both binaries and cited artifacts are present, and a fresh focused
  run passes 35/35 (`build/phase3/ps2x-tests-v152-doc-audit.log`).
- Reviewed the authoritative PCSX2 2.6.3 IPU input-DMA and DMAC implementation
  as a technical reference. It establishes an eight-QWC input FIFO, partial
  `MADR/QWC` progress, tag-in-progress retention, decoder-driven resume,
  active-channel write handling, and terminal completion/interrupt ordering.
  No PCSX2 code was copied. The native runtime still drains the chain
  synchronously, so implementation, focused tests, and a live oracle checkpoint
  remain required.

### Phase 4 VBlank cycle gate and cathedral control trace (2026-08-30)

- Narrow startup profiling proved the intermittent DMA17 stop was
  instrumentation-sensitive: heavy logging allowed startup to proceed while a
  fast untraced run could return to PC zero before ordinary DBCMAN/DMA work
  began. Successful and failed paths resolved the same final loader objects and
  the same wait method (`0x001BED00`), which polls the VBlank flags at
  `0x0047B204` and `0x0047B208`.
- The reusable defect was in `EeScheduler::processDueDeadlines`: host wall-clock
  readiness could expose a VBlank before its modeled EE-cycle deadline. The
  canonical and staged scheduler now require both the guest-cycle boundary and
  the host presentation deadline. Idle execution can still advance to the next
  modeled deadline through the existing event wait path.
- Both Windows targets link and the complete focused suite passes 39/39
  (`build/phase3/ps2x-tests-v153-vblank-cycle-gate.log`). The first clean replay
  without startup tracing progresses from initialization through DMA 27 by host
  tick 240, completes the first MPEG through picture 180 and sequence end, and
  creates the fresh cathedral handle `0x010848B0`; evidence is
  `build/phase3/phase4-v153-vblank-cycle-gate-replay-r1.log`. More than ten
  subsequent focused cold boots also passed DMA17 and reached the cathedral
  path. This is strong live evidence that the startup race is eliminated;
  retain one final untraced multi-run validation as an exit check.
- The corrected scheduler also restores the complete expected movie-control
  propagation without forced state: middleware 1 -> 3, handle status 0 -> 1,
  slot readiness 0 -> 1, and stream state 2 -> 4. Evidence is
  `build/phase3/phase4-v153-cathedral-propagation-r1.log`. The live cathedral
  objects are root `0x01081440`, stream `0x010837E0`, presentation object
  `0x01082AF0`, and MPEG handle `0x010848B0`.
- The remaining cathedral blocker is now decoded-frame retirement, not
  input production or middleware activation. The guest decode chain
  `0x00249828 -> 0x00249D30 -> 0x0024A560 -> 0x0024C9E0` continues, and the
  input byte count is legitimately replenished at `0x00250198` and consumed at
  `0x00250374`. The three demonstrated effective slots at `0x01083960`,
  `0x01083A48`, and `0x01083B30` transition 0 -> 1 -> 2; candidate
  `0x01083C18` remains zero in the latest focused trace and is not counted as a
  fourth slot. Allocation at `0x0024E698` then returns zero.
- Fresh PCSX2 2.6.3 oracle evidence in
  `build/phase3/oracle-v160-cathedral-release-selector-r1.jsonl` and
  `build/phase3/oracle-v160-cathedral-event-class6-table-r1.jsonl` proves that
  native and oracle agree exactly at cathedral root fields `0x0108350C =
  0x00459EE0` and `0x01083510 = 3`, including the event-class-6 table. The
  table's index 11 is `0x00257738`; index 12 is `0x002577B0`.
- Generated-code review corrects the earlier selector description:
  `0x00252640` selects class 6/index 11, while `0x002526F8` selects class
  6/index 12 and therefore `0x002577B0`. Native evidence in
  `build/phase3/phase4-v160-class6-index12-submit-r1.log` shows the index-12
  path works once for boot root `0x01007E40`; it never runs for cathedral root
  `0x01081440`. `build/phase3/phase4-v160-frame-release-callers-r1.log` likewise
  shows that the upstream movie worker invokes the relevant retirement path for
  boot only. The active differential has therefore moved upstream of the
  selector table to the movie-worker/presentation traversal. Do not clear pool
  slots or directly invoke the release callback.

### Phase 4 stream-identity correction and state-triggered profiler (2026-08-30)

- Re-audited the retained cathedral/presentation traces against their exact pad
  replays. Cross at read 481 tears down the currently active movie manager and
  clears update slot `0x003EA154`; samples after that point are post-movie state
  and cannot diagnose cathedral presentation. Start/Triangle remain forbidden
  in natural movie validation.
- Corrected `tools/pine_cathedral_presentation_probe.py` so New Game is confirmed
  on the default menu selection. The earlier two-Up oracle input wrapped to Load
  Game and is not valid opening evidence. The tool now has explicit cathedral
  and opening targets and records the update registry, manager singleton,
  middleware sample clock, clock-interpolator state, and frame-pool state.
- Native `0x001D3210` tracing proves the middleware sample counter at
  `0x003B7230` advances normally and the clock routine is called before movie
  teardown. The zero timestamp observed there is therefore not evidence of a
  dead host-audio counter. Evidence:
  `build/phase3/phase4-v153-cathedral-mpeg-clock-source-r2.log`.
- A raw zero-input PCSX2 lifecycle trace proves that the first movie uses update
  object `0x01007E40`; the cathedral constructs its distinct root
  `0x01081440` at 24.93 seconds and advances root state 1 -> 2 -> 4 by 25.69
  seconds. This gives the profiler an exact input-free stream trigger. Evidence:
  `build/phase3/oracle-v153-pretitle-movie-lifecycle-raw.jsonl` and
  `build/phase3/oracle-v153-cathedral-presentation-exact-root.jsonl`.
- The resulting clean native/oracle pair matches root state, selected method
  `0x002429C8`, update slot, middleware state, advancing sample counter, source
  object, child pointer, and zero clock offsets. PCSX2 arms the interpolator and
  advances `0x010823F4`; native repeatedly returns timestamp `0/0xBB80` from
  `0x001D30C8`. Evidence:
  `build/phase3/phase4-v153-cathedral-presentation-zero-input-r1.log` and
  `build/phase3/oracle-v153-cathedral-clock-source-components.jsonl`.
- Current executable action: split `0x001D30C8`'s numerator into source position
  `0x001CE540` and audio queued/consumed cursor `0x001CD0E0`. The matched source
  object and zero offsets prove that native collapses this subtraction to zero;
  compare `0x003BFCEC`, `0x003CEBC4`, `0x003CC700`, and `0x003CC708` plus the
  indirect audio-cursor method with PCSX2. Do not force a clock, registry slot,
  frame release, audio cursor, or movie state.

### Phase 4 cathedral audio-record ownership and exact interface profile (2026-08-30)

- Added `tools/pine_audio_stream_records_probe.py`, a reusable PINE profiler
  that waits for cathedral root `0x01081440`, scans the 40 records owned by
  `0x001D1D88`, and emits only active-record changes. The exact PCSX2 oracle
  identifies record 2 at `0x003C2910` as the cathedral stream: gates `+0=1`,
  `+1=2`, and `+0x49=1`, while `+0x34` and `+0x58` advance. Evidence is
  `build/phase3/oracle-v153-cathedral-audio-stream-records.jsonl`.
- Corrected two stale conclusions. Native `0x001D17B0` does not stop after the
  cathedral reaches state 4; it remains scheduled for hundreds of calls.
  Also, record 2 owns generic ring object `0x003D01E8`, not the previously
  watched `0x003D0128`. PCSX2 shows the record-owned ring cursor/occupancy at
  `0x003D01F4` changing independently of the older ring at `0x003D0134`.
  Evidence is `build/phase3/phase4-v153-cathedral-stream-record2-native-r2.log`
  and `build/phase3/oracle-v153-cathedral-record-owned-ring.jsonl`.
- Native reaches cathedral state 4 but record `+0x34` stalls at `0x00296000`
  and `+0x58` at `0x52C`; PCSX2 continues advancing those fields. This narrows
  the defect to the record-owned read/position chain, rather than loss of the
  global update callback.
- Added gated `PS2X_LIBSD_TRACE` instrumentation to canonical and staged
  `ps2xIOP/src/modules/libsd.cpp` and rebuilt successfully. No LibSd RPC is
  issued during this cathedral checkpoint, so the host LibSd response path is
  ruled out as the immediate cause. Evidence is
  `build/phase3/phase4-v153-libsd-rpc-profile-r1.log`.
- Exact indirect dispatch profiling resolved record 2's three interface methods
  as `0x001ED248`, `0x001ED2A0`, and `0x001ED2D0`; other active records use the
  separate `0x001DB268`, `0x001DB338`, and `0x001DB5C8` implementation. The
  cathedral methods delegate to `0x001E6538`, `0x001E7030`, and `0x001E70D0`.
  Evidence is
  `build/phase3/phase4-v153-cathedral-audio-interface-targets-r1.log`.
- Current executable action: compare the exact cathedral implementation's
  returned position/read/status values against PCSX2 after root state 4, then
  follow the first divergent delegate. Do not write record counters, ring
  occupancy, presentation clocks, or movie state.

### Phase 4 cathedral class-6 poll predicate isolation (2026-08-30)

- Post-callback native tracing now proves that decoded dark-stone frame payloads
  are valid rather than empty or malformed. The three effective slots hold
  image pointers `0x00DD2AC0`, `0x00EB2B00`, and `0x00F92B40` with successive
  timestamps `0x00000BB8`, `0x000003E8`, and `0x000007D0`. Evidence:
  `build/phase3/phase4-v160-frame-slot-payload-r1.log` and
  `build/phase3/phase4-v160-frame-slot-image-pointers-r1.log`.
- Downstream presentation is not the first failure. Per-frame manager method
  `0x002B64B0` and event-to-present builder `0x00239A68` each execute 45 times
  for the working red stream and once for the dark-stone stream, while the
  upstream `0x00239880 -> 0x00252640` poll continues hundreds of times after
  that single dark-stone frame. Evidence:
  `build/phase3/phase4-v160-presentation-counter-producer-r1.log`,
  `build/phase3/phase4-v160-event-to-present-record-r1.log`, and
  `build/phase3/phase4-v160-present-poll-entry-r1.log`.
- Exact generated-code review identifies the active class-6/index-11 method as
  `0x00257738`. It first calls `0x00244A58` for channel index 3 and, only if
  that succeeds, validates the returned event through `0x00255780`. The
  selected 0x74-byte channel record begins at `0x010828A4`;
  `0x00244A58` tests `0x010828A8` and dispatches using the field at
  `0x010828F0`. This narrows the first unresolved differential to those two
  subcalls rather than the MPEG decoder, slot payload, manager clock, or final
  presenter.
- The PINE presentation probe now records the exact channel-3 record type,
  early gate, status, event words, and method index, in addition to its existing
  queue dereference support. It passes `py -3 -m py_compile`. The next action is
  a fresh PCSX2 cold-boot capture of these fields followed by native return-value
  tracing at call sites `0x00257770` and `0x00257780`; fix only the first
  demonstrated divergence and do not synthesize an event or clear frame slots.
