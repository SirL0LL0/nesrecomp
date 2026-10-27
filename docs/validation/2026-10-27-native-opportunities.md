# NES native runtime opportunity burndown

Tracking: `beads-2dw.1.101`, related to optimization campaign `beads-2wy8.1`.
Isolated branch: `perf/native-runtime-20261027`.
Baseline engine: `b795ada2127fa3a4ad212f5d8dd494db3339032d`.
Measured runtime implementation: `67691b9d6bc6f72a82184278688ffbe4917dd6c1`.
Later CLI/game release-version changes do not alter that hardware implementation.

## Accepted representative runtime results

| Game | Median paired CPU reduction | Decision |
|---|---:|---|
| Super Mario Bros. | 2.74% | Modest positive on the tested workload |
| Super Mario Bros. 3 | 1.70% | Neutral/inconclusive: small mixed pairs; no proven gain |
| Metroid | 13.94% | Positive on the tested stock workload |
| The Legend of Zelda | 11.25% | Positive on the tested stock workload |
| Gumshoe | 12.88% | Positive for the combined package, including 2635 new ROM seeds |

All five cases pass independent pre/post A/A stability and alternating A/B
admission gates. SMB3's five reductions include -0.15%; do not market its small
mixed result as a proven win. No tested game has a reproducible regression.
SMB1/SMB3 already used HLE on Windows x64: their references retain it. The
supplementary explicit-LLE SMB1 result is not new production-default credit.

Measurements use standard production game binaries without qualification observers on SERVER, SDL software rendering
at 1280x720 and audio emulated, mixed and queued through dummy output. Five
alternating 1200-frame pairs measure frames 1801-3000; TCP setup is excluded,
normal shutdown included. Excluded full-3600-frame no-trace SDL observers match
canonical hardware/memory, produced PCM and composed picture. Gumshoe's timed
Zapper is fixed at (40,160), trigger true; separate functional probes pulse it.

Raw numerical arrays are in [2026-10-27-native-results.json](2026-10-27-native-results.json).
Complete source/binary identities, settings, raw samples, frozen harness and
captures are retained under the campaign evidence root
`G:/Temp/Codex/nesrecomp/native-20261027`: `campaign-results.json`,
`evidence-nas-tcp-1417`, `evidence-nas-final-gate-1535` and
`evidence-nas-final-gate-1610`. Rejected samples remain excluded.

## Opportunity decisions

| Opportunity | Evaluation / decision |
|---|---|
| Stable PPU classification | Selected in the combined native-service package. Reuse proven stable eligibility until a real invalidation; preserve all events. |
| Native PPU cycle service | Selected. Fuse a stable CPU-cycle tail while retaining every dot/half-dot, mapper notification and interrupt sample; fall back at unsupported edges/PAL alignments. |
| Packed PPU kernels | Reuse existing packed HLE kernels. Newly select HLE only for qualified Windows x64 Metroid/Zelda/Gumshoe defaults; preserve explicit cache choices and other-platform fallback. Existing SMB defaults are not a new gain. |
| APU output service | Selected. Cache base-channel output between actual changes; retain all timers, expansion-audio contributions and float accumulation order. Derived cache is not serialized. |
| Native memory accesses | Evaluated. Mirrored RAM is already direct and immutable PRG reads already fold to constants. Do not bypass observable device clocks/bus behavior. Further cross-unit elimination is deferred with selective IPO. |
| Native dispatch coverage | Gumshoe's supported miss log adds2635 seeds, removes none, and cuts observed ROM fallback17,602,316 to801 of107,207,423 cycles. Selected only as part of its qualified combined package; no seed-only speed claim. |
| Cartridge-RAM native views | Evaluated/deferred as `beads-2dw.1.105`. Zelda's10.1% emulated-cycle share is not hostCPU attribution. FDS views cannot simply cover cartridge stores/debug pokes or SNROM read gates changing on PPU A12. Need guarded mapping plus write/mapping invalidation; retain interpreter. |
| Duplicate stock/custom rendering | Evaluated; generic pixel bypass not selected. Raw pixels, palettes, status/sprite events, mapper clocks and Zapper observations remain required. Native services remove control work while retaining them. |
| Frame conversion/reuse | Evaluated existing generation caching/once-per-frame conversion. No new content-comparison cache selected without demonstrated benefit. |
| State/rollback | Reuse already integrated fused digest and inactive-HD savings from `0302a6f`; no duplicate credit or repeated negative allocation/lazy-scratch experiment. Current-build replay/rollback verified. |
| Idle cartridge work | Selected MMC1 stability change. Always retain A12 pin transitions; rebuild maps only when CHR-register selection changes CPU-side C/D/E signals. Mode/read-gating/board tests and all8 affected title profiles pass. |
| Production observability | Optional per-cycle tracing is inactive in timed launches; normal always-on device event history is retained in both arms. Legacy stack/ring cache keys are not runtime controls, so no disabling benefit is credited. Qualification observers never execute inside timed game binaries. |
| Compiler optimization | Whole-program IPO rejected for actual SMB1 Fit pixel and guest-memory divergence. Native-C-only IPO passes5 functional profiles but lacks admitted runtime gain/supported production profile; excluded and deferred as `beads-2dw.1.106`. |

The gains belong to each combined game package, not to individually attributed
PPU/APU/MMC1 components. Gumshoe alone receives expanded ROM seeds.

## Correctness, scope and owner acceptance

All13 current-title profiles pass3600-frame hardware/guest trace, PCM,
native/composed picture and own-build serialized replay comparisons. Incremental
MMC1 passes all8 affected profiles. Gumshoe's final supported combined build
passes stock/Fit/Zapper full comparisons and current-build replay. Actual hidden
SDL viewport/input/state checks pass across5 feature setups. Active HD CHRROM/RAM,
banks, scroll, sprites/flips/colors/fallback/restore and accelerated resize pass.
SMB1 same-binary stock/wide rollback with a hidden SDL peer passes; this is not
WAN or cross-build compatibility evidence.

Nine NTSC/PAL alignment cases pass on GCCx64 and MSVCWin32 IA32, including HLE's
scalar fallback. Mapper board/read-gating/banked-RAM cases pass on both compilers.
Save-state backward compatibility is not required; native saves remain protected.

Metroid voxel final presentation after reload differs equally in original and
candidate builds (`beads-2dw.6.21`); guest/state replay and cross-build observations
match. This shared defect does not disqualify the optimization.

Extra Metroid enhanced timing failed baseline-only A/A before a candidate
comparison; Zelda voxel timing was not completed. Those optional expanded timing
cases remain unverified. Do not claim measured improvement in every feature mode.
The13-profile functional gate covers those feature behaviors separately.

The owner approved the presented batch in direct normal windows with normal
audio, after requesting SMB1 and Metroid21:9. Preserve that approval; do not repeat
routine visual handoffs. Main integration and fresh pinned title/CLI builds are
authorized. No blog publication is authorized.

Packaging audit clarification: the ordinary device event ring stays active in production. The legacy `NESRECOMP_ENABLE_POSTMORTEM_RINGS` / stack cache keys do not disable it. This corrects the earlier description, not the measured binaries or numerical results. Per-cycle trace enablement is host-controlled and is not restored by loading the traced setup state; timed launches leave it inactive. Fresh release generation with the new MSVC native compiler emits byte-identical C in all124 generated units across the five titles. Exact shipping executables pass720-frame old/new hardware, PCM, pixels and own-build replay comparisons.
