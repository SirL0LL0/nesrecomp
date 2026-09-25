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

## Status / limits

Validated on Just Breed only (PRG mode 3). Modes 0-2 map onto the same 8KB-unit model but are untested. Not done yet:
MMC5 audio (pulse/PCM), split screen, translating the interpreter "islands" (inline-argument routines, computed
dispatch) so no interpreter is needed at run time.
