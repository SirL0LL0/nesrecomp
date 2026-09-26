#include "mmc5.h"
#include <string.h>

static void mmc5_remap(Mmc5 *m);

void mmc5_init(Mmc5 *m, const uint8_t *prg, uint32_t prg_size,
               const uint8_t *chr, uint32_t chr_size, uint32_t wram_size, uint8_t *wram_buf) {
    memset(m, 0, sizeof(*m));
    m->wram = wram_buf;
    m->prg = prg; m->prg_size = prg_size;
    m->chr = chr; m->chr_size = chr_size;
    if (wram_size == 0 || wram_size > MMC5_MAX_WRAM) wram_size = 0x2000;
    while (wram_size & (wram_size - 1)) wram_size &= wram_size - 1;
    m->wram_size = wram_size;
    /* default chip layout from the total size; mmc5_set_wram_config() refines it (two chips, 0/8/32KB each) */
    mmc5_set_wram_config(m, wram_size >= 0x8000 ? 32 : 8, wram_size >= 0x10000 ? 32 : (wram_size == 0x4000 ? 8 : 0));
    m->prg_mode = 3;
    m->prg_last = 0xFF;          /* power-on: last bank at $E000 */
    m->prg_reg[2] = 0xFF;
    m->chr_mode = 3;
    for (int i = 0; i < 8; i++) m->chr_a[i] = (uint16_t)i;      /* power-on: identity CHR registers (nintendulator) */
    for (int i = 0; i < 4; i++) m->chr_b[i] = (uint16_t)i;
    m->exram_mode = 0;
    mmc5_remap(m);
}

static int rom_units(const Mmc5 *m) { return (int)(m->prg_size >> 13); }

/* The board has up to two PRG-RAM chips (0, 8 or 32KB each, e.g. ETROM = 8+8, EWROM = 32+0). The 3-bit RAM bank number
 * of $5113-$5117 selects an 8KB block: banks 0-3 belong to chip 0, banks 4-7 to chip 1; a bank with no chip behind it
 * reads as open bus and ignores writes. The buffer holds chip 0 followed by chip 1. */
void mmc5_set_wram_config(Mmc5 *m, int chip0_kb, int chip1_kb) {
    int u0 = chip0_kb / 8, u1 = chip1_kb / 8;
    for (int b = 0; b < 8; b++) {
        if (b < 4) m->wram_map[b] = (int8_t)(u0 ? (u0 == 1 ? 0 : b) : -1);
        else       m->wram_map[b] = (int8_t)(u1 ? u0 + (u1 == 1 ? 0 : b - 4) : -1);
    }
    uint32_t total = (uint32_t)(u0 + u1) * 0x2000u;
    m->wram_size = total ? total : 0x2000u;
    m->wram_chips_kb[0] = (uint8_t)chip0_kb; m->wram_chips_kb[1] = (uint8_t)chip1_kb;
    mmc5_remap(m);
}

/* PRG-RAM layout of the known MMC5 boards, keyed by the CRC32 of the PRG ROM (table from nintendulator-mappers). */
int mmc5_known_wram(const uint8_t *prg, uint32_t prg_size) {
    static const struct { uint32_t crc; uint8_t c0, c1; } T[] = {
        {0x95CA9EC7, 0, 0}, {0x51D2112F, 0, 0}, {0x255B129C, 0, 0}, {0xCD9ACF43, 0, 0}, {0xD979C8B7, 0, 0},        /* ELROM: CV3, Laser Invasion, ... */
        {0xE7C72DBB, 8, 0}, {0x57F33F70, 8, 0}, {0x5D9D9891, 8, 0}, {0xE91548D8, 8, 0},                              /* EKROM: Gemfire, Royal Blood, Just Breed (J) */
        {0x2B548D75, 8, 8}, {0xE6C28C5F, 8, 8}, {0x2F50BD38, 8, 8}, {0x57E3218B, 8, 8}, {0xB56958D1, 8, 8},         /* ETROM */
        {0x98C8E090, 8, 8}, {0xCD35E2E9, 8, 8}, {0xF4CD4998, 8, 8}, {0x8FA95456, 8, 8},                              /* (Uncharted Waters = CD35E2E9) */
        {0x11EAAD26, 32, 0}, {0x286613D8, 32, 0}, {0x95BA5733, 32, 0}, {0xF4120E58, 32, 0},                          /* EWROM */
    };
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < prg_size; i++) {
        crc ^= prg[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    }
    crc = ~crc;
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++)
        if (T[i].crc == crc) return (T[i].c0 << 8) | T[i].c1;
    return -1;
}

