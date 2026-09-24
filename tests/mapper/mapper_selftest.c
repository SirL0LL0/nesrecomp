#include "mapper.h"

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

uint64_t g_frame_count = 0;
const char *g_last_recomp_func = NULL;
uint8_t g_chr_ram[0x2000];
uint8_t g_ppuctrl = 0;
uint8_t g_ppumask = 0x18;
static int s_test_line = 100;
int runtime_frame_scanline(void) { return s_test_line; }

static void test_uxrom_prg_banking(void) {
    uint8_t prg[8 * 0x4000] = {0};

    for (int bank = 0; bank < 8; bank++) {
        prg[bank * 0x4000 + 0x1000] = (uint8_t)bank;
    }

    mapper_init(prg, 8, 2, 0);
    assert(mapper_peek_prg(0x9000) == 0);
    assert(mapper_peek_prg(0xD000) == 7);

    mapper_write(0xC005, 5);
    assert(mapper_peek_prg(0x9000) == 5);
    assert(mapper_peek_prg(0xD000) == 7);

    mapper_write(0xFFFF, 0xFF);
    assert(mapper_peek_prg(0x9000) == 7);
    assert(mapper_peek_prg(0xD000) == 7);

    mapper_write(0x7FFF, 3);
    assert(mapper_peek_prg(0x9000) == 7);
}

static void test_mapper40_prg_and_irq(void) {
    uint8_t prg[4 * 0x4000] = {0};
    for (int bank8 = 0; bank8 < 8; bank8++) {
        prg[bank8 * 0x2000 + 0x0123] = (uint8_t)(0xA0 + bank8);
    }

    mapper_init(prg, 4, 40, 0);
    assert(mapper_peek_prg(0x6123) == 0xA6);
    assert(mapper_peek_prg(0x8123) == 0xA4);
    assert(mapper_peek_prg(0xA123) == 0xA5);
    assert(mapper_peek_prg(0xC123) == 0xA0);
    assert(mapper_peek_prg(0xE123) == 0xA7);

    mapper_write(0xE000, 3);
    assert(mapper_peek_prg(0xC123) == 0xA3);

    mapper_write(0xA000, 0);
    mapper_clock_cpu(4095);
    assert(!mapper_irq_asserted());
    mapper_clock_cpu(1);
    assert(mapper_irq_asserted());
    mapper_clock_cpu(4096);
    assert(mapper_irq_asserted());

    MapperState saved;
    mapper_get_state(&saved);
    mapper_write(0x8000, 0);
    assert(!mapper_irq_asserted());
    mapper_write(0xE000, 1);
    assert(mapper_peek_prg(0xC123) == 0xA1);
    mapper_set_state(&saved);
    assert(mapper_irq_asserted());
    assert(mapper_peek_prg(0xC123) == 0xA3);

    mapper_write(0x8000, 0);
    assert(!mapper_irq_asserted());
}

extern int g_current_bank;
static int g_current_bank_probe(void) { return g_current_bank; }
static void test_mmc5_via_mapper_api(void) {
    static uint8_t prg[64 * 0x2000];
    static uint8_t chr[256 * 1024];
    for (int u = 0; u < 64; u++) prg[u * 0x2000 + 0x10] = (uint8_t)u;
    for (int p = 0; p < 256; p++) chr[p * 1024] = (uint8_t)p;
    mapper_set_wram_size(0x2000);
    mapper_init(prg, 32, 5, 1);
    mapper_init_chr(chr, 32);
    uint8_t v = 0;

    assert(mapper_peek_prg(0xE010) == 63);                 /* power-on last bank */
    assert(mapper_write_ext(0x5100, 3) && mapper_write_ext(0x5101, 3));
    assert(mapper_write_ext(0x5114, 0x80 | 5) && mapper_write_ext(0x5115, 0x80 | 6));
    assert(mapper_write_ext(0x5116, 0x80 | 7) && mapper_write_ext(0x5117, 0xFF));
    assert(mapper_peek_prg(0x8010) == 5 && mapper_peek_prg(0xA010) == 6);
    assert(mapper_get_switchable_bank()[0x2010] == 6);     /* legacy 16KB view */
    assert(mapper_get_fixed_bank()[0x0010] == 7);
    assert(g_mmc5_win_bank8k[0] == 5 && g_current_bank_probe() == 2);

    /* WRAM in a ROM window, visible through both write paths */
    assert(mapper_write_ext(0x5102, 2) && mapper_write_ext(0x5103, 1));
    assert(mapper_write_ext(0x5115, 0x00));
    mapper_write(0xA020, 0x42);
    assert(mapper_read_ext(0xA020, &v) && v == 0x42);
    assert(mapper_get_switchable_bank()[0x2020] == 0x42);  /* dirty flag rebuilt */

    /* CHR: 1KB pages land in g_chr_ram; 8x16 splits BG/sprite sets */
    for (int i = 0; i < 8; i++) mapper_write_ext(0x5120 + i, 10 + i);
    assert(g_chr_ram[0] == 10 && g_chr_ram[7 * 0x400] == 17);
    for (int i = 0; i < 4; i++) mapper_write_ext(0x5128 + i, 200 + i);
    assert(mapper_bg_chr() == g_chr_ram);                  /* 8x8: single view */
    g_ppuctrl = 0x20; mapper_ppuctrl_changed();
    assert(mapper_bg_chr() != g_chr_ram && mapper_bg_chr()[0] == 200);
    assert(g_chr_ram[0] == 10);
    g_ppuctrl = 0;

    /* multiplier + IRQ through the generic entry points */
    mapper_write_ext(0x5205, 12); mapper_write_ext(0x5206, 34);
    assert(mapper_read_ext(0x5205, &v) && v == (12 * 34 & 0xFF));
    assert(mapper_read_ext(0x5206, &v) && v == (12 * 34 >> 8));
    /* $5204 bit 6 is derived from time in frame (visible lines = 21..260) */
    s_test_line = 5;   assert(mapper_read_ext(0x5204, &v) && !(v & 0x40));
    s_test_line = 100; assert(mapper_read_ext(0x5204, &v) && (v & 0x40));
    g_ppumask = 0;     assert(mapper_read_ext(0x5204, &v) && !(v & 0x40));
    g_ppumask = 0x18;
    mapper_write_ext(0x5203, 100); mapper_write_ext(0x5204, 0x80);
    int fired_at = -2;
    for (int k = 0; k < 241; k++) if (mapper_clock_scanline()) fired_at = k - 1;
    assert(fired_at == 100);
    assert(mapper_get_mirroring() == 0);                   /* $5105 not set yet: nt_map 0 */
    mapper_write_ext(0x5105, 0x44);
    assert(mapper_get_mirroring() == 2);
    assert(!mapper_read_ext(0x5FF0 - 0x1000, &v));         /* below $5000: not ours */
}
int main(void) {
    test_uxrom_prg_banking();
    test_mapper40_prg_and_irq();
    test_mmc5_via_mapper_api();
    puts("mapper self-test passed");
    return 0;
}
