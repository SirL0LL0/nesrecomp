# Accuracy-preserving cycle performance investigation

Central Beads: `beads-vy21`. Isolated branch: `perf/accuracy-20261022`.
Coherent reference: `a76140ceded76510c797155282358390ab474a92`.
Private evidence root: `G:/Temp/Codex/nesrecomp/accuracy-perf-20261022`
(the NAS uses the same task path). ROMs and bulk captures are not committed.

The retained bundle removes redundant portable host work: exact rollback
digest traversal, stable-span PPU control, byte-packed sprite shifting,
inert mapper read notifications, and exactly zero inactive HD snapshot
payloads. It also fixes the existing optional PPU helper's portable fallback.
Selection uses actual mapper/feature state, with no hard-coded game identity;
the per-title measurements qualify where those universal paths help.
No console cycles or hardware observations are removed. SMB1 snapshots shrink
by 3,441,472 bytes, from 4,034,842 to 593,370 bytes (85.3%). All declared final
functional checks pass. Whole-bundle CPU savings are unverified: the final
timing queue was stopped while waiting after further owner steering that the
testing had become excessive. Partial samples are preserved and not used for
a performance claim. Human playtesting can proceed on the qualified bundle.

Exploration is **frozen** following owner steering against expanding test
matrices. Surplus queues were stopped while waiting, with no active test
child; raw evidence is preserved. Deferred experiments are unverified and
are not handoff gates. This investigation does not claim exhaustive coverage
of every possible optimization. Final integration requires owner approval.

## Task burndown

| Task | Beads | Result |
| --- | --- | --- |
| Boundaries, coherent reference, two-title fixtures | `beads-gi2y` | Complete |
| CPU/dispatch/mapper/PPU/audio/copy/allocation/synchronization/replay attribution | `beads-hnu4` | Complete for the declared diagnostic profiles; device-level limits below |
| Individually controlled candidate decisions | `beads-qklx` | Complete within frozen scope; deferred cases explicitly unverified |
| Fixed combined qualification | `beads-4x05` | Complete functional qualification; combined CPU comparisons deferred |
| Two combined owner playtests and integration approval | `beads-wepn` | Ready for owner; playtests and approval pending |

## Repository and fixture boundaries

`F:/Projects/nesrecomp` is a workspace container, not a Git repository.
Its `nesrecomp/` checkout remains on legacy head `4e4a133`, including its
unrelated `tools/cli.py` edit. No original checkout, title submodule, probe,
save or configuration was reset. Work was performed solo in a G: worktree.

The pilot is Super Mario Bros. (`88424dd`, NROM). Super Mario Bros. 3
(`9d9d999`, MMC3) exercises different CHR banking and raster IRQ behavior.
Actual title builds share frozen recomp-ui `c11a4b6c649164b6951eff107f4607dcef117dd5`
and real title mod catalogs. Final native profiles use 44.1 kHz PCM; earlier
generic framework probes use 48 kHz. Inputs, private ROM copies, binaries and
ordered link objects have manifests. SMB3's final route includes level entry.

All unattended launches use dummy audio output while emulating PCM. Saves,
caches, logs, build products, TEMP/TMP and applicable AppData/XDG overrides
stay on task-owned F:/G: paths. Native progress files are never used; tests
use `--no-save`. Old save-state compatibility is not required. Current-build
restore, replay and safe invalid-input rejection remain required.

## Attribution and observable console behavior

Diagnostic instruction-pointer sampling used GCC 15.2 x64 `-O2`, native guest
code, LLE PPU, stock features and one ARGB conversion per frame, 2,400 frames
per title. Tracing and comparison dumps were off. The desktop was busy:
these samples locate work and are **not speedup measurements**. Inlined CPU
and dispatch work is charged to its containing function/object.

| Source attribution | SMB1 | SMB3 |
| --- | ---: | ---: |
| PPU fetches, sprite evaluation and pixel pipeline | 68.07% | 69.90% |
| APU, DMA/controller work and audio production | 15.49% | 14.37% |
| Mapper handling | 6.50% | 6.72% |
| Machine bus and master-clock scheduling | 6.25% | 6.11% |
| Other CPU/native/dispatch/runtime work | Remainder | Remainder |

