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

## Mapper status (checked against the "Mapper 005" notes and nintendulator-mappers' h_MMC5.cpp / s_MMC5.cpp)

Implemented: PRG modes 0-3 with ROM/WRAM per window, $5113 RAM page, RAM write protect ($5102/$5103), CHR modes 0-3 with
the A/B register sets (8x16 sprites use both, 8x8 uses the last written), 10-bit CHR registers with the high bits latched
from $5130 *at write time* (power-on: identity registers), ExRAM modes 0-3 (Ex0/Ex1 writable only while rendering, else $00
is stored; Ex2 RAM; Ex3 read-only), ExAttribute mode, nametable sources per $5105 slot (CIRAM 0/1, ExRAM, fill tile/attribute)
for both the renderers and $2007 accesses, vertical split screen ($5200-$5202), 8x8->16 multiplier, power-on state, and the
registers travel in save states (record `nesrecomp.mmc5`; older save states load as before).

* **PRG-RAM layout**: two chips of 0/8/32KB (ELROM 0+0, EKROM 8+0, ETROM 8+8, EWROM 32+0, ...). RAM bank 0-3 = chip 0, 4-7 = chip 1;
  a bank with no chip is open bus (reads 0, writes ignored). Chosen by `NESRECOMP_MMC5_WRAM=<chip0KB>[,<chip1KB>]`, else the
  CRC32 of the PRG (23 known games: Castlevania III 0+0, Just Breed (J) 8+0, Uncharted Waters 8+8, Romance of the Three Kingdoms II 32+0,
  ...), else the NES 2.0 header, else 8+0. A translated ROM has another CRC: name the layout in the environment or the header.
  The startup log prints `[Mapper] MMC5 PRG-RAM: a+b KB (source)`. Only an 8KB chip 0 is battery-saved; larger layouts use a private buffer.
* **Scanline IRQ / in-frame**: the counter counts rendered scanlines (-1 on the pre-render line, n on visible line n); the pending flag
  is cleared on visible line 0, "in frame" is set from visible line 1, compare value 0 never fires. A $2001 write that turns BG and
  sprites off resets the counter and drops "in frame" for the rest of the frame (a pending IRQ survives); lines drawn with rendering
  off are not counted.
* **Expansion audio**: two pulse channels ($5000-$5007, no sweep, 240 Hz envelope/length; only periods <= 1 are silent), 8-bit PCM $5011,
  status $5015. PCM read mode ($5010 bit 0: every CPU read of $8000-$BFFF becomes the sample) and the PCM IRQ ($5010 bit 7: raised when
  the sample is 0, cleared by reading $5010). Level: full-scale PCM = 255/120 of a full-volume pulse (nintendulator's linear mixing).

Tests (no game needed): `tests/mapper/mmc5_selftest.c` (registers, RAM chips, counter), `tests/mapper/apu_mmc5_selftest.c` (pulse
frequency, 240 Hz length counter, status bits, PCM level, PCM read mode/IRQ, low periods), `tests/mapper/mmc5_rom_tests.py RUNNER.exe`
(generates tiny MMC5 ROMs with `make_test_rom.py`, runs them headless and checks pixels: left/right split, nametable sources 0-3).

Known approximations / not done: the absolute level of the MMC5 audio against the 2A03 channels (only the pulse/PCM ratio is known); the
MMC5 audio state is not part of save states (games rewrite the registers); the split's fine-X/scroll edge cases and the $5202 high bits
are not verified against hardware; the mid-frame counter reset assumes the per-frame renderer clocks the mapper in step with the IRQ handler.

Validated on Just Breed (PRG mode 3, ExAttribute, extra sound registers seen in use) and Castlevania III (intro).

## Islands (routines the decompiler cannot write as C functions) - how they run without an interpreter

Inline-argument routines, jump-table dispatchers, and the "hard" routines (TSX/TXS, pulled return addresses, RTS used as a jump)
are executed as *translated basic blocks* (`generated/mmc5/blocks`): the block C code works on the real 6502 stack in `g_ram`,
so return-address games need no special modelling. The executor (`interp.c`) only dispatches from block to block and performs the
control transfers (JSR/JMP/RTS/RTI/BRK) with the S-floor contract; **it does not read or decode ROM** for anything the generator saw:
every block entry, including the instruction after a conditional branch or PLA/PLP/TXS, and every control transfer (`NesCtlEntry`,
checked against the loaded ROM at start-up) comes from the generated tables. Just Breed over 14.5M instructions: 79% decompiled
C, 19.6% blocks, 1.3% control transfers taken from the table, 0 instructions decoded from ROM.

* `NESRECOMP_STRICT=1` logs every address the executor had to decode (code that was never translated); `=2` stops at the first.
  Feed those addresses back with `NESRECOMP_BLOCK_MISS_FILE` -> `analysis/seeds.txt` (or more coverage) and regenerate.
* A block table entry and a decompiled function both carry an FNV hash of their bytes: a translated ROM that patches code falls back to
  the decode path for exactly those routines.
* Code that is not in the tables (never executed while recording coverage and not found by static analysis) and code running from RAM
  still falls back to the decode-based interpreter; that is the only place the interpreter is still needed.