/* byte offset in the WRAM buffer of the 8KB block behind RAM bank `bank`, or -1 (open bus) */
static int wram_block(const Mmc5 *m, int bank) {
    int u = m->wram_map[bank & 7];
    return u < 0 ? -1 : (int)((uint32_t)u << 13);
}

/* Resolve one window: reg = register value, span8 = window size in 8KB units,
 * idx = 8KB offset of this window inside that span, is_rom_forced for $5117. */
static void resolve(Mmc5 *m, int win, uint8_t reg, int span8, int idx, int forced_rom) {
    int rom = forced_rom || (reg & 0x80);
    if (rom) {
        int unit = ((reg & 0x7F) & ~(span8 - 1)) + idx;
        int n = rom_units(m);
        m->win_bank8k[win] = n ? unit % n : 0;
    } else {
        int unit = ((reg & 0x07) & ~(span8 - 1)) + idx;
        m->win_bank8k[win] = -1;
        m->win_wram_off[win] = wram_block(m, unit);       /* -1 = no RAM chip behind this bank (open bus) */
    }
}

static void mmc5_remap(Mmc5 *m) {
    m->wram6000_off = wram_block(m, m->wram_bank);
    switch (m->prg_mode & 3) {
    case 0:
        for (int i = 0; i < 4; i++) resolve(m, i, m->prg_last, 4, i, 1);
        break;
    case 1:
        resolve(m, 0, m->prg_reg[1], 2, 0, 0); resolve(m, 1, m->prg_reg[1], 2, 1, 0);
        resolve(m, 2, m->prg_last, 2, 0, 1);   resolve(m, 3, m->prg_last, 2, 1, 1);
        break;
    case 2:
        resolve(m, 0, m->prg_reg[1], 2, 0, 0); resolve(m, 1, m->prg_reg[1], 2, 1, 0);
        resolve(m, 2, m->prg_reg[2], 1, 0, 0);
        resolve(m, 3, m->prg_last, 1, 0, 1);
        break;
    default:
        resolve(m, 0, m->prg_reg[0], 1, 0, 0);
        resolve(m, 1, m->prg_reg[1], 1, 0, 0);
        resolve(m, 2, m->prg_reg[2], 1, 0, 0);
        resolve(m, 3, m->prg_last, 1, 0, 1);
        break;
    }
}

int mmc5_window_bank8k(const Mmc5 *m, int win) { return m->win_bank8k[win & 3]; }

static int wram_writable(const Mmc5 *m) {
    return m->ram_protect1 == 0x02 && m->ram_protect2 == 0x01;
}

static uint8_t mmc5_cpu_read_raw(const Mmc5 *m, uint16_t addr) {
    if (addr < 0x6000) return 0;
    if (addr < 0x8000)
        return m->wram6000_off < 0 ? 0 : m->wram[(uint32_t)m->wram6000_off + (addr & 0x1FFF)];
    int win = (addr - 0x8000) >> 13;
    int off = addr & 0x1FFF;
    int b = m->win_bank8k[win];
    if (b >= 0) return m->prg[((uint32_t)b << 13) + off];
    return m->win_wram_off[win] < 0 ? 0 : m->wram[(uint32_t)m->win_wram_off[win] + off];
}

uint8_t mmc5_cpu_read(const Mmc5 *m, uint16_t addr) {
    uint8_t b = mmc5_cpu_read_raw(m, addr);
    if (m->gg_n && addr >= 0x8000 && m->gg_flag[addr - 0x8000])
        for (int i = 0; i < m->gg_n; i++)
            if (m->gg_addr[i] == addr && (m->gg_cmp[i] < 0 || m->gg_cmp[i] == b)) return m->gg_val[i];
    return b;
}

