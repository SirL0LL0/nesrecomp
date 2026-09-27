/*
 * cyc_ext.h - what a game host adds to the cycle-accurate machine: cheats and save states.
 *
 *   RAM pokes        cyc_ram_ptr()          the 2KB of CPU RAM, writable (call between frames)
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

bool cyc_state_save(const char *path);
bool cyc_state_load(const char *path);

#ifdef __cplusplus
}
#endif
