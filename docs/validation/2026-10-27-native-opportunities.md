# NES native runtime opportunity burndown

Tracking: `beads-2dw.1.101`, related to optimization campaign `beads-2wy8.1`.
Isolated branch: `perf/native-runtime-20261027`.
Evidence root: `G:/Temp/Codex/nesrecomp/native-20261027`.
Machine-readable list: `opportunities.json`; source freeze: `current-heads.json`.

Each game must remain neutral or improve. Mixed small measurements are
inconclusive. Matching original/candidate failures are separate existing defects.
Correctness includes machine observations, PCM, pixels and current-build replay.
No save-state compatibility across implementations is required.

| Opportunity | Evaluation and remaining decision |
|---|---|
| Stable PPU classification | Cache eligibility across already proven stable spans; portable exact prototype passes differential checks. Current-game qualification and timing underway. |
| Native PPU cycle service | Execute one complete stable CPU-cycle tail while retaining every dot/half-dot, mapper notification and interrupt sample. Exact prototype passes; measure separately from classification cache. |
| Packed PPU kernels | Current 13-profile game differential passes, including own-build replay. Active HD CHR ROM/RAM, banks, scroll, sprite flips/size, color modes, fallback, continuation and actual accelerated hidden-window resize tests pass. Scalar fallback builds/runs without SSE2. Runtime measurement still decides selection. |
| APU output service | Cache the base channel level between audible changes; preserve every timer, float accumulation and expansion-audio contribution. Exact prototype passes; measure alone and with PPU work. |
| Native memory accesses | Ordinary RAM already uses direct mirrored loads and generated immutable ROM reads are folded constants. Test remaining cross-unit call cost with LTO rather than bypassing observable bus/device work. |
| Native dispatch coverage | SMB1/SMB3 routes are approximately 100% native with small matched fallback counts. Current Zelda is 89.8% native, with 10.1% of cycles executing cartridge RAM. Evaluate that separate target; LTO also evaluates call overhead. |
| Cartridge-RAM native views | Evaluated, not selected: Zelda's 10.1% emulated-cycle count is not host CPU attribution. Existing FDS support cannot simply be enabled: ordinary cartridge stores/debug pokes lack the code watch, and SNROM read gating can change on PPU A12 during a folded fetch. A guarded cartridge-RAM feature with stable mapping, write/mapping invalidation and restore tests is deferred as `beads-2dw.1.105`; keep the interpreter. |
| Duplicate stock/custom rendering | Custom renderers still depend on the cycle machine's status, palettes, sprite events and raw observations. Generic pixel bypass is not established safe; native PPU services preserve these effects. |
| Frame conversion/reuse | Wide composition already caches by generation; stock converts once per frame. No extra content-comparison cache retained without a demonstrated saving. Check actual SDL presentation cost. |
| State and rollback | Reuse baseline's accepted fused digest and zero inactive-HD storage from `0302a6f`. Previous allocation/copy and lazy-scratch experiments were neutral/negative; no repeated gain claim. Verify current-build rollback with retained package. |
| Idle cartridge work | Existing inert PPU read guard retained. New MMC1 candidate keeps A12 pin transitions but rebuilds mapping tables only if selecting a CHR register changes CPU-side C/D/E signals. GCC x64 and MSVC Win32 mapper tests pass; full current-title comparison and timing decide retention. |
| Production observability | Release comparisons disable trace/stack/rings/startup diagnostics. No continuously executing profiler/verifier in timed game builds. |
| Compiler optimization | Whole-program IPO is excluded: SMB1 stock matches, but Fit changes composed pixels and then guest memory at frame 933. Native CPU/hardware/generated-C-only IPO, retaining original host/HLE/snapshot/mod/UI objects, passes full SMB1 stock/Fit/co-op and SMB3 stock/Fit. This is a separate package, not proof of a root-cause fix. Measure it before adding a production build profile; details in `beads-2dw.1.106`. |

Final current-title suite: Super Mario Bros., Super Mario Bros. 3, Metroid,
The Legend of Zelda and Gumshoe, including representative supported feature modes.
Development captures from older title heads are explicitly superseded for final
qualification. No performance result is claimed while host reservations prevent
matched measurements. Preserve raw A/A and alternating A/B samples and exact
package attribution before recording a win.

Functional receipts are separate from performance acceptance. Current native
PPU/APU services pass all 13 title profiles over 3,600 frames; packed HLE also
passes those profiles. The incremental MMC1 change passes all eight affected
Zelda/Gumshoe/Metroid profiles, including custom renderers. Guest traces, PCM,
native/composed pixels and own-build serialized replay are exact. Metroid's
voxel final presentation after reload differs equally in both builds and is
tracked independently as `beads-2dw.6.21`.

Actual hidden SDL viewport/input/state checks pass in five feature setups.
SMB1 same-binary stock/widescreen rollback checks pass, including a hidden SDL
peer. These do not establish WAN or cross-build compatibility. Nine NTSC/PAL
alignment cases pass on GCC x64 and MSVC Win32 IA32, including packed HLE's
scalar fallback. Mapper mode/read-gating/banked-RAM checks pass on both compilers.
Gumshoe's separately tracked seed candidate (`beads-2dw.16.3`) reduces observed
fallback cycles from 17,602,316 to 801 with exact stock/Fit/Zapper results;
native coverage itself is not a speed measurement.

All implementation packages remain experimental until matched A/A-controlled
runtime measurements pass per game. No default HLE/IPO switch, main merge or
release is implied by this functional qualification.

After technical qualification, leave normal-window, normal-audio candidate games
with prepared setups and locally saved owner observations. The owner's explicit
good response authorizes integration and new title releases pinned to the merged
engine. No merge or release before that response.
