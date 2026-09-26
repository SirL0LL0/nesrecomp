#include "mmc5.h"

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRG_UNITS 64                    /* 512KB, like Just Breed */
#define CHR_KB 256

static uint8_t prg[PRG_UNITS * 0x2000];
static uint8_t chr[CHR_KB * 1024];
static Mmc5 m;

static void w(uint16_t a, uint8_t v) { assert(mmc5_reg_write(&m, a, v)); }
static uint8_t r(uint16_t a) { uint8_t v = 0xEE; assert(mmc5_reg_read(&m, a, &v)); return v; }

static void setup(void) {
    for (int u = 0; u < PRG_UNITS; u++) {
        prg[u * 0x2000] = (uint8_t)u;                 /* unit id at window start */
        prg[u * 0x2000 + 0x1FFF] = (uint8_t)(0x80 + u);
    }
    for (int p = 0; p < CHR_KB; p++) chr[p * 1024] = (uint8_t)p;
    static uint8_t wram_buf[0x2000];
    memset(wram_buf, 0, sizeof wram_buf);
    mmc5_init(&m, prg, sizeof prg, chr, sizeof chr, 0x2000, wram_buf);
}

static void test_power_on(void) {
    setup();
    assert(mmc5_cpu_read(&m, 0xE000) == 63);          /* last unit at $E000 */
    assert(mmc5_cpu_read(&m, 0xFFFF) == 0x80 + 63);
}

static void test_prg_mode3(void) {
    setup();
    w(0x5100, 3);
    w(0x5114, 0x80 | 7);  w(0x5115, 0x80 | 9);
    w(0x5116, 0xFD);      w(0x5117, 0xFF);
    assert(mmc5_cpu_read(&m, 0x8000) == 7);
    assert(mmc5_cpu_read(&m, 0xA000) == 9);
    assert(mmc5_cpu_read(&m, 0xC000) == 61);          /* 0xFD & 0x7F = 125 -> 125 % 64 */
    assert(mmc5_cpu_read(&m, 0xE000) == 63);
    assert(mmc5_window_bank8k(&m, 0) == 7);
    w(0x5116, 0xFE);
    assert(mmc5_cpu_read(&m, 0xC000) == 62);
}

static void test_prg_modes_0_1_2(void) {
    setup();
    w(0x5100, 0); w(0x5117, 0x80 | 8);                /* 32KB at units 8..11 */
    for (int i = 0; i < 4; i++) assert(mmc5_cpu_read(&m, 0x8000 + i * 0x2000) == 8 + i);
    w(0x5100, 1); w(0x5115, 0x80 | 20); w(0x5117, 0x80 | 30);
    assert(mmc5_cpu_read(&m, 0x8000) == 20 && mmc5_cpu_read(&m, 0xA000) == 21);
    assert(mmc5_cpu_read(&m, 0xC000) == 30 && mmc5_cpu_read(&m, 0xE000) == 31);
    w(0x5100, 2); w(0x5115, 0x80 | 40); w(0x5116, 0x80 | 5); w(0x5117, 0x80 | 6);
    assert(mmc5_cpu_read(&m, 0x8000) == 40 && mmc5_cpu_read(&m, 0xA000) == 41);
    assert(mmc5_cpu_read(&m, 0xC000) == 5 && mmc5_cpu_read(&m, 0xE000) == 6);
}

static void test_wram_and_protect(void) {
    setup();
    w(0x5100, 3);
    w(0x5113, 0);
    mmc5_cpu_write(&m, 0x6000, 0x55);                 /* protected: ignored */
    assert(mmc5_cpu_read(&m, 0x6000) == 0);
    w(0x5102, 2); w(0x5103, 1);
    mmc5_cpu_write(&m, 0x6000, 0x55);
    assert(mmc5_cpu_read(&m, 0x6000) == 0x55);
    /* RAM mapped into $A000 window (bit 7 clear), as Just Breed does */
    w(0x5115, 0x00);
    assert(mmc5_window_bank8k(&m, 1) == -1);
    assert(mmc5_cpu_read(&m, 0xA000) == 0x55);        /* same 8KB WRAM bank 0 */
    mmc5_cpu_write(&m, 0xA010, 0x77);
    assert(mmc5_cpu_read(&m, 0x6010) == 0x77);
    /* ROM windows are not writable */
    mmc5_cpu_write(&m, 0xE000, 0x99);
    assert(mmc5_cpu_read(&m, 0xE000) == 63);
}

