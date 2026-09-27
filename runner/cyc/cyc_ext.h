/*
 * cyc_ext.h - what a game host adds to the cycle-accurate machine: cheats and save states.
 *
 *   RAM pokes        cyc_ram_ptr()          the 2KB of CPU RAM, writable (call between frames)
 *   Game Genie       cyc_gg_add/clear       byte patches on PRG ROM reads, with the optional compare value. While one is
 *                                           installed the recompiled code is bypassed (it has ROM bytes folded in),
 *                                           so the machine runs on the interpreter, cycle for cycle the same.
 *   Save states      cyc_state_save/load    the whole machine at a frame boundary (CPU, RAM, PPU, APU, cartridge).
 *                                           Audio filters are not part of the state.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t *cyc_ram_ptr(void);

void cyc_gg_clear(void);
/* addr $8000-$FFFF; compare < 0: none. Returns false when the table is full or addr is out of range. */
bool cyc_gg_add(uint16_t addr, uint8_t value, int compare);
int  cyc_gg_count(void);

bool cyc_state_save(const char *path);
bool cyc_state_load(const char *path);

#ifdef __cplusplus
}
#endif
