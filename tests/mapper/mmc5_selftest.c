#include "mmc5.h"

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
    mmc5_init(&m, prg, sizeof prg, chr, sizeof chr, 0x2000);
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
        if (mmc5_clock_scanline(&m)) fired_line = k - 1;   /* k=0 is pre-render (-1) */
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
    puts("mmc5_selftest: all tests passed");
    return 0;
}