void mmc5_gg_clear(Mmc5 *m) { m->gg_n = 0; memset(m->gg_flag, 0, sizeof m->gg_flag); }

int mmc5_gg_add(Mmc5 *m, uint16_t addr, uint8_t val, int cmp) {
    if (m->gg_n >= MMC5_MAX_GG || addr < 0x8000) return 0;
    m->gg_addr[m->gg_n] = addr; m->gg_val[m->gg_n] = val; m->gg_cmp[m->gg_n] = (int16_t)cmp;
    m->gg_flag[addr - 0x8000] = 1;
    m->gg_n++;
    return 1;
}

int mmc5_gg_overlaps(const Mmc5 *m, uint16_t start, uint32_t nbytes) {
    for (int i = 0; i < m->gg_n; i++)
        if (m->gg_addr[i] >= start && (uint32_t)m->gg_addr[i] < (uint32_t)start + nbytes) return 1;
    return 0;
}

void mmc5_cpu_write(Mmc5 *m, uint16_t addr, uint8_t val) {
    if (!wram_writable(m) || addr < 0x6000) return;
    if (addr < 0x8000) {
        if (m->wram6000_off >= 0) m->wram[(uint32_t)m->wram6000_off + (addr & 0x1FFF)] = val;
        return;
    }
    int win = (addr - 0x8000) >> 13;
    if (m->win_bank8k[win] >= 0) return;          /* ROM: ignore */
    if (m->win_wram_off[win] >= 0) m->wram[(uint32_t)m->win_wram_off[win] + (addr & 0x1FFF)] = val;
}

int mmc5_reg_write(Mmc5 *m, uint16_t addr, uint8_t val) {
    if (addr < 0x5000 || addr > 0x5FFF) return 0;
    if (addr <= 0x501F) { m->audio_regs[addr - 0x5000] = val; return 1; }
    if (addr >= 0x5C00) {
        if (m->exram_mode == 2) m->exram[addr - 0x5C00] = val;                                /* Ex2: CPU RAM */
        else if (m->exram_mode < 2) m->exram[addr - 0x5C00] = m->in_frame ? val : 0;         /* Ex0/Ex1: only while rendering */
        return 1;                                                                            /* Ex3: read-only */
    }
    switch (addr) {
    case 0x5100: m->prg_mode = val & 3; mmc5_remap(m); break;
    case 0x5101: m->chr_mode = val & 3; break;
    case 0x5102: m->ram_protect1 = val & 3; break;
    case 0x5103: m->ram_protect2 = val & 3; break;
    case 0x5104: m->exram_mode = val & 3; break;
    case 0x5105: m->nt_map = val; break;
    case 0x5106: m->fill_tile = val; break;
    case 0x5107: m->fill_attr = val & 3; break;
    case 0x5113: m->wram_bank = val & 7; mmc5_remap(m); break;
    case 0x5114: case 0x5115: case 0x5116:
        m->prg_reg[addr - 0x5114] = val; mmc5_remap(m); break;
    case 0x5117: m->prg_last = val; mmc5_remap(m); break;
    case 0x5130: m->chr_upper = val & 3; break;
    case 0x5200: m->split_ctrl = val; break;
    case 0x5201: m->split_scroll = val; break;
    case 0x5202: m->split_bank = val; break;
    case 0x5203: m->irq_compare = val; break;
    case 0x5204: m->irq_enabled = (val & 0x80) != 0; break;
    case 0x5205: m->mult_a = val; break;
    case 0x5206: m->mult_b = val; break;
    default:
        if (addr >= 0x5120 && addr <= 0x5127) { m->chr_a[addr - 0x5120] = (uint16_t)(val | ((m->chr_upper & 3) << 8)); m->chr_last_set_b = 0; }
        else if (addr >= 0x5128 && addr <= 0x512B) { m->chr_b[addr - 0x5128] = (uint16_t)(val | ((m->chr_upper & 3) << 8)); m->chr_last_set_b = 1; }
        break;
    }
    return 1;
}

