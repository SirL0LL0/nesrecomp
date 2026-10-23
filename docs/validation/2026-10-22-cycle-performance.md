# Accuracy-preserving cycle performance investigation

Tracking: central Beads `beads-vy21`. Working branch: `perf/accuracy-20261022`.
Reference: `a76140ceded76510c797155282358390ab474a92`.
Evidence root: `G:/Temp/Codex/nesrecomp/accuracy-perf-20261022`.
The original checkout and its unrelated `tools/cli.py` edit were preserved.

## Task burndown

| Task | Beads | Status |
| --- | --- | --- |
| Repository boundaries, fixed reference, two-title fixtures | `beads-gi2y` | Complete |
| Attribute CPU, mapper, PPU, audio, copying, allocation, synchronization, replay | `beads-hnu4` | Complete for the declared profiles; limitations below |
| Independently measure candidates with A/A and ABBA controls | `beads-qklx` | Complete: fused digest retained; two serializer candidates rejected |
| Current-build state and real two-title rollback qualification | `beads-4x05` | Automated qualification complete |
| Normal-window/audio owner handoffs and integration approval | `beads-wepn` | Handoffs prepared; human playtests and approval pending |

The workspace container `F:/Projects/nesrecomp` is not a Git repository. Its
`nesrecomp/` checkout was on the older legacy branch at `4e4a133`. Current
game projects use the cycle backend, so this investigation uses an isolated
worktree of current `origin/master` on G:. No existing checkout was reset.
Work was performed solo.

The pilot is Super Mario Bros. (`88424dd`, NROM). Super Mario Bros. 3
(`9d9d999`, MMC3) supplies different CHR banking and raster IRQ behavior.
Their original ROMs and seed files were copied into ignored/private evidence;
ROM bytes are not part of this change. Neither battery files nor title-local
configuration were used or modified.

## Attribution and console contract

Diagnostic sampling used the uninstrumented GCC 15.2 x64 `-O2` reference,
LLE PPU, native guest code, stock features, 48 kHz PCM and one ARGB conversion
per frame. Each route ran 2,400 frames. Comparison hashes and per-frame dumps
were disabled during sampling. These are main-thread instruction-pointer
samples, collected on the busy desktop, **not CPU speedup measurements**.
Inlining charges work to its containing function/object.

| Source attribution | SMB1 samples | SMB3 samples |
| --- | ---: | ---: |
| PPU hardware, fetches, sprite evaluation and pixel pipeline | 68.07% | 69.90% |
| APU, DMA/controller work and audio production | 15.49% | 14.37% |
| Mapper handling | 6.50% | 6.72% |
| Machine bus and master-clock scheduling | 6.25% | 6.11% |
| Other CPU/native/dispatch/runtime work | Remainder | Remainder |

The native/generated CPU and scheduler have low exclusive sample shares;
the hardware called from them dominates. This does not justify reducing
guest CPU cycles. Mapper call costs include notifications whose bodies may
be inert for a particular board; removing those host call boundaries is a
future candidate, not a measured result here. MMC2/MMC4 read-release latches,
MMC5 read detection and FDS snoops must remain in any such experiment.

The PPU's sprite-zero/overflow state, A12/IRQ transitions, address and scroll
latches, CHR selection, palette changes and open-bus behavior remain executed.
The ARGB conversion consumes completed color indices; replacing that image
does not make the preceding PPU work removable. No PPU, APU, DMA, interrupt,
mapper or CPU scheduling implementation changed in the retained candidate.
No tile, palette, OAM or framebuffer cache was introduced.

Copies and allocations matter on the state path, even though they are mostly
absent from normal offline frames. The measured stock snapshot is 326,548
bytes; the rollback trailer makes it 326,560. The original serializer grows
an output buffer and allocates cartridge, host and mod scratch blocks on each
save. It allocates the 1 MiB mod scratch block even with zero registered hooks.
The cartridge temporary is copied into the output a second time. These were
tested individually below; plausible allocation savings did not become a
whole-workload win.

Rollback snapshots and digests share one cached image until invalidation.
The original digest traverses every byte for the master FNV hash, then
traverses the header, chunks and trailer again for three partition hashes.
The candidate keeps both independent FNV recurrences and visits each chunk
once. It preserves the existing treatment of malformed/trailing bytes:
they enter the master hash, while only complete chunks enter a partition.
It changes neither the serialized domain nor replayed hardware work.

Offline sampling excludes SDL presentation, audio-device synchronization and
frame pacing. Real rollback runs separately report live and replay wall cost,
state save/load cost, replay count and total process CPU. Those wall phase
timers are not mislabeled as isolated CPU costs. A dedicated CPU attribution
of GPU/audio-device synchronization and each replay phase remains unverified.

## Measurement method

