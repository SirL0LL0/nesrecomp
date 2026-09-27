/*
 * cyc_app.h - game-facing side of the cycle-backend application layer (cyc_app.c).
 *
 * cyc_app.c provides main(): the recomp-ui launcher (ROM check, settings, mods), the mod runtime, the SDL host with
 * audio and gamepads, the in-game menu (recomp-ui runtime UI), F1-F12 save-state slots, and cheats via cyc_ext.h.
 * The game project supplies only this description.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;           /* launcher and window title */
    const char *game_id;        /* mod package target id */
    const char *rom_crc32;      /* 8 lowercase hex digits: CRC32 of the ROM after its 16-byte iNES header */
    uint32_t    expected_crc;   /* the same value, for the launcher's ROM check (0: do not check) */
    int         players;        /* 1 or 2 */
    void      (*on_init)(void);             /* machine loaded and powered on, before the first frame (may be NULL) */
    void      (*on_frame)(uint64_t frame);  /* before each frame runs (may be NULL) */
} CycAppGame;

/* Implemented by the game project. */
const CycAppGame *cyc_app_game(void);

#ifdef __cplusplus
}
#endif