Controlled software-SDL thread-cycle attribution separately records core work
at 74.77%/76.46%, audio drain/queue at 0.23%/0.21%, conversion/texture/draw
at 6.50%/6.37%, present at 0.08%/0.07%, and deadline waiting at 18.21%/16.69%.
These are within-run diagnostic cycles, not cross-run savings, physical GPU
cost, audio-device CPU attribution or wall-time wait duration. Evidence:
`broad/sdl-checks-controlled/attribution.json`.

Copies and allocations concentrate on snapshots. The generic stock snapshot
is 326,548 bytes, or 326,560 with the rollback trailer. The old save path uses
output growth plus cartridge, host and mod scratch allocations. A 1 MiB mod
scratch allocation occurs even without hooks; the cartridge temporary is
copied again. Fewer allocations did not by themselves yield a CPU win.
Requested-byte counters are not resident-memory measurements.

Actual SMB1 registers four HD pixel blocks plus pipeline state even with HD
inactive. Every payload byte, including padding, is checked before compact
encoding; nonzero or active data retain their complete representation. Loads
reconstruct exact zeros and invalidate presentation. SMB3 has no HD records:
its 333,416-byte native state is unchanged. No cache or invalidation heuristic
is substituted for exact payload checks.

Rollback snapshots/digests already share a cached image until invalidation,
and replay already skips presentation. The fused digest keeps independent
master/partition FNV recurrences but traverses each region once. Malformed and
trailing bytes keep their original hash treatment. Real delayed-peer trials
measure total process CPU and record live/replay/save/load wall phases; those
phase timers are not isolated CPU attribution. Replay still executes the
same guest and hardware work.

The retained PPU paths preserve sprite-zero hits and overflow, scrolling and
address latches, CHR selection, mid-frame palette effects, mapper A12/IRQ and
read-release latches, DMA/DMC interaction, interrupts and relevant open bus.
Stable-span eligibility is checked at each dot/half-dot; fetches and side
effects still execute. HD builds retain their original sprite callback loop.
Mapper `/RD` calls are skipped only when their existing body cannot mutate
state; MMC5 and pending pattern-latch handling remain. A replaced framebuffer
does not justify removing PPU behavior. No tile/palette/OAM cache was added.

## Controlled CPU measurements and decisions

The desktop's other authorized heavy jobs were left running. Following owner
direction, SSH documented in `F:/NAS` was used to run serial measurements on
SERVER: Intel i5-4670, four cores, Windows 10. F:/G: space/load were checked;
remote scratch and caches are on G:. No service or other window's job was
stopped. All cases use the shared byte lock
`G:/Temp/Codex/performance-measurement.lock`, wait for a quiet interval, and
monitor other game/compiler processes throughout each admitted leg.

Each case has two warmup legs, four A/A legs, then ABBA/BAAB/ABBA (18 admitted
legs). Results use user+kernel process CPU; wall time is separate. Setup is
identical and included. Features, inputs and object positions are held fixed.
Raw attempts, exclusions and hashes are preserved. Positive percentages mean
less CPU. Results describe these hosts/profiles; they are neither additive
nor universal FPS claims.

| Isolated candidate / profile | Median CPU reduction | A/A paired variation | Decision |
| --- | ---: | --- | --- |
| Exact fused digest, real SMB1 two-peer rollback | 3.80% | -1.89%, -2.30% | Retained (`995f599`) |
| Same digest, SMB3 framework rollback | 4.03% | +1.47%, -5.47% | Retained, with noisy control explicitly noted |
| Portable LLE stable span, SMB1 2,000 frames | 6.16% | -1.21%, +0.56% | Retained |
| Same span, SMB3 active 3,000-frame route | 5.67% | -0.82%, 0.00% | Secondary-title support |
| Portable sprite shifting, stock non-HD SMB1 | 3.63% | +0.28%, +0.56% | Retained for non-HD builds; no such claim for native HD-enabled SMB1 |
| Inert mapper `/RD` guard, SMB1 2,000 frames | 2.22% | +0.28%, 0.00% | Retained |
| Exact inactive HD zero storage, native SMB1 snapshot every frame | 18.49% | -1.06%, -2.51% | Retained; snapshot-heavy profile only |
| Packed sprite priority, SMB1 | 0.98% | +0.93%, -0.84% | Rejected: too close to control variation |
| Temporary cartridge allocation/copy removal | -0.60% | +0.30%, 0.00% | Rejected: no repeatable CPU gain |
| Lazy mod scratch | -0.45% | +1.20%, +0.90% | Rejected: no repeatable CPU gain |
| Windows waitable pacing, 900 paced SMB1 frames | 6.58% | +0.53%, -2.32% | Rejected as default: worse deadline lateness |

