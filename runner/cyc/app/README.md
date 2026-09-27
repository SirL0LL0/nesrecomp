# runner/cyc/app - application layer for cycle-backend games

Turns a game built with `nesrecomp_add_cycle_game` (see `../PROJECTS.md`) into a complete application:

| Piece | What it does |
|-------|--------------|
| `cyc_app.c` `main()` | [recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui) launcher (ROM check by CRC32, settings, controls, **Mod** page), or the command line; then the mod runtime; then `cyc_host_main` (the generic host in `../cyc_host.c`, built with `main` renamed) |
| `cyc_app.c` `cyc_sdl_main()` | interactive host: window, audio, keyboard/gamepad (keybinds.ini), in-game menu (recomp-ui runtime UI, Esc), save-state slots, fast forward |
| `../cyc_ext.c` | what a game adds to the machine: writable CPU RAM (RAM cheats), Game Genie patches on PRG reads, whole-machine save states |

The game project provides one function, `cyc_app_game()` (`cyc_app.h`): name, mod package id, ROM CRC32, player count and two
hooks (`on_init`, `on_frame`). CMake:

```cmake
include(${NESRECOMP_ROOT}/runner/cyc/project.cmake)
include(${NESRECOMP_ROOT}/runner/cyc/app/app.cmake)
nesrecomp_add_cycle_game(MyGame ROM ... SEED_FILE ... HEADLESS)   # HEADLESS: cyc_app.c replaces cyc_sdl.c
nesrecomp_cyc_app(MyGame)
target_sources(MyGame PRIVATE game.c)
set(RECOMP_UI_ENABLE_MODS ON CACHE BOOL "" FORCE)
include(${RECOMP_UI_ROOT}/recomp_ui.cmake)
recomp_target_launcher_ui(MyGame CONSOLE nes)
recomp_target_runtime_ui_sdlrenderer2(MyGame)
```

## Keys
Pad: keybinds.ini (created on first run). Esc: menu (pauses). F1-F12: load slot, Shift+F1-F12: save slot
(`savestates/slotNN.sav`, next to the exe). Tab (hold): fast forward. Ctrl+F12: screenshot. Alt+Enter: fullscreen.

## Cheats
* RAM: `cyc_ram_ptr()[addr] = value` before each frame (from `on_frame`).
* Game Genie: `cyc_gg_add(addr, value, compare)` from `on_init`. The patch is applied where the CPU reads PRG ROM, only if the
  byte read equals `compare` (when given). While any patch is installed the recompiled code is bypassed (it has ROM bytes
  folded in as constants) and the machine runs on the interpreter; both are cycle-for-cycle the same machine.

## Save states
`cyc_state_save/load` copy CPU, RAM, PPU, APU, cartridge registers and CHR RAM at a frame boundary. A state made with one
build loads only into a build of the same ROM. Verified: after a load, `--hash-out` (memory, CPU registers, cycle count and
hardware internals) is identical to the original run frame for frame.

## Native coverage
Code the seed file does not list runs on the interpreter (correct, slower). To grow the seed list from real play, create an
empty file `miss.on` next to the exe: on exit `cycle_seeds_played.txt` is written (merged across sessions); append its lines to
the game's seed file and rebuild.

## Test hooks (environment)
`CYC_SAVE_AT=<frame>:<file>` / `CYC_LOAD_AT=<frame>:<file>` (scripted runs), `CYC_TEST_MENU_AT=<frame>` (open the menu),
`CYC_TEST_QUIT_AT=<frame>` (exit cleanly).
