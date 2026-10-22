# Legacy differential co-simulation

`tools/nes_cosim.py` coordinates the function-level runner's instrumented
traces and an independent Mesen libretro oracle via `nesref`. It reports the
first disagreement in the sampled machine state. This guide describes that
legacy tooling, not qualification of the current cycle backend. For current
runtime and test entry points, use the [cycle guide](../runner/cyc/README.md)
and [test guide](../tests/README.md). Paths below are relative to the repository
root.

## Commands

| Command | Comparison |
| --- | --- |
| `gate1 <exe> <rom> [frames]` | Two recomp runs: byte-identical trace determinism |
| `gate2 <nesref> <core> <rom> [frames]` | Two oracle runs: RAM trace determinism |
| `gate3 <exe> <rom> [frames]` | Fault injection: detect the injected frame/subsystem |
| `abram <exe> <rom> <nesref> <core> [frames] [script]` | Stack-masked 2 KB WRAM against Mesen |
| `abcycle <exe> <rom> <nesref> <core> [frames]` | Per-frame cycle advances and drift |
| `abppu <exe> <rom> <nesref> <core> [frames]` | OAM, palette and nametable memory |
| `abaudio <exe> <rom> <nesref> <core> [seconds] [keep_dir] [script]` | PCM pitch, onsets, timbre and drift |
| `abstate <exe> <rom> <nesref> <core> [frames] [script]` | State comparison at a scripted anchor |
| `diff <a.jsonl> <b.jsonl>` | First divergence between two hash streams |
| `run <exe> <rom> <out> [frames] [inject]` | One instrumented trace |

Gates print PASS/FAIL and return a nonzero exit code on failure. RAM/PPU/cycle
comparisons measure different domains; do not infer a full-system pass from
one matching domain.

## Alignment and measurement limits

A deterministic free-run attract sequence can be compared with a constant boot
offset. The legacy trace tags rows with cycle-derived `g_cosim_vframe`, rather
than counting NMI callbacks: boot work, suppressed boundaries and NMI-off
transitions can elapse video frames without firing an NMI. Sampling remains
post-NMI, with boundary sampling for NMI-off frames. This prevents an index
stall from looking like state divergence.

The coordinator's adaptive offsets and horizontal scroll-phase matching help
classify residual differences. A better match after shifting identifies an
alignment candidate; it does not prove that the underlying timing is correct.
Scripted runs must compare the same reached state. `COSIM_SCRIPT` or the
supported trailing script argument drives input; `abstate` compares at an
explicit scripted point rather than treating arbitrary aligned frames as the
same gameplay moment.

These limits remain relevant when interpreting the recorded results:

- Host-modeled state can drift from oracle state. The legacy MM3 fiber
  scheduler's zero-page state was not bit-faithful; attract comparisons did
  not cover in-stage MMC3 IRQ behavior. Reach a deterministic stage fixture
  before claiming coverage of the status-bar split or IRQ timing.
- Frame hashes and mean cycle counts do not establish per-dot identity,
  intra-instruction bus placement, DMA alignment or IRQ fire timing. Even
  matching frame-boundary MMC3 counter registers would leave those event
  timings unproven.
- Freezing an RNG seed can help reproduce a trajectory but does not align
  every frame counter or host scheduler. There is no general Mesen-to-recomp
  save-state conversion in this guide; use targeted input/anchor fixtures.
- RAM convergence does not establish PPU extraction correctness, displayed
  pixels, audio fidelity or gameplay feel. Check each relevant domain.

## Running safely

Build a game against the matching instrumented legacy runner configuration;
see the game's build instructions and [framework build guide](../README.md#build-nesrecomp-from-source).
Use an isolated disposable executable/profile and a copy of the ROM. The
coordinator's `clear_saves()` deletes `<exe_dir>/saves/*.srm` before recomp
launches to start battery games consistently. Never point it at an owner's
installed game or persistent saves.

Set `TEMP`/`TMP` and applicable profile/cache/output paths to ignored project
storage or an F:/G: scratch directory before running. Keep automated launches
muted, and use a hidden process wrapper on Windows. For example, from a game
checkout, supply the actual local paths to these coordinator commands:

```text
python ../nesrecomp/tools/nes_cosim.py gate1 <isolated-exe> <copied-rom> 900
python ../nesrecomp/tools/nes_cosim.py abram <isolated-exe> <copied-rom> <nesref> <core> 900
python ../nesrecomp/tools/nes_cosim.py abppu <isolated-exe> <copied-rom> <nesref> <core> 900
```

## Recorded legacy coverage — 2026-07-02

The original checkpoint reported SMB RAM 99.94%, OAM 99.96%, nametables 99.97%
and mean cycle drift +0.000; Faxanadu OAM 100% and nametables 99.92%; and MM3
OAM 99.48% and nametables 99.92%, with the host-scheduler exclusions above.
Gates 1/2/3 passed for those recorded runs. These are historical attract-run
measurements, not fresh tests or campaign coverage.

Zelda's apparent frame-counter divergence resolved to an NMI-index stall:
cycle-derived indexing recovered 100% RAM convergence. Low PPU scores remained
an alignment/extraction investigation; matching RAM alone did not certify them.

Gumshoe's NMI-off index shift was also corrected, but a real residual remained:
RAM 99.36%, nametables about 71% (about 91% after a scroll shift), with timers
one frame ahead after the transition around frames 367–373. This distinguishes
an actual timing difference from the fixed index artifact. Preserve that
boundary when interpreting a high aggregate match.

Further June investigations are retained in the
[historical accuracy record](validation/accuracy-history.md).

## Validation rules

- Do not ship timing/default changes on headless co-sim numbers alone. The
  existing owner playtest requirement still applies to gameplay changes;
  environment-gated tooling changes can be verified headlessly.
- Build the measurement first, then change timing and compare it. Record the
  backend, build, inputs, masks, alignment and measured domain with a verdict.
- Run one instance per game at a time.