The desktop was saturated by other authorized tasks. Healthy jobs were left
running. Following the owner's direction, SSH access documented in `F:/NAS`
was used for a serial queue on SERVER (Intel i5-4670, four cores, Windows 10).
All remote scratch, WAVs, ROM copies and process-local TEMP/TMP were on its
G: disk. No service was stopped or reconfigured.

Both heads have identical generated guest objects and features. The baseline
and serializer variants also share runtime objects; only the candidate source
object differs. Binary/input SHA-256 manifests accompany the raw evidence.
The portable fused digest additionally passed MSVC 19.41 Win32 `/O2 /arch:IA32`.
No new CPU instruction requirement or assembly path was introduced.
This qualifies the scalar digest arithmetic on another ABI; it is not a
cross-build netplay compatibility claim. Existing executable/ROM identity
checks remain in place, and every real peer pair uses one exact executable.

Measurements use process user+kernel CPU, with wall time retained separately.
The sequence is warm both heads, four A/A legs, then three four-leg ABBA/BAAB
blocks. Setup and warmup are included identically in process CPU. The process
timer has Windows accounting granularity; runs last several CPU seconds.
Every sample is retained. Profilers and comparison tracing are excluded.
An advisory byte lock is shared at
`G:/Temp/Codex/performance-measurement.lock` on the measurement host.
The netplay workload is paced normally; the metric is CPU consumed by the
same complete match, not paced FPS or wall-time throughput.

The first whole-session timing series was discarded after review found that
the baseline digest object had been appended after the netplay facade objects.
The corrected consumer replaces its source at the same list index. Both
titles' complete object order now matches except for that digest object,
verified in `net-layout-fixed-build.json`. The fresh series is isolated under
the NAS task's `net-layout-fixed/` directory and monitors other game/compiler
processes throughout each leg, waiting and excluding an overlap before retry.

## Candidate decisions

Positive percentages below mean reduced CPU time.

| Candidate | Workload | A/A paired variation | ABBA block reductions | Decision |
| --- | --- | --- | --- | --- |
| Remove temporary cartridge allocation/copy | SMB1, 600 frames, one complete save and ARGB request/frame, 48 kHz PCM | +0.30%, 0.00% | -0.60%, -0.60%, +1.19% | Rejected: no repeatable gain |
| Allocate mod scratch only when a hook needs it | Same workload | +1.20%, +0.90% | -1.20%, -0.45%, 0.00% | Rejected: no repeatable gain |
| Fused exact master/partition digest | 3,000 digests of an actual SMB1 rollback image | -1.43%, -0.70% | +48.41%, +48.04%, +50.18% | Directional subsystem result; retention uses session results below |
| Fused exact master/partition digest | SMB1, real two-peer rollback match, 48 kHz PCM | -1.89%, -2.30% | +6.14%, +3.80%, +3.33% | Retained: median block CPU reduction 3.80% |
| Fused exact master/partition digest | SMB3, same rollback profile | +1.47%, -5.47% | +7.04%, +4.03%, +3.84% | Retained with noisier control: median block CPU reduction 4.03% |

These session percentages use the sum of both peers' process CPU. All six
candidate totals were below all six reference totals for SMB3, and every
balanced block improved for both titles. Its A/A control nevertheless moved
by as much as 5.47%; the reported percentages describe this host and profile,
not a precise universal speedup. Full samples remain in
`nas-results/net-layout-fixed/{smb1,smb3}/samples.json`.

Every admitted session completed identical live-frame counts (362 and 374
for the two peers) and seven replay ticks per peer. All 18 admitted runs per
title, including warmup and A/A controls, produced the same complete final
serialized state across heads and peers. One reference leg stopped at a
different confirmed-frame frontier; it was excluded, retained in the raw
logs and repeated without weakening the gate. The first timing series with
different linker object order was discarded in full. Final comparisons
replace the digest object at the identical position in both link commands.

Phase wall timers provide context, not independent CPU attribution. The
median of peer p50 values over the six measured legs per head is:

| Title/head | Live frame ms | Replay frame ms | State save us | State load us |
| --- | ---: | ---: | ---: | ---: |
| SMB1 reference | 8.939 | 8.986 | 229.15 | 86.30 |
| SMB1 fused | 8.924 | 9.002 | 221.60 | 85.70 |
| SMB3 reference | 9.409 | 9.363 | 485.05 | 340.85 |
| SMB3 fused | 9.472 | 9.457 | 497.55 | 340.95 |

Digest work happens outside these frame/save/load timer scopes. Those phase
numbers do not establish a speedup; the whole-process CPU comparison does.
Only the fused digest is retained. No per-game specialization, reduced
hardware execution or cache was needed for this win.