static void test_wram_bank_mirroring_8k(void) {
    setup();                                          /* wram_size = 8KB */
    w(0x5102, 2); w(0x5103, 1);
    w(0x5113, 0); mmc5_cpu_write(&m, 0x6000, 0x11);
    w(0x5113, 3);                                     /* 8KB chip: bank mirrors */
    assert(mmc5_cpu_read(&m, 0x6000) == 0x11);
}

static void test_chr_modes(void) {
    setup();
    w(0x5101, 3);                                     /* 1KB pages */
    for (int i = 0; i < 8; i++) w(0x5120 + i, 10 + i);
    assert(mmc5_chr_page(&m, 0, 0, 0)[0] == 10);
    assert(mmc5_chr_page(&m, 7, 0, 0)[0] == 17);
    w(0x5130, 1);                                     /* upper bits: +256 pages */
    assert(mmc5_chr_page(&m, 0, 0, 0)[0] == (uint8_t)((10 + 256) % 256));
    w(0x5130, 0);
    w(0x5101, 2);                                     /* 2KB pages: regs 1,3,5,7 */
    w(0x5121, 3); w(0x5127, 5);
    assert(mmc5_chr_page(&m, 0, 0, 0)[0] == 6 && mmc5_chr_page(&m, 1, 0, 0)[0] == 7);
    assert(mmc5_chr_page(&m, 6, 0, 0)[0] == 10 && mmc5_chr_page(&m, 7, 0, 0)[0] == 11);
    w(0x5101, 1);                                     /* 4KB pages: regs 3,7 */
    w(0x5123, 2); w(0x5127, 9);
    assert(mmc5_chr_page(&m, 3, 0, 0)[0] == 11);
    assert(mmc5_chr_page(&m, 4, 0, 0)[0] == 36);
    w(0x5101, 0); w(0x5127, 4);                       /* 8KB */
    assert(mmc5_chr_page(&m, 5, 0, 0)[0] == 37);
}

static void test_chr_sets_8x16(void) {
    setup();
    w(0x5101, 3);
    for (int i = 0; i < 8; i++) w(0x5120 + i, 100 + i);   /* set A */
    for (int i = 0; i < 4; i++) w(0x5128 + i, 200 + i);   /* set B (last written) */
    /* 8x16: sprites use A, BG uses B, B mirrored in upper half */
    assert(mmc5_chr_page(&m, 2, 0, 1)[0] == 102);
    assert(mmc5_chr_page(&m, 2, 1, 1)[0] == 202);
    assert(mmc5_chr_page(&m, 6, 1, 1)[0] == 202);
    /* 8x8: last written set (B) is used for everything */
    assert(mmc5_chr_page(&m, 1, 0, 0)[0] == 201);
    w(0x5120, 50);                                        /* A written last */
    assert(mmc5_chr_page(&m, 0, 1, 0)[0] == 50);
}

static void test_multiplier_and_exram(void) {
    setup();
    w(0x5205, 200); w(0x5206, 100);
    assert(r(0x5205) == (uint8_t)(20000 & 0xFF) && r(0x5206) == (uint8_t)(20000 >> 8));
    w(0x5104, 2);
    w(0x5C10, 0xAB);
    assert(r(0x5C10) == 0xAB);
    w(0x5104, 3);
    w(0x5C10, 0xCD);                                       /* read-only mode */
    assert(r(0x5C10) == 0xAB);
    w(0x5104, 0);
    assert(r(0x5C10) == 0);                                /* not readable in mode 0/1 */
    w(0x5105, 0x44);
    assert(mmc5_nt_source(&m, 0) == 0 && mmc5_nt_source(&m, 1) == 1 &&
           mmc5_nt_source(&m, 2) == 0 && mmc5_nt_source(&m, 3) == 1);
    w(0x5105, 0xE4);
    assert(mmc5_nt_source(&m, 3) == 3 && mmc5_nt_source(&m, 2) == 2);
}