int mmc5_reg_read(Mmc5 *m, uint16_t addr, uint8_t *out) {
    if (addr < 0x5000 || addr > 0x5FFF) return 0;
    if (addr == 0x5015) { *out = 0; return 1; }   /* audio length status: not modelled */
    if (addr >= 0x5C00) {
        *out = (m->exram_mode >= 2) ? m->exram[addr - 0x5C00] : 0;
        return 1;
    }
    if (addr == 0x5204) {
        *out = (uint8_t)((m->irq_pending ? 0x80 : 0) | (m->in_frame ? 0x40 : 0));
        m->irq_pending = 0;
        m->irq_fired = 0;
        return 1;
    }
    if (addr == 0x5205) { *out = (uint8_t)((m->mult_a * m->mult_b) & 0xFF); return 1; }
    if (addr == 0x5206) { *out = (uint8_t)((m->mult_a * m->mult_b) >> 8); return 1; }
    return 0;                                      /* open bus */
}

int mmc5_nt_source(const Mmc5 *m, int nt) { return (m->nt_map >> ((nt & 3) * 2)) & 3; }

const uint8_t *mmc5_chr_page(const Mmc5 *m, int slot1k, int bg, int sprite16) {
    if (!m->chr || m->chr_size == 0) return 0;
    int use_b = sprite16 ? bg : m->chr_last_set_b;
    int page;                                       /* 1KB unit */
    int mode = m->chr_mode & 3;
    slot1k &= 7;
    if (!use_b) {
        int reg;
        switch (mode) {
        case 0:  reg = 7; break;
        case 1:  reg = (slot1k < 4) ? 3 : 7; break;
        case 2:  reg = (slot1k | 1); break;
        default: reg = slot1k; break;
        }
        int val = m->chr_a[reg];
        int span = 1 << (3 - mode);                 /* 8,4,2,1 KB pages */
        page = (val * span) + (slot1k & (span - 1));
    } else {
        int s = slot1k & 3;
        int reg;
        switch (mode) {
        case 0: case 1: reg = 3; break;
        case 2:         reg = (s | 1); break;
        default:        reg = s; break;
        }
        int val = m->chr_b[reg];
        int span = 1 << (3 - mode);
        int within = (mode == 0) ? (slot1k & 7) : (slot1k & (span - 1));
        page = (val * span) + within;
    }
    uint32_t units = m->chr_size >> 10;
    page = (int)((uint32_t)page % units);
    return m->chr + ((uint32_t)page << 10);
}

void mmc5_frame_start(Mmc5 *m) {
    m->line_calls = 0;
    m->in_frame = 0;
    m->ctr_base = 0;
    m->off_this_frame = 0;
}

/* Scanline counter as on the chip (nintendulator's model): it counts the scanlines that were rendered, starting at -1 on
 * the pre-render line, so on visible line n it equals n. Turning rendering off (a $2001 write with BG and sprites off, or
 * a line drawn with rendering off) resets it to -2 and drops "in frame" for the rest of the frame; a pending IRQ survives.
 * The pending flag is cleared on visible line 0, "in frame" is set from visible line 1, compare value 0 never fires. */
int mmc5_clock_scanline(Mmc5 *m, int rendering) {
    if (m->line_calls >= 241) mmc5_frame_start(m);
    int k = m->line_calls++;
    if (!rendering) {
        m->in_frame = 0;
        m->ctr_base = m->line_calls;                /* next rendered line counts -1 */
        if (k > 0) m->off_this_frame = 1;
        return 0;
    }
    if (k == 0) { m->ctr_base = 0; m->off_this_frame = 0; m->in_frame = 0; }
    int ctr = k - m->ctr_base - 1;
    m->scanline = ctr;
    if (k == 1) { m->irq_pending = 0; m->irq_fired = 0; }
    m->in_frame = (k >= 2 && !m->off_this_frame);
    if (m->irq_compare != 0 && ctr == m->irq_compare)
        m->irq_pending = 1;
    if (m->irq_pending && m->irq_enabled && !m->irq_fired) {
        m->irq_fired = 1;
        return 1;
    }
    return 0;
}

