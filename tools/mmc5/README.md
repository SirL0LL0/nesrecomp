# MMC5 backend

NESRecomp's C recompiler models 16KB banks plus one fixed bank. MMC5 has four 8KB PRG windows that the game switches
at run time (ROM or WRAM). Instead of extending that model, MMC5 games run through a backend that works on 8KB
*units* and resolves code through the live window mapping.

```
game.nes ──► NESRecomp.exe (mapper 5) ──► tools/mmc5/mmc5_regen.py
                                             │ disasm.py     analysis/all.bin ──► disasm/unitNN.asm (re-assembles byte-identical)
                                             │ blocks.py     ──► generated/mmc5/blocks   basic blocks translated to C
                                             └ decompile.py  ──► generated/mmc5/decomp   C functions (+ decomp_r: readable)
runner: mmc5.c (mapper) + interp.c (6502 interpreter tier) + mmc5_tier.c (boot, dispatch, diagnostics)
```

## Use in a game project

```cmake
include(${NESRECOMP_ROOT}/runner/runner.cmake)
add_executable(MyGame ${NESRECOMP_RUNNER_SOURCES} extras.c)
nesrecomp_mmc5_tier(MyGame ${CMAKE_SOURCE_DIR}/generated/mmc5)   # mmc5_tier.c + generated blocks/functions
```

```
python <nesrecomp>/tools/mmc5/mmc5_regen.py --rom game.nes      # or: NESRecomp.exe game.nes
cmake --build build
```

Without any generated code the game already runs: everything goes through the interpreter, which also *records
coverage*: `NESRECOMP_COV_FILE=analysis/run1.bin game.exe game.nes`. Merge sessions with `cov_merge.py`, feed
`analysis/all.bin` to the next regeneration, and more code becomes translated. Random-input exploration from save states:
`explore.py`.

Per-game files (in the game directory, all optional):
* `analysis/all.bin` (+ `.win`): coverage;  `analysis/seeds.txt`: extra entry points `unit addr` (hex);
* `analysis/symbols.tsv`: `R addr name` for RAM, `F unit:addr name` for functions.

## Runtime switches

`NESRECOMP_BLOCKS=0` / `NESRECOMP_DECOMP=0` disable a tier; `NESRECOMP_INTERP_HIST=N` lists the most-run interpreted
instructions; `NESRECOMP_BLOCK_MISS_FILE=f` writes addresses run by the interpreter that were not translated (append
them to `seeds.txt`); `NESRECOMP_BOUNDARY_LOG=f[,N]` + `seqdiff.py` finds the first diverging instruction between two
runs; `NESRECOMP_DECOMP_VERIFY=N` compares each decompiled function with the interpreter from the same state.

## Validation

`decomp_check.py RUNNER_DIR ROM --exe NAME.exe`: same input script with and without decompiled code, final save-state
hashes must match. Generated code carries a hash of the ROM bytes it came from; if they differ (patched/translated ROM)
that block or function is skipped and the interpreter runs it.

## Mapper status (checked against the "Mapper 005" reference notes)

Implemented: PRG modes 0-3 with ROM/WRAM per window, $5113 RAM page, RAM write protect ($5102/$5103), CHR modes 0-3 with
the A/B register sets (8x16 sprites use both, 8x8 uses the last written), 10-bit CHR registers with the high bits latched
from $5130 *at write time*, ExRAM modes 0-3 (Ex0/Ex1 writable only while rendering, else $00 is stored; Ex2 RAM; Ex3
read-only), ExAttribute mode, nametable mapping incl. fill mode, 8x8->16 multiplier, scanline IRQ, power-on state, and
the registers travel in save states (record `nesrecomp.mmc5`; older save states load as before).

Not implemented: expansion audio (2 pulse channels $5000-$5007, PCM $5011, status $5015 - registers are stored only),
vertical split screen ($5200-$5202; only Uchuu Keibitai SDF uses it), the "in frame" flag dropping when $2001 turns
rendering off mid-frame, and PRG-RAM above what the iNES header declares (set `NESRECOMP_MMC5_WRAM=64` for games that
swap RAM pages such as Uncharted Waters; that buffer is not persisted).

Validated on Just Breed (PRG mode 3, ExAttribute, extra sound) and Castlevania III (intro). Not done yet: translating the
interpreter "islands" (inline-argument routines, computed dispatch) so no interpreter is needed at run time.