static int run_frame(int enable, int irq_expected_line) {
    int fired_line = -2;
    mmc5_frame_start(&m);
    for (int k = 0; k < 241; k++) {
        if (mmc5_clock_scanline(&m, 1)) fired_line = k - 1;   /* k=0 is pre-render (-1) */
    }
    (void)enable;
    assert(fired_line == irq_expected_line);
    return fired_line;
}

static void test_scanline_irq(void) {
    setup();
    w(0x5203, 220);
    w(0x5204, 0x80);
    run_frame(1, 220);
    assert(r(0x5204) & 0x80);                              /* pending until read */
    assert(!(r(0x5204) & 0x80));                           /* read acknowledges */
    run_frame(1, 220);                                     /* fires again next frame */
    w(0x5204, 0x00);
    run_frame(0, -2);                                      /* disabled: no delivery */
    assert(r(0x5204) & 0x80);                              /* but pending flag still set */
    w(0x5203, 0);
    w(0x5204, 0x80);
    (void)r(0x5204);
    run_frame(1, -2);                                      /* compare 0 never fires */
    w(0x5203, 1); run_frame(1, 1);
}


/* From the MMC5 reference: CHR regs are 10 bits, the high 2 copied from $5130 AT WRITE TIME. */
static void test_chr_high_bits_latched(void) {
    setup();
    w(0x5101, 3);                                      /* 1KB mode */
    w(0x5130, 0x00); w(0x5127, 0x20);                  /* $5127 = $020 */
    w(0x5130, 0x02); w(0x5123, 0x41);                  /* $5123 = $241; $5127 still $020, not $220 */
    assert(mmc5_chr_page(&m, 7, 0, 1)[0] == (uint8_t)0x20);   /* A slot 7 = $5127 */
    assert(mmc5_chr_page(&m, 3, 0, 1)[0] == (uint8_t)(0x241 % CHR_KB));
}

/* ExRAM: Ex2 = CPU RAM, Ex3 = read only, Ex0/1 writable only while rendering (else $00 is stored). */
static void test_exram_rules(void) {
    setup();
    w(0x5104, 2); w(0x5C00, 0x5A);  assert(r(0x5C00) == 0x5A);
    w(0x5104, 3); w(0x5C00, 0x11);  assert(r(0x5C00) == 0x5A);          /* Ex3: write ignored */
    w(0x5104, 1); m.in_frame = 0; w(0x5C01, 0x77);
    w(0x5104, 2); assert(r(0x5C01) == 0x00);                            /* vblank write into Ex1 stores 0 */
    w(0x5104, 1); m.in_frame = 1; w(0x5C01, 0x77);
    w(0x5104, 2); assert(r(0x5C01) == 0x77);                            /* while rendering it sticks */
}

static void test_state_roundtrip(void) {
    setup();
    w(0x5100, 2); w(0x5115, 0x82); w(0x5116, 0x85); w(0x5130, 1); w(0x5120, 9);
    w(0x5104, 2); w(0x5C10, 0xAB); w(0x5203, 0x30); w(0x5204, 0x80);
    uint8_t buf[2048]; int n = mmc5_state_get(&m, buf, sizeof buf);
    assert(n > 0);
    setup();                                                             /* wipe */
    assert(mmc5_state_set(&m, buf, n));
    assert(m.prg_mode == 2 && m.chr_a[0] == (9 | 0x100) && m.irq_compare == 0x30 && m.irq_enabled);
    assert(r(0x5C10) == 0xAB);
    assert(mmc5_cpu_read(&m, 0x8000) == 2 && mmc5_cpu_read(&m, 0xC000) == 5);   /* 16KB window pair 2,3 ... */
}


/* Two PRG-RAM chips (0/8/32KB): banks 0-3 belong to chip 0, 4-7 to chip 1; a bank without a chip reads as open bus. */
static uint8_t big_wram[0x10000];
static void wram_setup(int c0, int c1) {
    setup();
    memset(big_wram, 0, sizeof big_wram);
    mmc5_init(&m, prg, sizeof prg, chr, sizeof chr, 0x2000, big_wram);
    mmc5_set_wram_config(&m, c0, c1);
    w(0x5102, 2); w(0x5103, 1);
}
static void wr(int bank, uint8_t v) { w(0x5113, bank); mmc5_cpu_write(&m, 0x6000, v); }
static uint8_t rd(int bank) { w(0x5113, bank); return mmc5_cpu_read(&m, 0x6000); }