The serializer candidates preserved every capture but were not retained.
Their source variants remain in private evidence (`state-cart.c`,
`state-lazy.c`). No allocation-count reduction is advertised as a frame-rate
improvement. The first NAS queue did not monitor other investigations for its
entire duration; the final session queue explicitly waits for other game and
compiler processes and refuses an overlapping sample.

## Correctness evidence

For **both titles**, each serializer variant and the reference ran the same
600-frame input route, with and without a synthetic 4 KiB mod state record.
All full hardware/guest hash lines, framebuffer-index logs, PCM WAVs and final
serialized bytes matched. Every frame also tested repeated serialization,
save/load/save byte equality and repeated ARGB requests. TriCNES's independent
whole-machine oracle matched all 600 observable trace/memory/picture hash
lines. Its internal hash representation is different and was excluded using
the existing comparison contract. This oracle build has no PCM output;
independent oracle audio remains unverified. Reference/candidate PCM is exact.

A separate 12-frame stress run injects native RAM/OAM/CIRAM writes and
CPU-bus scroll/palette writes during visible scanlines; SMB3 also changes MMC3
CHR banks. It resets before restoring each current-build state, checks
save/load/save equality, repeats rendering and verifies invalid-header
rejection without memory mutation. Reference and both serializer candidates
have identical final state/picture/hardware and audio results. These invasive
inputs test implementation equivalence; they are not ordinary gameplay or
an independent hardware oracle claim.

The digest regression test compares master and every partition against an
independent definition of the original two-pass algorithm. It includes empty,
short, valid, zero-length, unknown-tag, oversized-chunk and truncated images.
It passes under GCC x64 and MSVC Win32 IA32 with assertions active, and through
the normal `cyc_rb_digest_test` CTest target.

Real UDP loopback runs use the pinned `recomp-net` `b0751b7` and rbengine
`2a03e73`, two peers with actual input prediction/corrections, and normal
frame replay. For each title, reference and fused heads separately completed
362 simulation ticks with seven actual replay ticks and zero desyncs.
Complete final images matched both peers and both heads:

- SMB1: `36696a0ee2cb9abcb64d75303831355946ca1cfd62a08616f35e99805b28e5bf`
- SMB3: `385dcfaf52534b5fead042fe7977f1dbf8c396d25de76438c66785db9913931d`

The generic stock host correctly refused the SMB-specific `coop`/`ws` seal
in the first harness attempt. The subsequent runs use its supported `vw=256`
seal; no compatibility checks were weakened. Original fixtures were not edited.
WAN/cross-machine compatibility and user-facing rewind were not qualified.
The current stock cycle host exposes state load and rollback, but no rewind
UI was found; reset/restore and actual rollback are the available exercised
paths. Old save-state compatibility is not a gate. Battery persistence is
untouched; all unattended runs disable persistence and use private ROM copies.

The final SDL-enabled stock binaries also match the reference for a
2,400-frame SMB1 route and a 3,412-frame SMB3 title/world-map route: all guest
and hardware hash lines, PCM, final PNG and save bytes are equal. Restoring
frame 1,000 reproduces all remaining hash lines (1,399 and 2,411 frames), and
reference/candidate loaded PCM matches. The SMB3 route does not establish
level-entry coverage; its world-map/raster and injected CHR-bank checks are
the qualified coverage. No stage mechanic is claimed from the final picture.
Mixed hidden-SDL/headless peer tests additionally converge on both titles,
with seven replay ticks and zero desyncs per run. All unattended audio is muted
at the output device while PCM emulation remains enabled.

## Evidence and remaining gate

The private evidence root contains `consumer/`, reference/candidate source
copies, `build-probes/`, `build-digest/`, `build-digest-win32/`, `checks/`,
`stress/`, `profiles/`, `netchecks/` and `nas-results/`. `samples.json` files
contain every CPU/wall sample, arguments and hashes. NAS measurements remain
under the same task path on its G: disk until copied back.

The private owner launcher is `../owner-playtest.ps1` relative to the framework
worktree. Its binaries and instructions are in `../handoff/`. It explicitly
selects a normal window, enabled audio at normal volume, private configuration
and the qualified software SDL presentation so driver shader caches do not go
to C:. A second local peer stays hidden and uses a dummy output device. These
are stock cycle-consumer handoffs; enhanced title mods and GPU presentation
have not been qualified by these measurements. No owner-facing window has
been launched during unattended work.

The standalone unmodified MinGW cycle host also exposed an existing weak
`cyc_host_extras` link failure, tracked separately as `beads-unuw`. The probes
supply a strong extras definition identically on both sides. No unrelated
shipping link fix is bundled here.

Automated qualification is complete for the profiles declared above. Human
playtests and approval remain pending. No final integration or title pin
update is authorized before the owner reviews one combined handoff per title.
