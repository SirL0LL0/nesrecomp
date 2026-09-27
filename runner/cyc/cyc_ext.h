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

uint8_t *cyc_ram_ptr(void);   /* the 2KB of CPU RAM ($0000-$07FF), writable */

/* A generic bus write: the mapper routes it exactly like a CPU write (cartridge WRAM at $6000-$7FFF,
 * PRG-RAM, mapper registers...). Safe to call between frames (it does not touch the clock). */
void cyc_bus_write(uint16_t addr, uint8_t value);

bool cyc_state_save(const char *path);
bool cyc_state_load(const char *path);

#ifdef __cplusplus
}
#endif