HD storage's three block reductions are 16.14%, 19.26%, 18.49%. This controlled
2,000-frame workload uses actual native title callbacks/catalog, one snapshot
and ARGB request per frame, and exact PCM/pixel-chain/expanded-state checks.
Only the HD object differs; the identical observer occupies the same link
position. It is not ordinary offline frame throughput.

The pacing trial preserves 900 presents, state, pixels and PCM, but the median
of measured per-run p99 lateness rises from 14.35 us to 537.4 us. Reference
p99 values span 7.1–12,485.3 us and candidate values 512.8–3,221.4 us. Raw
outliers remain recorded. A CPU win does not qualify that presentation tradeoff.

The digest's corrected session blocks improve 6.14/3.80/3.33% for SMB1 and
7.04/4.03/3.84% for SMB3. Each peer executes seven replay ticks; final state is
identical across all admitted heads/peers. Its digest-only 3,000-call result
(48.41%) is a subsystem diagnostic, not frame throughput. Median live/replay
wall times stay around 9 ms; digest work is outside those frame timer scopes.

Final combined native CPU comparisons used ordinary uninstrumented title
binaries, one end-state save, WAV and final PNG, and matching normalized
ordered objects. They were stopped while waiting, with no active test child.
They are not handoff gates and must not be resumed without new scope:

| Title / profile | Median CPU reduction | A/A | Blocks | Status |
| --- | ---: | --- | --- | --- |
| SMB1, 2,000 frames, actual catalog | Unverified | Partial | Incomplete | Deferred; partial raw legs retained |
| SMB3, 3,000 frames, active-level route | Unverified | Unmeasured | Unmeasured | Deferred |

## Deferred and rejected broader experiments

Nineteen isolated implementations received initial two-title state/pixel/PCM
route checks. Broader prototypes included CHR/nametable shortcuts, deferred
CHR-write indexing, APU channel inlining, expansion-audio/ROM-snoop/IRQ
specialization, bounded exact PRG hashing, LTO, exact ARGB reuse, smaller state
reserve, stack chunk descriptors, and SSE sprite helpers. Their unfinished CPU
comparisons are **deferred/unverified**, not accepted wins or required work.
The 512 KiB reserve reduction and 4 KiB heap-to-stack substitution are requested
allocation findings only; lazy scratch saves 914,920 requested peak bytes only
without hooks. None is bundled. Optional HLE remains optional and unchanged
as a shipping profile; its broader audit does not justify a default switch.

The existing helper now retains a scalar interface on other targets and gates
SSE2 on the actual x86-64 ABI (`5f5321c`). Native/scalar GCC x64 tests, MSVC
Win32 IA32 tests, and an AArch64 helper cross-build pass. This is not execution
qualification on every supported platform. New simulation SIMD prototypes
are not retained. The missing CPU-feature/qualified-profile exchange in the
NES netplay binding/pinned transport is separately tracked in
`beads-2dw.1.100`; strict executable/ROM/config identity checks are unchanged.
Same-host loopback and cross-compilation do not prove cross-build netplay.

The first digest timing series was discarded in full because its changed
object occupied a different link position. Corrected results use equal
positions (`net-layout-fixed-build.json`). One reference rollback trial with
different peer shutdown frontiers was retained as failed and repeated whole;
equality gates were not relaxed. Default adaptive-delay sessions converge but
are not cross-head equal-input proof. A separate metadata collector accidentally
serialized PowerShell provider metadata; only that collector was stopped.
Timestamp audit found zero admitted overlaps, and collection now uses plain
.NET strings (`nas-results/collector-overlap-audit.json`).