static void test_wram_chips(void) {
    wram_setup(8, 0);                                     /* EKROM: Just Breed */
    wr(0, 0x11); assert(rd(3) == 0x11);                   /* banks 0-3 mirror the one chip */
    wr(5, 0x22); assert(rd(5) == 0 && rd(4) == 0);        /* banks 4-7: nothing there, writes ignored */
    wram_setup(8, 8);                                     /* ETROM: 8+8 */
    wr(0, 0x11); wr(4, 0x44);
    assert(rd(0) == 0x11 && rd(2) == 0x11 && rd(4) == 0x44 && rd(7) == 0x44);
    wram_setup(32, 0);                                    /* EWROM: four distinct 8KB pages */
    for (int b = 0; b < 4; b++) wr(b, (uint8_t)(0x60 + b));
    for (int b = 0; b < 4; b++) assert(rd(b) == 0x60 + b);
    assert(rd(4) == 0);
    wram_setup(0, 0);                                     /* ELROM: no RAM at all (Castlevania III) */
    wr(0, 0x55); assert(rd(0) == 0);
    wram_setup(32, 8);                                    /* 32KB then 8KB: banks 4-7 -> block 4 */
    wr(1, 0x21); wr(4, 0x99);
    assert(rd(1) == 0x21 && rd(6) == 0x99 && rd(0) == 0);
    /* RAM in a PRG window follows the same map */
    w(0x5100, 3); w(0x5115, 0x04);
    assert(mmc5_window_bank8k(&m, 1) == -1 && mmc5_cpu_read(&m, 0xA000) == 0x99);
}

static void test_known_boards(void) {
    static uint8_t junk[0x2000];
    assert(mmc5_known_wram(junk, sizeof junk) == -1);
    /* CRC32 of eight zero bytes is 0x6522DF69: not a board; just checks the CRC path is stable */
    assert(mmc5_known_wram(junk, 8) == -1);
}

/* The counter counts rendered scanlines; switching rendering off resets it, drops "in frame" and keeps a pending IRQ. */
static void test_rendering_off(void) {
    setup();
    w(0x5203, 100); w(0x5204, 0x80);
    int fired = -2;
    mmc5_frame_start(&m);
    for (int k = 0; k < 241; k++) {
        if (k == 50) mmc5_rendering_disabled(&m);         /* $2001 write with BG/sprites off at line 49 */
        if (k >= 50 && k < 60) { if (mmc5_clock_scanline(&m, 0)) fired = k - 1; assert(!m.in_frame); continue; }
        if (mmc5_clock_scanline(&m, 1)) fired = k - 1;
    }
    /* counter restarts at -1 on line 60 -> compare 100 is reached 101 lines later = line 160 */
    assert(fired == 160);
    assert(!m.in_frame);                                  /* stays clear until the next frame */
    (void)r(0x5204);
    /* pending survives rendering off */
    mmc5_frame_start(&m);
    for (int k = 0; k < 130; k++) mmc5_clock_scanline(&m, 1);
    assert(m.irq_pending);
    mmc5_rendering_disabled(&m);
    assert(m.irq_pending && !m.in_frame);
    /* in frame is set from visible line 1 (k=2), pending is cleared on visible line 0 (k=1) */
    mmc5_frame_start(&m);
    mmc5_clock_scanline(&m, 1); assert(!m.in_frame);
    mmc5_clock_scanline(&m, 1); assert(!m.in_frame && !m.irq_pending);
    mmc5_clock_scanline(&m, 1); assert(m.in_frame);
}

static void test_power_on_chr_regs(void) {
    setup();
    assert(m.chr_a[0] == 0 && m.chr_a[5] == 5 && m.chr_b[3] == 3);
}

int main(void) {
    test_power_on();
    test_prg_mode3();
    test_prg_modes_0_1_2();
    test_wram_and_protect();
    test_wram_bank_mirroring_8k();
    test_chr_modes();
    test_chr_sets_8x16();
    test_multiplier_and_exram();
    test_scanline_irq();
    test_chr_high_bits_latched();
    test_exram_rules();
    test_state_roundtrip();
    test_wram_chips();
    test_known_boards();
    test_rendering_off();
    test_power_on_chr_regs();
    puts("mmc5_selftest: all tests passed");
    return 0;
}
