/* cyc_ext.c - see cyc_ext.h. */
#include "cyc_ext.h"

#include "cpu6502.h"
#include "cyc_core.h"
#include "hw_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *cyc_ram_ptr(void) { return hw.ram; }

void cyc_bus_write(uint16_t addr, uint8_t value) { hw_write(addr, value); }

uint8_t cyc_bus_read(uint16_t addr) { return hw_read(addr); }

/* ---- Save states ---- */

void *hw_apu_state(size_t *size);
void hw_ppu_extra_save(uint8_t out[2]);
void hw_ppu_extra_load(const uint8_t in[2]);
extern uint16_t hw_frame_index[256 * 240];

typedef struct { void *p; size_t n; } Blob;

static size_t blobs(Blob *b, uint8_t extra[2]) {
    size_t apu_n;
    void *apu_p = hw_apu_state(&apu_n);
    size_t k = 0;
    b[k].p = &cpu;            b[k++].n = sizeof cpu;
    b[k].p = &hw;             b[k++].n = sizeof hw;
    b[k].p = &ppu;            b[k++].n = sizeof ppu;
    b[k].p = apu_p;           b[k++].n = apu_n;
    b[k].p = hw_frame_index;  b[k++].n = sizeof hw_frame_index;
    b[k].p = extra;           b[k++].n = 2;
    b[k].p = &hw_dma_stalls;  b[k++].n = sizeof hw_dma_stalls;
    /* Writable CHR (CHR RAM boards, or the RAM chip of boards that have both). */
    if (hw_cart.chr_ram) { b[k].p = hw_cart.chr; b[k++].n = hw_cart.chr_len; }
    else if (hw_cart.chr_ram_len) { b[k].p = hw_cart.chr + hw_cart.chr_ram_base; b[k++].n = hw_cart.chr_ram_len; }
    return k;
}

#define MAGIC 0x31535943u   /* "CYS1" */

bool cyc_state_save(const char *path) {
    uint8_t extra[2];
    hw_ppu_extra_save(extra);
    Blob b[16];
    size_t n = blobs(b, extra);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    uint32_t head[3] = { MAGIC, cyc_prg_hash(), (uint32_t)n };
    bool ok = fwrite(head, sizeof head, 1, f) == 1;
    for (size_t i = 0; ok && i < n; i++) {
        uint32_t len = (uint32_t)b[i].n;
        ok = fwrite(&len, 4, 1, f) == 1 && fwrite(b[i].p, 1, len, f) == len;
    }
    /* The cartridge, apart from the two buffer pointers it holds. */
    if (ok) {
        uint32_t len = (uint32_t)sizeof hw_cart;
        HwCart copy = hw_cart;
        copy.prg = NULL;
        copy.chr = NULL;
        ok = fwrite(&len, 4, 1, f) == 1 && fwrite(&copy, 1, len, f) == len;
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp); return false; }
    remove(path);
    return rename(tmp, path) == 0;
}

bool cyc_state_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t extra[2] = {0, 0};
    Blob b[16];
    size_t n = blobs(b, extra);
    uint32_t head[3];
    /* CYC_STATE_ANY_ROM=1 (diagnosi): accetta uno stato salvato con un'altra versione della stessa ROM, utile
     * quando cambiano solo i dati (es. una traduzione ricostruita) e il codice e' lo stesso. */
    bool any_rom = getenv("CYC_STATE_ANY_ROM") != NULL;
    bool ok = fread(head, sizeof head, 1, f) == 1 && head[0] == MAGIC && (any_rom || head[1] == cyc_prg_hash()) &&
              head[2] == n;
    /* Read everything first: a bad file must not leave a half-loaded machine. */
    void *tmp[16] = {0};
    for (size_t i = 0; ok && i < n; i++) {
        uint32_t len;
        ok = fread(&len, 4, 1, f) == 1 && len == b[i].n;
        if (ok) { tmp[i] = malloc(len ? len : 1); ok = tmp[i] && fread(tmp[i], 1, len, f) == len; }
    }
    HwCart *cart = (HwCart *)malloc(sizeof *cart);
    if (ok) {
        uint32_t len;
        ok = cart && fread(&len, 4, 1, f) == 1 && len == sizeof hw_cart && fread(cart, 1, len, f) == len;
    }
    fclose(f);
    if (ok) {
        for (size_t i = 0; i < n; i++) memcpy(b[i].p, tmp[i], b[i].n);
        cart->prg = hw_cart.prg;
        cart->chr = hw_cart.chr;
        memcpy(&hw_cart, cart, sizeof hw_cart);
        hw_ppu_extra_load(extra);
    }
    for (size_t i = 0; i < n; i++) free(tmp[i]);
    free(cart);
    return ok;
}