## Final functional qualification

The fixed bundle passed one serial qualification, recorded in
`final-qualification/milestones.json` and its logs:

- MSVC x64 Release with assertions: **37/37 CTest tests**, including audio,
  timing, mod contracts, invalid-state rejection and cross-process FDS native
  progress persistence. Representative full Win32 IA32 interpreter build and
  PPU/HD contract tests also pass.
- Actual native SMB1 default/Fit/co-op2 (2,400 frames each), and SMB3 default/Fit
  (3,600 frames each): exact guest/hardware traces, raw pixels, PCM and composited
  images. Frame-1,000 current-build restore reproduces the remaining execution.
  Compact HD records expand to the complete reference state byte for byte.
- Both final core paths, including forced-original LLE, pass **132** independent
  mapper/PPU/expansion programs at four alignments under the TriCNES comparison
  contract, plus **96-frame** invasive NTSC/PAL stress at four alignments per
  title: visible-frame writes, banking, idle/status reads, IRQ, DMA/DMC,
  reset/restore and frequent next-scanline observations.
- Long core routes with and without synthetic mod state, repeated save/load/save
  and immediate ARGB requests match. Exact HD contracts exercise 210 payload
  byte roundtrips, padding/word/end boundaries, active payloads, short buffers
  and invalid headers without mutation. Active HD CHR ROM/RAM, banking, scroll,
  sprite flips, saved continuation and hidden-window resizing pass.
- Actual final SMB1 UDP loopback, full title configuration seal, fixed six-frame
  delay and both adaptation mechanisms identically off: each peer completes
  **602 simulation ticks, 13 replay ticks, zero desyncs**. Whole states match
  within each pair. Resolved input history and expanded whole state, including
  the rollback trailer, equal the reference. Sizes are 4,034,854 vs 593,382 bytes.
- Both actual native binaries pass hidden, muted SDL/game-UI startup with Fit
  and software presentation. The preboot OpenGL picker is explicitly bypassed
  with `NESRECOMP_NO_LAUNCHER=1`, as it cannot use SDL's dummy video driver.

Evidence: `title-checks/candidate/comparison.json`,
`title-netchecks/final-comparison.json`, `broad/bundle-checks/final_bundle/`,
`broad/events-stress/`, `broad/fixtures/`, and `final-qualification/window-smoke/`.
The original LLE comparison remains buildable with
`-DNESRECOMP_PPU_LLE_REFERENCE=1`. Portable digest contracts remain in normal
CTest. The public CPU helper is `tools/cyc/bench_host_work.py`; private final
workloads are staged by `final_timing_stage.py` and run serially by
`remote_launch.ps1 -Leaf broad-combined`. Completed balanced summaries and
samples are indexed in `nas-results/broad-results-snapshot.json`.

The independent oracle has no PCM output: reference/candidate audio equality
is proven for these inputs, independent whole-game audio accuracy is not.
SMB3's actual native project has no netplay target; earlier generic-framework
SMB3 real rollback checks do not manufacture one. No rewind UI was found;
current-build reset/restore and real rollback are the exercised replay paths.
Physical GPU/DAC, WAN, cross-build netplay and a human gameplay assessment
remain unverified. The existing MinGW weak-extras link issue (`beads-unuw`)
is separate and not bundled.

## Owner handoff and approval gate

Final actual native binaries/catalogs are staged privately in `handoff-final/`;
earlier provisional handoffs are preserved. `final-qualified.json` records the
completed functional gates and explicitly deferred combined CPU comparisons.
`owner-playtest.ps1` verifies that marker and the executable hash, then starts
one normal window with normal audio, Fit enabled,
private configuration/cache paths and progress persistence disabled. It clears
unattended dummy drivers and rollback-test overrides. No owner window is
launched automatically. One combined human playtest per title remains.

No source merge, remote source push, title submodule pin update or final
integration has been performed. Owner approval is the final integration gate.
