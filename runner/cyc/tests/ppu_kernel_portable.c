#define NESRECOMP_PPU_HLE_FORCE_PORTABLE 1
#include "ppu_hle_kernel.h"

unsigned portable_sprite_mask(const uint8_t x[8],const uint8_t lo[8],const uint8_t hi[8],bool skipped) {
    return ppu_hle_sprite_mask(x,lo,hi,skipped);
}
unsigned portable_sprite_shift(uint8_t x[8],uint8_t lo[8],uint8_t hi[8],bool render,bool skipped) {
    return ppu_hle_sprite_shift(x,lo,hi,render,skipped);
}