void mmc5_rendering_disabled(Mmc5 *m) {
    m->in_frame = 0;
    m->ctr_base = m->line_calls;                    /* the next rendered line counts -1 */
    if (m->line_calls > 0 && m->line_calls < 241) m->off_this_frame = 1;
}

/* ---- savestate ------------------------------------------------------------------------------------------ */
#define MMC5_STATE_VER 1
int mmc5_state_get(const Mmc5 *m, uint8_t *buf, int cap) {
    int n = 0;
#define PUT8(v)  do { if (n >= cap) return -1; buf[n++] = (uint8_t)(v); } while (0)
#define PUT16(v) do { PUT8((v) & 0xFF); PUT8(((v) >> 8) & 0xFF); } while (0)
    PUT8(MMC5_STATE_VER);
    PUT8(m->prg_mode); PUT8(m->chr_mode); PUT8(m->ram_protect1); PUT8(m->ram_protect2);
    PUT8(m->exram_mode); PUT8(m->nt_map); PUT8(m->fill_tile); PUT8(m->fill_attr); PUT8(m->wram_bank);
    for (int i = 0; i < 3; i++) PUT8(m->prg_reg[i]);
    PUT8(m->prg_last);
    for (int i = 0; i < 8; i++) PUT16(m->chr_a[i]);
    for (int i = 0; i < 4; i++) PUT16(m->chr_b[i]);
    PUT8(m->chr_upper); PUT8(m->chr_last_set_b);
    PUT8(m->split_ctrl); PUT8(m->split_scroll); PUT8(m->split_bank);
    PUT8(m->irq_compare); PUT8(m->irq_enabled); PUT8(m->irq_pending); PUT8(m->in_frame);
    PUT8(m->mult_a); PUT8(m->mult_b);
    PUT16(m->line_calls); PUT16(m->scanline); PUT8(m->irq_fired);
    for (int i = 0; i < 0x20; i++) PUT8(m->audio_regs[i]);
    if (n + 0x400 > cap) return -1;
    memcpy(buf + n, m->exram, 0x400);
    n += 0x400;
#undef PUT8
#undef PUT16
    return n;
}

int mmc5_state_set(Mmc5 *m, const uint8_t *buf, int len) {
    int n = 0;
#define GET8()  (n < len ? buf[n++] : 0)
#define GET16() (n + 1 < len ? (n += 2, (uint16_t)(buf[n - 2] | (buf[n - 1] << 8))) : (n = len, 0))
    if (len < 1 || buf[0] != MMC5_STATE_VER) return 0;
    n = 1;
    m->prg_mode = GET8() & 3; m->chr_mode = GET8() & 3; m->ram_protect1 = GET8(); m->ram_protect2 = GET8();
    m->exram_mode = GET8() & 3; m->nt_map = GET8(); m->fill_tile = GET8(); m->fill_attr = GET8(); m->wram_bank = GET8() & 7;
    for (int i = 0; i < 3; i++) m->prg_reg[i] = GET8();
    m->prg_last = GET8();
    for (int i = 0; i < 8; i++) m->chr_a[i] = GET16();
    for (int i = 0; i < 4; i++) m->chr_b[i] = GET16();
    m->chr_upper = GET8() & 3; m->chr_last_set_b = GET8();
    m->split_ctrl = GET8(); m->split_scroll = GET8(); m->split_bank = GET8();
    m->irq_compare = GET8(); m->irq_enabled = GET8(); m->irq_pending = GET8(); m->in_frame = GET8();
    m->mult_a = GET8(); m->mult_b = GET8();
    m->line_calls = GET16(); m->scanline = GET16(); m->irq_fired = GET8();
    for (int i = 0; i < 0x20; i++) m->audio_regs[i] = GET8();
    if (n + 0x400 <= len) memcpy(m->exram, buf + n, 0x400);
#undef GET8
#undef GET16
    mmc5_remap(m);
    return 1;
}
