#pragma once
/*
 * mmc5.h - Nintendo MMC5 (iNES mapper 5) core.
 *
 * Self-contained: it owns register state, WRAM, ExRAM and the multiplier, and
 * resolves banking. The rest of the runner reads PRG/CHR through these
 * functions instead of the 16KB switchable/fixed buffers the other mappers use.
 *
 * Not modelled yet: expansion audio ($5000-$5015), vertical split ($5200-$5202).
 * The registers are stored so reads/writes are harmless.
 */
#include <stdint.h>

#define MMC5_MAX_WRAM 0x10000

typedef struct Mmc5 {
    const uint8_t *prg;      /* PRG ROM, prg_size bytes */
    const uint8_t *chr;      /* CHR ROM, chr_size bytes (NULL/0 = CHR RAM) */
    uint32_t prg_size, chr_size;
    uint32_t wram_size;      /* power of two, <= MMC5_MAX_WRAM */
    uint8_t  wram[MMC5_MAX_WRAM];
    uint8_t  exram[0x400];

    uint8_t prg_mode, chr_mode;
    uint8_t ram_protect1, ram_protect2;
    uint8_t exram_mode, nt_map, fill_tile, fill_attr;
    uint8_t wram_bank;                 /* $5113 */
    uint8_t prg_reg[3];                /* $5114-$5116; $5117 in prg_last */
    uint8_t prg_last;
    uint8_t chr_a[8], chr_b[4];        /* $5120-$5127, $5128-$512B */
    uint8_t chr_upper;                 /* $5130 */
    uint8_t chr_last_set_b;            /* last CHR register group written */
    uint8_t split_ctrl, split_scroll, split_bank;
    uint8_t irq_compare, irq_enabled, irq_pending, in_frame;
    uint8_t mult_a, mult_b;
    uint8_t audio_regs[0x20];          /* $5000-$501F, stored only */

    int line_calls;                    /* mapper_clock_scanline calls this frame */
    int scanline;                      /* counter compared with irq_compare */
    int irq_fired;                     /* edge latch for the current pending */

    int win_bank8k[4];                 /* per-window ROM 8KB unit, or -1 = WRAM */
    int win_wram_off[4];               /* byte offset in wram when win_bank8k == -1 */
    int wram6000_off;
} Mmc5;

void mmc5_init(Mmc5 *m, const uint8_t *prg, uint32_t prg_size,
               const uint8_t *chr, uint32_t chr_size, uint32_t wram_size);

/* $5000-$5FFF register/ExRAM access. Returns 1 if the address was handled. */
int  mmc5_reg_read(Mmc5 *m, uint16_t addr, uint8_t *out);
int  mmc5_reg_write(Mmc5 *m, uint16_t addr, uint8_t val);

/* $6000-$FFFF CPU bus. */
uint8_t mmc5_cpu_read(const Mmc5 *m, uint16_t addr);
void    mmc5_cpu_write(Mmc5 *m, uint16_t addr, uint8_t val);

/* ROM 8KB unit behind window (0-3 = $8000/$A000/$C000/$E000), -1 if WRAM. */
int mmc5_window_bank8k(const Mmc5 *m, int win);

/* Pointer to the 1KB CHR-ROM page for pattern slot 0-7 ($0000-$1FFF in 1KB
 * steps). bg selects the BG register set (set B); sprite16 tells whether the
 * PPU is in 8x16 sprite mode (only then are the two sets used separately). */
const uint8_t *mmc5_chr_page(const Mmc5 *m, int slot1k, int bg, int sprite16);

/* Nametable source for NT 0-3: 0=CIRAM0 1=CIRAM1 2=ExRAM 3=fill. */
int mmc5_nt_source(const Mmc5 *m, int nt);

/* One call per PPU line: pre-render first, then visible lines 0-239.
 * Returns 1 when a new IRQ should be delivered to the CPU. */
int  mmc5_clock_scanline(Mmc5 *m);
/* Call at the start of every frame (also fine to omit; wraps after 241). */
void mmc5_frame_start(Mmc5 *m);
