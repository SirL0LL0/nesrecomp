/*
 * interp.c — 6502 interpreter fallback tier.
 *
 * See interp.h and docs/PHASE1_INTERP_FALLBACK_PLAN.md for the design.
 *
 * The opcode SEMANTICS here are a faithful mirror of code_generator.c's
 * emit_instruction: same flag macros (FLAG_NZ / NZC_ADD / NZC_SUB), same
 * effective-address forms (operand_addr_expr), same bus helpers
 * (nes_read/nes_write/nes_read16zp/nes_read16_jmpbug). The decode table is
 * the SAME table the recompiler uses (recompiler/src/cpu6502_decoder.c),
 * compiled into the runner — so interpreted and recompiled execution cannot
 * diverge in decode.
 *
 * The BOUNDARY CONTRACT (interp_run): the interpreter runs its own program
 * counter and never call_by_address()es a return address (that re-enters the
 * world from scratch and grows the C stack without bound — see the depth-510
 * note in code_generator.c). Nested missed calls stay inside one C frame on
 * the shared 6502 RAM stack. Control returns to native code when an RTS/RTI
 * (or an unbalanced stack pop) lifts g_cpu.S above the entry level S_floor —
 * at which point the native C call stack / continuation carries the return.
 */
#include "interp.h"
#include "nes_runtime.h"
#include "game_extras.h"
#include "mapper.h"
#include "game_extras.h"
#include "cpu6502_decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Tunables ---- */
#define INTERP_STEP_CAP   2000000   /* per-call cap; explicit resume renews on frames */
#define INTERP_MAX_DEPTH  64        /* nested interp_run guard (native callee misses) */

/* ---- Config (lazily initialised on first dispatch) ---- */
static int s_enabled = -1;          /* -1 = uninitialised */
static int s_native_handoff_mode = -1;
static int s_active_handoff_mode = -1; /* per-run override; nests with interp calls */
extern int g_recomp_push_all_jsr;   /* defined by the generated dispatch TU */

/* ---- Covered-ness probe ----
 * interp_run dispatches a JSR/JMP target through call_by_address. If the
 * target is covered, call_by_address runs it natively and returns 1. If not,
 * the generated miss path calls back into nes_interp_dispatch; when that
 * call's address matches the armed probe we answer "miss" (return 0) WITHOUT
 * recursing, so interp_run handles the target inline on the 6502 stack. */
static int      s_probe_armed = 0;
static uint16_t s_probe_addr  = 0;

static int s_depth = 0;             /* interp_run nesting depth */

/* nes_interp_run_until state (applies only to the run started by that call, identified by depth) */
static int      s_ru_active = 0, s_ru_floor_valid = 0, s_ru_depth = 0;
static uint16_t s_ru_pc = 0;
static uint8_t  s_ru_s = 0, s_ru_floor = 0;
static int      s_ru_reason = 0;    /* 1 = reached stop_pc, 2 = returned past the C function's frame (floor) */

/* Generated control-flow uses these sidecars to carry a guest continuation
 * across C frames. The interpreter must participate in the same contract when
 * a native handoff returns through an interpreted caller. */
extern uint16_t g_rts_target;
extern uint16_t g_rti_target;
extern uint16_t g_rti_source;
extern int      g_rti_bank;

/* ---- Stats ---- */
static NesInterpStats s_stats;
static char s_last_decline_reason[96] = "interpreter fallback declined";
static NesInterpExit s_last_exit = {
    NES_INTERP_EXIT_DECLINED, 0, 0, 0, 0
};

void nes_interp_get_stats(NesInterpStats *out) { if (out) *out = s_stats; }
void nes_interp_frame_boundary(void) { s_stats.instrs_this_frame = 0; }
void nes_interp_get_last_exit(NesInterpExit *out) { if (out) *out = s_last_exit; }

typedef struct {
    uint8_t used;
    NesInterpHotspot value;
} InterpHotspotSlot;
static InterpHotspotSlot s_hotspots[NES_INTERP_HOTSPOT_CAP];

static void interp_note_hotspot(uint16_t entry, int bank, uint32_t instrs) {
    uint32_t key = ((uint32_t)(uint16_t)bank << 16) | entry;
    uint32_t slot = (key * 2654435761u) & (NES_INTERP_HOTSPOT_CAP - 1);
    for (uint32_t probe = 0; probe < NES_INTERP_HOTSPOT_CAP; probe++) {
        InterpHotspotSlot *s =
            &s_hotspots[(slot + probe) & (NES_INTERP_HOTSPOT_CAP - 1)];
        if (!s->used) {
            memset(&s->value, 0, sizeof(s->value));
            s->used = 1;
            s->value.entry_pc = entry;
            s->value.bank = (int16_t)bank;
        }
        if (s->value.entry_pc == entry && s->value.bank == bank) {
            s->value.period_calls++;
            s->value.period_instrs += instrs;
            if (instrs > s->value.period_max_run)
                s->value.period_max_run = instrs;
            s->value.total_calls++;
            s->value.total_instrs += instrs;
            return;
        }
    }
}

int nes_interp_get_hotspots(NesInterpHotspot *out, int cap, int clear_period) {
    if (cap < 0) cap = 0;
    int copied = 0;
    for (int i = 0; i < NES_INTERP_HOTSPOT_CAP; i++) {
        InterpHotspotSlot *s = &s_hotspots[i];
        if (!s->used || !s->value.period_calls) continue;
        if (out && copied < cap) out[copied] = s->value;
        if (copied < cap) copied++;
        if (clear_period) {
            s->value.period_calls = 0;
            s->value.period_instrs = 0;
            s->value.period_max_run = 0;
        }
    }
    return copied;
}

void nes_interp_set_enabled(int enabled) { s_enabled = enabled ? 1 : 0; }
int  nes_interp_is_enabled(void) { return s_enabled == 1; }

void nes_interp_set_native_handoff_mode(NesInterpHandoffMode mode) {
    if (mode < NES_INTERP_HANDOFF_ISLAND || mode > NES_INTERP_HANDOFF_LEGACY)
        mode = NES_INTERP_HANDOFF_ISLAND;
    s_native_handoff_mode = (int)mode;
}

static void interp_lazy_init(void) {
    if (s_enabled != -1 && s_native_handoff_mode != -1) return;
    /* Default: on when the build supports the stack contract; env can force off. */
    int on = g_recomp_push_all_jsr ? 1 : 0;
    const char *e = getenv("NESRECOMP_INTERP_FALLBACK");
    if (e) {
        if (!strcmp(e, "off") || !strcmp(e, "0")) on = 0;
        else if (!strcmp(e, "on") || !strcmp(e, "1")) on = g_recomp_push_all_jsr ? 1 : 0;
    }
    if (s_enabled == -1) {
        s_enabled = on;
        if (on && !g_recomp_push_all_jsr) s_enabled = 0; /* contract needs push_all_jsr */
    }

    /* Native collaboration policy while already inside the interpreter.
     * off/island (default): the interpreter owns all nested control flow until
     * an architectural exit. This is the correctness floor: a stack/return
     * rewriting helper cannot be split across two execution models.
     * safe: allow balanced JSR handoffs, keep JMP tails in the island.
     * on/legacy: probe and hand off both JSR and JMP. */
    if (s_native_handoff_mode == -1) {
        int mode = NES_INTERP_HANDOFF_ISLAND;
        const char *h = getenv("NESRECOMP_INTERP_NATIVE_HANDOFF");
        if (h && *h) {
            if (!strcmp(h, "on") || !strcmp(h, "1") || !strcmp(h, "legacy"))
                mode = NES_INTERP_HANDOFF_LEGACY;
            else if (!strcmp(h, "off") || !strcmp(h, "0") || !strcmp(h, "island"))
                mode = NES_INTERP_HANDOFF_ISLAND;
            else if (!strcmp(h, "safe"))
                mode = NES_INTERP_HANDOFF_SAFE;
        }
        const char *island = getenv("NESRECOMP_INTERP_ISLAND");
        if (island && *island) {
            if (!strcmp(island, "on") || !strcmp(island, "1"))
                mode = NES_INTERP_HANDOFF_ISLAND;
            else if (!strcmp(island, "off") || !strcmp(island, "0"))
                mode = NES_INTERP_HANDOFF_LEGACY;
        }
        s_native_handoff_mode = mode;
    }
}

NesInterpHandoffMode nes_interp_get_native_handoff_mode(void) {
    if (s_native_handoff_mode < 0) interp_lazy_init();
    return (NesInterpHandoffMode)s_native_handoff_mode;
}

static void interp_note_decline(uint16_t entry, uint16_t ipc, const char *reason) {
    const char *why = (reason && *reason) ? reason : "interpreter fallback declined";
    snprintf(s_last_decline_reason, sizeof(s_last_decline_reason),
             "%s (entry=$%04X ipc=$%04X bank=%d)",
             why, entry, ipc, g_current_bank);
    s_stats.declines++;
}

static int interp_native_handoff_allowed(int is_tail) {
    if (s_native_handoff_mode < 0) interp_lazy_init();
    int mode = (s_active_handoff_mode >= 0)
             ? s_active_handoff_mode : s_native_handoff_mode;
    if (mode == NES_INTERP_HANDOFF_LEGACY) return 1;
    if (mode == NES_INTERP_HANDOFF_ISLAND) return 0;
    return is_tail ? 0 : 1; /* safe */
}

/* ---- Side-effect-free instruction fetch (bank-correct) ---- */
static inline uint8_t interp_fetch(uint16_t pc) {
    if (pc >= 0x8000)              return mapper_peek_prg(pc);
    if (pc >= 0x6000 && mapper_get_type() == 40)
        return mapper_peek_prg(pc);
    if (pc < 0x2000)              return g_ram[pc & 0x07FF];
    if (pc >= 0x6000) {
        uint8_t v = g_sram[pc - 0x6000];
        nes_trace_sram_fetch(pc, v);
        return v;
    }
    return 0x00; /* $2000-$5FFF: not a code region — decodes as BRK and bails */
}

/* ---- Effective address — mirrors operand_addr_expr() exactly ---- */
static inline uint16_t interp_ea(AddrMode am, uint8_t op1, uint8_t op2) {
    uint16_t abs16 = (uint16_t)(op1 | ((uint16_t)op2 << 8));
    switch (am) {
        case AM_ZP:   return op1;
        case AM_ZPX:  return (uint8_t)(op1 + g_cpu.X);
        case AM_ZPY:  return (uint8_t)(op1 + g_cpu.Y);
        case AM_ABS:  return abs16;
        case AM_ABSX: return (uint16_t)(abs16 + g_cpu.X);
        case AM_ABSY: return (uint16_t)(abs16 + g_cpu.Y);
        case AM_INDX: return nes_read16zp((uint8_t)(op1 + g_cpu.X));
        case AM_INDY: return (uint16_t)(nes_read16zp(op1) + g_cpu.Y);
        default:      return 0;
    }
}

/* Read the operand value for a read-type op (immediate vs memory). */
static inline uint8_t interp_rd(AddrMode am, uint8_t op1, uint8_t op2) {
    if (am == AM_IMM) return op1;
    return nes_read(interp_ea(am, op1, op2));
}

/* ---- Flag helpers (identical to generated FLAG_NZ / NZC_ADD / NZC_SUB) ---- */
#define I_NZ(v) do { g_cpu.N = ((uint8_t)(v) >> 7) & 1; g_cpu.Z = ((uint8_t)(v) == 0) ? 1 : 0; } while (0)

/* ---- Translated basic blocks (see nes_blocks.h) ---- */
#include "nes_blocks.h"
#define BLK_UNITS 64
static uint16_t s_cov_seq; static int s_cov_seq_ok;
static const NesBlockEntry *s_blk_tab = NULL;
static int                  s_blk_n = 0;
static const uint8_t       *s_blk_code = NULL;               /* codebits from the generator */
static const NesBlockEntry *s_blk_by_unit[BLK_UNITS][8192];  /* lazily filled lookup, [unit][off] */
static int                  s_blk_on = 0;
static uint8_t              s_blk_miss[BLK_UNITS * 1024];   /* interpreted instr outside the translated set */
static uint8_t              s_blk_miss_seen_entry[BLK_UNITS * 1024];
static uint64_t s_blk_instrs = 0, s_blk_runs = 0, s_int_known = 0, s_int_miss = 0, s_int_dyn = 0, s_int_ctrl = 0;
static uint64_t s_ctl_table_hits = 0, s_ctl_decoded = 0;
static int s_note_ctrl = 0;   /* the instruction being noted is JSR/JMP/RTS/RTI/BRK (block boundaries: control flow stays in interp.c) */
static uint32_t *s_int_hist = NULL;      /* [unit<<13 | off] instructions run by the pure interpreter (NESRECOMP_INTERP_HIST=N prints the top N) */

static void hist_report(void) {
    const char *e = getenv("NESRECOMP_INTERP_HIST");
    if (!e || !s_int_hist) return;
    int top = atoi(e) > 0 ? atoi(e) : 30;
    for (int k = 0; k < top; k++) {
        uint32_t best = 0; uint32_t bi = 0;
        for (uint32_t i = 0; i < (BLK_UNITS << 13); i++) if (s_int_hist[i] > best) { best = s_int_hist[i]; bi = i; }
        if (!best) break;
        fprintf(stderr, "[hist] unit %2u  off %04X  %u\n", bi >> 13, bi & 0x1FFF, best);
        s_int_hist[bi] = 0;
    }
}

static void blocks_report(void) {
    if (!s_blk_n) return;
    hist_report();
    unsigned distinct = 0;
    for (int i = 0; i < BLK_UNITS * 1024; i++)
        for (int b = 0; b < 8; b++) if (s_blk_miss[i] & (1u << b)) distinct++;
    fprintf(stderr, "[blocks] translated-block instrs=%llu (runs=%llu) | interpreted: in-translated-set=%llu MISS=%llu (distinct addrs %u) RAM/WRAM=%llu\n",
            (unsigned long long)s_blk_instrs, (unsigned long long)s_blk_runs, (unsigned long long)s_int_known,
            (unsigned long long)s_int_miss, distinct, (unsigned long long)s_int_dyn);
    fprintf(stderr, "[blocks] instructions the executor read and decoded from ROM: %llu; control transfers taken from the table: %llu\n",
            (unsigned long long)s_ctl_decoded, (unsigned long long)s_ctl_table_hits);
    fprintf(stderr, "[blocks] of the interpreted instructions in the translated set, %llu are control transfers (JSR/JMP/RTS/RTI/BRK); other code the interpreter had to decode: %llu\n",
            (unsigned long long)s_int_ctrl, (unsigned long long)(s_int_known - s_int_ctrl + s_int_miss + s_int_dyn));
    {   /* where the guest instructions actually ran: NB_STEP is called by decompiled functions and by blocks alike */
        uint64_t total = s_stats.instrs_total, interp = s_int_known + s_int_miss + s_int_dyn;
        uint64_t dec = total > s_blk_instrs + interp ? total - s_blk_instrs - interp : 0;
        fprintf(stderr, "[where] total=%llu | decompiled C=%llu (%.1f%%) | translated blocks=%llu (%.1f%%) | pure interpreter=%llu (%.1f%%)\n",
                (unsigned long long)total, (unsigned long long)dec, total ? 100.0 * dec / total : 0.0,
                (unsigned long long)s_blk_instrs, total ? 100.0 * s_blk_instrs / total : 0.0,
                (unsigned long long)interp, total ? 100.0 * interp / total : 0.0);
    }
    const char *mf = getenv("NESRECOMP_BLOCK_MISS_FILE");
    if (mf && *mf) {
        FILE *f = fopen(mf, "w");
        if (f) {
            for (int u = 0; u < BLK_UNITS; u++)
                for (int off = 0; off < 8192; off++)
                    if (s_blk_miss[u * 1024 + (off >> 3)] & (1u << (off & 7))) {
                        int e = (s_blk_miss_seen_entry[u * 1024 + (off >> 3)] >> (off & 7)) & 1;
                        fprintf(f, "%02X %04X%s\n", u, (unsigned)off, e ? " entry" : "");
                    }
            fclose(f);
        }
    }
}

static const NesCtlEntry *s_ctl_by_unit[BLK_UNITS][8192];
static int s_ctl_n = 0;

void nes_blocks_install_ctl(const NesCtlEntry *tab, int n) {
    uint32_t prg_size = 0;
    const uint8_t *prg = mapper_get_prg_raw(&prg_size);
    int good = 0;
    memset(s_ctl_by_unit, 0, sizeof s_ctl_by_unit);
    for (int i = 0; i < n; i++) {
        uint32_t o = ((uint32_t)tab[i].unit << 13) | tab[i].off;
        /* the operand bytes must match the ROM that is running (translated ROMs patch code) */
        if (prg && o + 3 <= prg_size && prg[o] == tab[i].op &&
            (g_opcode_table[tab[i].op].size < 2 || prg[o + 1] == tab[i].a) &&
            (g_opcode_table[tab[i].op].size < 3 || prg[o + 2] == tab[i].b)) {
            s_ctl_by_unit[tab[i].unit][tab[i].off] = &tab[i];
            good++;
        }
    }
    s_ctl_n = good;
    fprintf(stderr, "[blocks] %d/%d control transfers match this ROM (executed from the table, no decode)\n", good, n);
}

static inline const NesCtlEntry *ctl_lookup(uint16_t pc) {
    if (!s_ctl_n || pc < 0x8000) return NULL;
    int w = (pc - 0x8000) >> 13;
    int unit = g_mmc5_win_bank8k[w];
    if (unit < 0 || unit >= BLK_UNITS) return NULL;
    const NesCtlEntry *c = s_ctl_by_unit[unit][pc & 0x1FFF];
    return (c && c->win == w) ? c : NULL;
}

void nes_blocks_install(const NesBlockEntry *tab, int n, const uint8_t *codebits, int nunits) {
    const char *dis = getenv("NESRECOMP_BLOCKS");
    if ((dis && (!strcmp(dis, "0") || !strcmp(dis, "off"))) || getenv("NESRECOMP_PC_TRACE")) {
        fprintf(stderr, "[blocks] translated blocks disabled by environment\n");
    } else {
        s_blk_on = 1;
    }
    (void)nunits;
    s_blk_tab = tab; s_blk_n = n; s_blk_code = codebits;
    memset(s_blk_by_unit, 0, sizeof s_blk_by_unit);
    /* Generated blocks describe the ROM they were made from; a translated ROM patches code, so every block is
     * checked against the PRG that is actually loaded and skipped (= interpreted) on mismatch. */
    uint32_t prg_size = 0;
    const uint8_t *prg = mapper_get_prg_raw(&prg_size);
    int good = 0;
    for (int i = 0; i < n; i++) {
        uint32_t o = ((uint32_t)tab[i].unit << 13) | tab[i].off;
        if (prg && o + tab[i].nbytes <= prg_size && nes_fnv1a(prg + o, tab[i].nbytes, NES_FNV_INIT) == tab[i].hash) {
            s_blk_by_unit[tab[i].unit][tab[i].off] = &tab[i];
            good++;
        }
    }
    if (getenv("NESRECOMP_INTERP_HIST")) s_int_hist = (uint32_t *)calloc((size_t)BLK_UNITS << 13, sizeof(uint32_t));
    atexit(blocks_report);
    fprintf(stderr, "[blocks] %d/%d translated blocks match this ROM and are installed (%s)\n", good, n, s_blk_on ? "active" : "inactive");
}

/* ---- Decompiled functions registry (nes_decomp.h) ---- */
#include "nes_decomp.h"
static const NesDecompEntry *s_dec_by_unit[BLK_UNITS][8192];
static int s_dec_on = 0;

void nes_decomp_install(const NesDecompEntry *tab, int n, uint8_t *valid) {
    const char *dis = getenv("NESRECOMP_DECOMP");
    memset(s_dec_by_unit, 0, sizeof s_dec_by_unit);
    /* Each function carries a hash of its instruction bytes: functions whose code differs in the loaded ROM
     * (translated ROMs patch code) are marked invalid; their C body falls back to the interpreter (DEC_GUARD). */
    uint32_t prg_size = 0;
    const uint8_t *prg = mapper_get_prg_raw(&prg_size);
    int good = 0;
    for (int i = 0; i < n; i++) {
        uint32_t h = NES_FNV_INIT;
        int ok = prg != NULL;
        for (const uint16_t *r = tab[i].ranges; ok && r[1]; r += 2) {
            uint32_t o = ((uint32_t)tab[i].unit << 13) | r[0];
            if (o + r[1] > prg_size) { ok = 0; break; }
            h = nes_fnv1a(prg + o, r[1], h);
        }
        valid[i] = (ok && h == tab[i].hash) ? 1 : 0;
        if (valid[i]) { s_dec_by_unit[tab[i].unit][tab[i].off] = &tab[i]; good++; }
    }
    s_dec_on = !(dis && (!strcmp(dis, "0") || !strcmp(dis, "off")));
    fprintf(stderr, "[decomp] %d/%d decompiled functions match this ROM (%s)\n", good, n, s_dec_on ? "active" : "inactive");
}

NesDecompFn nes_decomp_lookup(uint16_t addr) {
    if (!s_dec_on || addr < 0x8000) return NULL;
    int w = (addr - 0x8000) >> 13;
    int unit = g_mmc5_win_bank8k[w];
    if (unit < 0 || unit >= BLK_UNITS) return NULL;
    const NesDecompEntry *e = s_dec_by_unit[unit][addr & 0x1FFF];
    return (e && e->win == w) ? e->fn : NULL;
}

void nes_interp_step_hook(uint16_t pc, uint8_t opcode) {
    const OpcodeEntry *e = &g_opcode_table[opcode];
    {   /* NESRECOMP_STEP_TRACE="8096,809B": print S and registers when translated/decompiled code executes those PCs */
        static int s_st = -1; static uint16_t s_st_list[32]; static int s_st_n;
        if (s_st < 0) {
            const char *env = getenv("NESRECOMP_STEP_TRACE"); s_st = 0;
            for (const char *p = env; p && *p && s_st_n < 32; ) {
                s_st_list[s_st_n++] = (uint16_t)strtoul(p, (char **)&p, 16); s_st = 1;
                if (*p == ',') p++; else break;
            }
        }
        if (s_st) for (int q = 0; q < s_st_n; q++) if (s_st_list[q] == pc)
            fprintf(stderr, "[step] pc=%04X A=%02X X=%02X Y=%02X S=%02X\n", pc, g_cpu.A, g_cpu.X, g_cpu.Y, g_cpu.S);
    }
    nes_cpu_instruction_boundary(pc, e->cycles);
    mapper_cov_mark(pc, e->size, !(s_cov_seq_ok && pc == s_cov_seq));
    s_cov_seq = (uint16_t)(pc + e->size); s_cov_seq_ok = 1;
    s_stats.instrs_total++;
    s_stats.instrs_this_frame++;
}

/* Block for the live PC, or NULL. The live window mapping must match the unit and window the
 * block was generated for; RAM/WRAM code never has one. */
static inline const NesBlockEntry *blk_lookup(uint16_t pc) {
    if (pc < 0x8000) return NULL;
    int w = (pc - 0x8000) >> 13;
    int unit = g_mmc5_win_bank8k[w];
    if (unit < 0 || unit >= BLK_UNITS) return NULL;
    const NesBlockEntry *b = s_blk_by_unit[unit][pc & 0x1FFF];
    return (b && b->win == w) ? b : NULL;
}

/* Statistics for an instruction the interpreter executes itself. */
static void blk_note_interp(uint16_t pc, int is_entry) {
    int unit = (pc >= 0x8000) ? g_mmc5_win_bank8k[(pc - 0x8000) >> 13] : -1;
    if (unit < 0 || unit >= BLK_UNITS) { s_int_dyn++; return; }
    unsigned off = pc & 0x1FFF;
    if (s_int_hist) s_int_hist[((unsigned)unit << 13) | off]++;
    if (s_blk_code[unit * 1024 + (off >> 3)] & (1u << (off & 7))) { s_int_known++; if (s_note_ctrl) s_int_ctrl++; return; }
    s_int_miss++;
    s_blk_miss[unit * 1024 + (off >> 3)] |= (uint8_t)(1u << (off & 7));
    if (is_entry) s_blk_miss_seen_entry[unit * 1024 + (off >> 3)] |= (uint8_t)(1u << (off & 7));
}

/* 1 = BRK behaves like hardware (vector through $FFFE) instead of ending the run. */
int g_interp_hw_brk = 0;

/* Forward decl: the generated dispatcher. */
extern int call_by_address(uint16_t addr);

/* Probe + dispatch a control-transfer target.
 * Returns 1 if the target was covered and executed natively; 0 on miss
 * (caller interprets the target inline). */
static int interp_probe_native_target(uint16_t target) {
    nes_dring_mark('P', target);   /* interp probe (pre-dispatch) */
    g_rts_target = 0;
    g_rti_target = 0;
    g_rti_source = 0;
    g_rti_bank = -1;
    s_probe_armed = 1;
    s_probe_addr  = target;
    int hit = call_by_address(target);
    s_probe_armed = 0;
    if (hit) s_stats.native_handoffs++;
    return hit;
}

static int interp_dispatch_target(uint16_t target, int is_tail) {
    if (!interp_native_handoff_allowed(is_tail)) {
        s_stats.native_handoffs_suppressed++;
        return 0;
    }
    return interp_probe_native_target(target);
}

/* NESRECOMP_WATCH="0020,0205": report which instruction (or interrupt at the boundary before ipc) changed those CPU
 * RAM bytes; only sees changes made while the interpreter drives (translated code runs between checks). */
static void interp_watch(uint16_t ipc, const char *tag) {
    static int s_wt = -1; static uint16_t s_wt_addr[8]; static uint8_t s_wt_old[8]; static int s_wt_n; static uint16_t s_wt_pc;
    static long s_wt_lines;
    if (s_wt < 0) {
        const char *env = getenv("NESRECOMP_WATCH"); s_wt = 0;
        for (const char *p = env; p && *p && s_wt_n < 8; ) {
            unsigned v = (unsigned)strtoul(p, (char **)&p, 16); s_wt_addr[s_wt_n] = (uint16_t)(v & 0x7FF);
            s_wt_old[s_wt_n++] = g_ram[v & 0x7FF]; s_wt = 1;
            if (*p == ',') p++; else break;
        }
    }
    if (s_wt != 1 || s_wt_lines >= 200000) return;
    for (int q = 0; q < s_wt_n; q++)
        if (g_ram[s_wt_addr[q]] != s_wt_old[q]) {
            fprintf(stderr, "[W] f=%llu $%04X %02X->%02X (%s) prev_pc=$%04X ipc=$%04X A=%02X X=%02X Y=%02X S=%02X\n",
                    (unsigned long long)g_frame_count, s_wt_addr[q], s_wt_old[q], g_ram[s_wt_addr[q]], tag,
                    s_wt_pc, ipc, g_cpu.A, g_cpu.X, g_cpu.Y, g_cpu.S);
            s_wt_old[q] = g_ram[s_wt_addr[q]]; s_wt_lines++;
        }
    s_wt_pc = ipc;
}

static NesInterpExit make_exit(NesInterpExitKind kind, uint16_t entry,
                               uint16_t next_pc, uint8_t entry_s) {
    NesInterpExit out;
    out.kind = kind;
    out.entry_pc = entry;
    out.next_pc = next_pc;
    out.entry_s = entry_s;
    out.exit_s = g_cpu.S;
    return out;
}

/*
 * interp_run — execute the missed routine at `entry`, returning when control
 * leaves it back to native code (RTS/RTI/unbalanced-pop lifting S above the
 * entry level). The typed exit is the architectural boundary contract; the
 * public generated-code ABI converts it to handled/not-handled for now.
 */
static NesInterpExit interp_run_ex(uint16_t entry, int stop_on_stack_lift,
                                    NesInterpHandoffMode handoff_mode,
                                    uint32_t max_steps) {
    const uint8_t entry_s = g_cpu.S;
    const int entry_bank = g_current_bank;
    if (s_depth >= INTERP_MAX_DEPTH) {
        interp_note_decline(entry, entry, "interpreter depth guard");
        s_last_exit = make_exit(NES_INTERP_EXIT_DECLINED, entry, entry, entry_s);
        return s_last_exit;
    }
    s_depth++;
    int previous_handoff_mode = s_active_handoff_mode;
    s_active_handoff_mode = (int)handoff_mode;
    s_stats.runs++;
    {
        static int s_rt0 = -2; static long s_rt0_lines;
        if (s_rt0 == -2) { const char *e = getenv("NESRECOMP_RUN_TRACE"); s_rt0 = e ? atoi(e) : -1; }
        if (s_rt0 >= 0 && (long long)g_frame_count >= s_rt0 && s_rt0_lines < 4000) {
            s_rt0_lines++;
            fprintf(stderr, "[run] BEG f=%llu depth=%d entry=$%04X stop=%d mode=%d S=%02X\n",
                    (unsigned long long)g_frame_count, s_depth, entry, stop_on_stack_lift, (int)handoff_mode, g_cpu.S);
        }
    }
    nes_dring_mark('I', entry);   /* interp run start (cpu pc) */

    const uint8_t S_floor = entry_s;
    uint16_t ipc = entry;
    long budget = INTERP_STEP_CAP;
    uint64_t budget_frame = g_frame_count;
    uint32_t this_run = 0;
    NesInterpExit result =
        make_exit(NES_INTERP_EXIT_NATIVE_ESCAPE, entry, entry, entry_s);

    for (;;) {
        /* An explicit save-state continuation owns the whole program and may
         * legitimately stay here forever (Metroid's multi-instruction NMI
         * wait loop). Cap instructions WITHOUT frame progress, not the total
         * lifetime after loading. Ordinary fallback calls keep their original
         * per-call guard, including calls nested inside a resumed program. */
        if (!stop_on_stack_lift && budget_frame != g_frame_count) {
            budget_frame = g_frame_count;
            budget = INTERP_STEP_CAP;
        }
        if (--budget < 0) {
            fprintf(stderr, "[interp] WATCHDOG: run from $%04X exceeded %d instrs "
                            "%s (bank=%d, ipc=$%04X) — bailing\n",
                    entry, INTERP_STEP_CAP,
                    stop_on_stack_lift ? "in one call" : "without frame progress",
                    g_current_bank, ipc);
            s_stats.watchdog_trips++;
            interp_note_decline(entry, ipc, "interpreter watchdog");
            result = make_exit(NES_INTERP_EXIT_DECLINED, entry, ipc, entry_s);
            break;
        }

        if (s_ru_active && s_ru_depth == s_depth &&
            ((s_ru_pc && ipc == s_ru_pc && g_cpu.S == s_ru_s) ||
             (s_ru_floor_valid && g_cpu.S > s_ru_floor))) {
            static int s_ru_dbg = -1;
            if (s_ru_dbg < 0) s_ru_dbg = getenv("NESRECOMP_RU_DEBUG") ? 1 : 0;
            if (s_ru_dbg)
                fprintf(stderr, "[ru] exit entry=%04X at ipc=%04X S=%02X | stop_pc=%04X stop_s=%02X floor_valid=%d floor=%02X depth=%d\n",
                        entry, ipc, g_cpu.S, s_ru_pc, s_ru_s, s_ru_floor_valid, s_ru_floor, s_depth);
            s_ru_reason = (s_ru_pc && ipc == s_ru_pc && g_cpu.S == s_ru_s) ? 1 : 2;
            result = make_exit(NES_INTERP_EXIT_NATIVE_ESCAPE, entry, ipc, entry_s);
            goto done;
        }

        if (s_blk_on && !max_steps) {
            const NesBlockEntry *b = blk_lookup(ipc);
            if (b) {
                budget -= (long)b->ninsn - 1;
                uint16_t np = b->fn();
                s_blk_instrs += b->ninsn; s_blk_runs++;
                if (this_run <= UINT32_MAX - b->ninsn) this_run += b->ninsn;
                ipc = np;
                if (stop_on_stack_lift && g_cpu.S > S_floor) {
                    result = make_exit(NES_INTERP_EXIT_STACK_ESCAPE, entry, np, entry_s);
                    goto done;
                }
                continue;
            }
        }

        uint8_t opcode, op1, op2;
        {   const NesCtlEntry *ce = (s_blk_on && !max_steps) ? ctl_lookup(ipc) : NULL;
            if (ce) { opcode = ce->op; op1 = ce->a; op2 = ce->b; s_ctl_table_hits++; }
            else {
                opcode = interp_fetch(ipc);
                op1 = (g_opcode_table[opcode].size > 1) ? interp_fetch((uint16_t)(ipc + 1)) : 0;
                op2 = (g_opcode_table[opcode].size > 2) ? interp_fetch((uint16_t)(ipc + 2)) : 0;
                s_ctl_decoded++;
                {   /* NESRECOMP_STRICT=1: log every address the executor had to decode from ROM (code that was never translated);
                     * NESRECOMP_STRICT=2: stop at the first one. This is what "no interpreter needed" means. */
                    static int s_strict = -1; static int s_strict_n;
                    if (s_strict < 0) { const char *e2 = getenv("NESRECOMP_STRICT"); s_strict = e2 ? atoi(e2) : 0; }
                    if (s_strict && s_blk_on && !max_steps && s_strict_n < 200) {
                        s_strict_n++;
                        fprintf(stderr, "[strict] untranslated code executed at $%04X (op %02X, window unit %d, frame %llu)\n", ipc, opcode,
                                ipc >= 0x8000 ? g_mmc5_win_bank8k[(ipc - 0x8000) >> 13] : -1, (unsigned long long)g_frame_count);
                        if (s_strict >= 2) { fflush(stderr); exit(3); }
                    }
                }
            }
        }
        const OpcodeEntry *e = &g_opcode_table[opcode];
        uint16_t abs16 = (uint16_t)(op1 | ((uint16_t)op2 << 8));

        /* NMI is sampled between instructions (mirrors codegen's per-insn call). */
        interp_watch(ipc, "insn");
        nes_cpu_instruction_boundary(ipc, e->cycles);
        interp_watch(ipc, "interrupt/boundary");
        {
            /* NESRECOMP_PC_TRACE="D094,C21D": log frame + registers whenever one of these PCs executes */
            static int s_pt = -1; static uint16_t s_pt_list[16]; static int s_pt_n; static long s_pt_lines;
            if (s_pt < 0) {
                const char *e = getenv("NESRECOMP_PC_TRACE"); s_pt = 0;
                for (const char *p = e; p && *p && s_pt_n < 16; ) {
                    unsigned v = (unsigned)strtoul(p, (char **)&p, 16); s_pt_list[s_pt_n++] = (uint16_t)v; s_pt = 1;
                    if (*p == ',') p++; else break;
                }
            }
            if (s_pt == 1 && s_pt_lines < 400000) {
                for (int q = 0; q < s_pt_n; q++)
                    if (s_pt_list[q] == ipc) {
                        uint16_t ret = (uint16_t)((g_ram[0x100 + (uint8_t)(g_cpu.S + 1)] | (g_ram[0x100 + (uint8_t)(g_cpu.S + 2)] << 8)) + 1);
                        fprintf(stderr, "[PC] f=%llu pc=%04X A=%02X X=%02X Y=%02X S=%02X BE=%02X%02X ret=%04X w=%d,%d,%d,%d\n",
                                (unsigned long long)g_frame_count, ipc, g_cpu.A, g_cpu.X, g_cpu.Y, g_cpu.S,
                                g_ram[0xBF], g_ram[0xBE], ret,
                                g_mmc5_win_bank8k[0], g_mmc5_win_bank8k[1], g_mmc5_win_bank8k[2], g_mmc5_win_bank8k[3]);
                        {   /* NESRECOMP_PC_TRACE_ZP="A4,A5,A6,A7": append those zero-page bytes */
                            static const char *zp = (const char *)-1;
                            if (zp == (const char *)-1) zp = getenv("NESRECOMP_PC_TRACE_ZP");
                            if (zp) {
                                fprintf(stderr, "[ZP]");
                                for (const char *z = zp; *z; ) {
                                    unsigned a = (unsigned)strtoul(z, (char **)&z, 16);
                                    fprintf(stderr, " %02X=%02X", a, g_ram[a & 0x7FF]);
                                    if (*z == ',') z++; else break;
                                }
                                fprintf(stderr, "\n");
                            }
                        }
                        s_pt_lines++;
                    }
            }
        }
        {
            /* arrival is a "target" unless sequential */
            mapper_cov_mark(ipc, e->size, !(s_cov_seq_ok && ipc == s_cov_seq));
            s_cov_seq = (uint16_t)(ipc + e->size); s_cov_seq_ok = 1;
        }
        s_stats.instrs_total++;
        s_stats.instrs_this_frame++;
        if (s_blk_n) { s_note_ctrl = (e->mnemonic == MN_JSR || e->mnemonic == MN_JMP || e->mnemonic == MN_RTS || e->mnemonic == MN_RTI || e->mnemonic == MN_BRK); blk_note_interp(ipc, 0); }
        if (this_run < UINT32_MAX) this_run++;

        uint16_t next = (uint16_t)(ipc + e->size); /* default sequential advance */

        switch (e->mnemonic) {
            /* ---- Load / Store ---- */
            case MN_LDA: g_cpu.A = interp_rd(e->addr_mode, op1, op2); I_NZ(g_cpu.A); break;
            case MN_LDX: g_cpu.X = interp_rd(e->addr_mode, op1, op2); I_NZ(g_cpu.X); break;
            case MN_LDY: g_cpu.Y = interp_rd(e->addr_mode, op1, op2); I_NZ(g_cpu.Y); break;
            case MN_LAX: g_cpu.A = g_cpu.X = interp_rd(e->addr_mode, op1, op2); I_NZ(g_cpu.A); break;
            case MN_STA: nes_write(interp_ea(e->addr_mode, op1, op2), g_cpu.A); break;
            case MN_STX: nes_write(interp_ea(e->addr_mode, op1, op2), g_cpu.X); break;
            case MN_STY: nes_write(interp_ea(e->addr_mode, op1, op2), g_cpu.Y); break;
            case MN_SAX: nes_write(interp_ea(e->addr_mode, op1, op2), (uint8_t)(g_cpu.A & g_cpu.X)); break;

            /* ---- Transfers ---- */
            case MN_TAX: g_cpu.X = g_cpu.A; I_NZ(g_cpu.X); break;
            case MN_TAY: g_cpu.Y = g_cpu.A; I_NZ(g_cpu.Y); break;
            case MN_TXA: g_cpu.A = g_cpu.X; I_NZ(g_cpu.A); break;
            case MN_TYA: g_cpu.A = g_cpu.Y; I_NZ(g_cpu.A); break;
            case MN_TSX: g_cpu.X = g_cpu.S; I_NZ(g_cpu.X); break;
            case MN_TXS: g_cpu.S = g_cpu.X; break;

            /* ---- Stack ---- */
            case MN_PHA: g_ram[0x100 + g_cpu.S] = g_cpu.A; g_cpu.S--; break;
            case MN_PLA: g_cpu.S++; g_cpu.A = g_ram[0x100 + g_cpu.S]; I_NZ(g_cpu.A); break;
            case MN_PHP: {
                uint8_t p = (uint8_t)((g_cpu.N << 7) | (g_cpu.V << 6) | 0x30 |
                                      (g_cpu.D << 3) | (g_cpu.I << 2) | (g_cpu.Z << 1) | g_cpu.C);
                g_ram[0x100 + g_cpu.S] = p; g_cpu.S--;
                break;
            }
            case MN_PLP: {
                g_cpu.S++; uint8_t p = g_ram[0x100 + g_cpu.S];
                g_cpu.N = (p >> 7) & 1; g_cpu.V = (p >> 6) & 1; g_cpu.D = (p >> 3) & 1;
                g_cpu.I = (p >> 2) & 1; g_cpu.Z = (p >> 1) & 1; g_cpu.C = p & 1;
                break;
            }

            /* ---- ALU ---- */
            case MN_ADC: {
                uint8_t m = interp_rd(e->addr_mode, op1, op2);
                uint16_t r = (uint16_t)(g_cpu.A + m + g_cpu.C);
                g_cpu.C = (r > 0xFF) ? 1 : 0;
                g_cpu.N = (r >> 7) & 1;
                g_cpu.Z = ((r & 0xFF) == 0) ? 1 : 0;
                g_cpu.V = (~((g_cpu.A) ^ (m)) & ((g_cpu.A) ^ r) & 0x80) ? 1 : 0;
                g_cpu.A = (uint8_t)(r & 0xFF);
                break;
            }
            case MN_SBC: {
                uint8_t m = interp_rd(e->addr_mode, op1, op2);
                int16_t r = (int16_t)(g_cpu.A - m - (1 - g_cpu.C));
                g_cpu.C = (r >= 0) ? 1 : 0;
                g_cpu.N = ((r & 0xFF) >> 7);
                g_cpu.Z = ((r & 0xFF) == 0) ? 1 : 0;
                g_cpu.V = (((g_cpu.A) ^ (m)) & ((g_cpu.A) ^ r) & 0x80) ? 1 : 0;
                g_cpu.A = (uint8_t)(r & 0xFF);
                break;
            }
            case MN_AND: g_cpu.A &= interp_rd(e->addr_mode, op1, op2); I_NZ(g_cpu.A); break;
            case MN_ORA: g_cpu.A |= interp_rd(e->addr_mode, op1, op2); I_NZ(g_cpu.A); break;
            case MN_EOR: g_cpu.A ^= interp_rd(e->addr_mode, op1, op2); I_NZ(g_cpu.A); break;

            /* ---- Shifts / Rotates ---- */
            case MN_ASL:
                if (e->addr_mode == AM_ACC) {
                    g_cpu.C = (g_cpu.A >> 7) & 1; g_cpu.A = (uint8_t)(g_cpu.A << 1); I_NZ(g_cpu.A);
                } else {
                    uint16_t a = interp_ea(e->addr_mode, op1, op2); uint8_t v = nes_read(a);
                    g_cpu.C = (v >> 7) & 1; v = (uint8_t)(v << 1); nes_write(a, v); I_NZ(v);
                }
                break;
            case MN_LSR:
                if (e->addr_mode == AM_ACC) {
                    g_cpu.C = g_cpu.A & 1; g_cpu.A >>= 1; I_NZ(g_cpu.A);
                } else {
                    uint16_t a = interp_ea(e->addr_mode, op1, op2); uint8_t v = nes_read(a);
                    g_cpu.C = v & 1; v >>= 1; nes_write(a, v); I_NZ(v);
                }
                break;
            case MN_ROL:
                if (e->addr_mode == AM_ACC) {
                    uint8_t c = g_cpu.C; g_cpu.C = (g_cpu.A >> 7) & 1;
                    g_cpu.A = (uint8_t)((g_cpu.A << 1) | c); I_NZ(g_cpu.A);
                } else {
                    uint16_t a = interp_ea(e->addr_mode, op1, op2); uint8_t v = nes_read(a);
                    uint8_t c = g_cpu.C; g_cpu.C = (v >> 7) & 1;
                    v = (uint8_t)((v << 1) | c); nes_write(a, v); I_NZ(v);
                }
                break;
            case MN_ROR:
                if (e->addr_mode == AM_ACC) {
                    uint8_t c = g_cpu.C; g_cpu.C = g_cpu.A & 1;
                    g_cpu.A = (uint8_t)((g_cpu.A >> 1) | (c << 7)); I_NZ(g_cpu.A);
                } else {
                    uint16_t a = interp_ea(e->addr_mode, op1, op2); uint8_t v = nes_read(a);
                    uint8_t c = g_cpu.C; g_cpu.C = v & 1;
                    v = (uint8_t)((v >> 1) | (c << 7)); nes_write(a, v); I_NZ(v);
                }
                break;

            /* ---- Inc / Dec ---- */
            case MN_INC: {
                uint16_t a = interp_ea(e->addr_mode, op1, op2);
                uint8_t v = (uint8_t)(nes_read(a) + 1); nes_write(a, v); I_NZ(v);
                break;
            }
            case MN_DEC: {
                uint16_t a = interp_ea(e->addr_mode, op1, op2);
                uint8_t v = (uint8_t)(nes_read(a) - 1); nes_write(a, v); I_NZ(v);
                break;
            }
            case MN_INX: g_cpu.X = (uint8_t)(g_cpu.X + 1); I_NZ(g_cpu.X); break;
            case MN_DEX: g_cpu.X = (uint8_t)(g_cpu.X - 1); I_NZ(g_cpu.X); break;
            case MN_INY: g_cpu.Y = (uint8_t)(g_cpu.Y + 1); I_NZ(g_cpu.Y); break;
            case MN_DEY: g_cpu.Y = (uint8_t)(g_cpu.Y - 1); I_NZ(g_cpu.Y); break;

            /* ---- Compare ---- */
            case MN_CMP: { uint8_t m = interp_rd(e->addr_mode, op1, op2); int r = g_cpu.A - m; g_cpu.C = (g_cpu.A >= m) ? 1 : 0; I_NZ(r & 0xFF); break; }
            case MN_CPX: { uint8_t m = interp_rd(e->addr_mode, op1, op2); int r = g_cpu.X - m; g_cpu.C = (g_cpu.X >= m) ? 1 : 0; I_NZ(r & 0xFF); break; }
            case MN_CPY: { uint8_t m = interp_rd(e->addr_mode, op1, op2); int r = g_cpu.Y - m; g_cpu.C = (g_cpu.Y >= m) ? 1 : 0; I_NZ(r & 0xFF); break; }
            case MN_BIT: {
                uint8_t m = interp_rd(e->addr_mode, op1, op2);
                g_cpu.Z = (g_cpu.A & m) ? 0 : 1; g_cpu.N = (m >> 7) & 1; g_cpu.V = (m >> 6) & 1;
                break;
            }

            /* ---- Flags ---- */
            case MN_CLC: g_cpu.C = 0; break;
            case MN_SEC: g_cpu.C = 1; break;
            case MN_CLD: g_cpu.D = 0; break;
            case MN_SED: g_cpu.D = 1; break;
            case MN_CLI: g_cpu.I = 0; break;
            case MN_SEI: g_cpu.I = 1; break;
            case MN_CLV: g_cpu.V = 0; break;

            /* ---- NOPs (NOP_READ performs the operand read for MMIO side effects) ---- */
            case MN_NOP: break;
            case MN_NOP_READ:
                if (e->addr_mode != AM_IMP && e->addr_mode != AM_ACC && e->addr_mode != AM_IMM)
                    (void)nes_read(interp_ea(e->addr_mode, op1, op2));
                break;

            /* ---- Branches (relative) ---- */
            case MN_BCC: if (!g_cpu.C) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;
            case MN_BCS: if ( g_cpu.C) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;
            case MN_BEQ: if ( g_cpu.Z) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;
            case MN_BNE: if (!g_cpu.Z) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;
            case MN_BMI: if ( g_cpu.N) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;
            case MN_BPL: if (!g_cpu.N) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;
            case MN_BVC: if (!g_cpu.V) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;
            case MN_BVS: if ( g_cpu.V) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;

            /* ---- Jumps / Calls / Returns (the boundary contract) ---- */
            case MN_JSR: {
                uint16_t target = abs16;
                uint16_t ret = (uint16_t)(ipc + 2);   /* 6502 pushes PC+2 */
                uint8_t call_s = g_cpu.S;
                g_ram[0x100 + g_cpu.S] = (uint8_t)((ret >> 8) & 0xFF); g_cpu.S--;
                g_ram[0x100 + g_cpu.S] = (uint8_t)(ret & 0xFF);        g_cpu.S--;
                if (interp_dispatch_target(target, 0)) {
                    /* Covered: ran natively. A normal RTS restores call_s and
                     * resumes just after this JSR. A non-local native return
                     * can instead pop an interpreted ancestor; preserve that
                     * guest continuation and let the floor check below decide
                     * whether it belongs to this interpreter frame or the
                     * still-live native caller. */
                    if (g_cpu.S == call_s) {
                        /* Normal native RTS pops the JSR operand we pushed
                         * below (ipc+2), so execution resumes at ipc+3. Some
                         * games use JSR helpers that rewrite that return
                         * operand before RTSing (Kirby $D805 skips inline
                         * bank/target bytes). In that case S is still
                         * restored, but the architectural continuation is the
                         * popped RTS operand + 1. */
                        uint16_t ret = (uint16_t)(ipc + 2);
                        if (g_rti_target != 0)
                            next = g_rti_target;
                        else if (g_rts_target != 0 && g_rts_target != ret)
                            next = (uint16_t)(g_rts_target + 1);
                        else
                            next = (uint16_t)(ipc + 3);
                    } else if (g_rti_target != 0) {
                        next = g_rti_target;
                    } else if (g_rts_target != 0) {
                        next = (uint16_t)(g_rts_target + 1);
                    } else {
                        result = make_exit(NES_INTERP_EXIT_NATIVE_ESCAPE,
                                           entry, next, entry_s);
                        goto done;
                    }
                } else {
                    next = target;  /* miss: interpret inline (push stays on 6502 stack) */
                }
                break;
            }
            case MN_JMP: {
                uint16_t target = (e->addr_mode == AM_IND)
                                  ? nes_read16_jmpbug(abs16) : abs16;
                /* A save-state can capture the PC at a ROM's permanent
                 * frame-driver loop (SMB1 $8057 is `JMP $8057`). Safe
                 * handoff normally keeps JMP tails inside the interpreter,
                 * because a returning native tail can discard an interpreted
                 * ancestor. A direct self-loop at the explicit resume entry
                 * has no such ancestor or return path. If that exact address
                 * is generated, hand it back to native execution so a long-
                 * lived game does not consume the per-run interpreter budget
                 * and terminate at the watchdog cap. Misses remain in the
                 * interpreter, including RAM loops and non-entry loops. */
                int resume_self_loop =
                    !stop_on_stack_lift && e->addr_mode == AM_ABS &&
                    entry >= 0x8000 && ipc == entry && target == entry;
                int native_hit = resume_self_loop
                    ? interp_probe_native_target(target)
                    : interp_dispatch_target(target, 1);
                if (native_hit) {
                    /* A native tail target may RTS/RTI into an interpreted
                     * ancestor. Resume it while still below this run's stack
                     * floor; otherwise the floor check returns to native. */
                    if (g_rti_target != 0)
                        next = g_rti_target;
                    else if (g_rts_target != 0)
                        next = (uint16_t)(g_rts_target + 1);
                    else {
                        result = make_exit(NES_INTERP_EXIT_NATIVE_ESCAPE,
                                           entry, next, entry_s);
                        goto done;
                    }
                    break;
                }
                next = target;               /* miss: interpret inline */
                break;
            }
            case MN_RTS: {
                g_cpu.S++; uint8_t lo = g_ram[0x100 + g_cpu.S];
                g_cpu.S++; uint8_t hi = g_ram[0x100 + g_cpu.S];
                g_rts_target = (uint16_t)(((uint16_t)hi << 8) | lo);
                g_rti_target = 0;
                uint16_t ret = (uint16_t)(g_rts_target + 1);
                if (stop_on_stack_lift && g_cpu.S > S_floor) {
                    result = make_exit(NES_INTERP_EXIT_RETURN,
                                       entry, ret, entry_s);
                    goto done;  /* returned to a still-live native caller */
                }
                next = ret;                                        /* nested return */
                break;
            }
            case MN_RTI: {
                g_cpu.S++; uint8_t p = g_ram[0x100 + g_cpu.S];
                g_cpu.N = (p >> 7) & 1; g_cpu.V = (p >> 6) & 1; g_cpu.D = (p >> 3) & 1;
                g_cpu.I = (p >> 2) & 1; g_cpu.Z = (p >> 1) & 1; g_cpu.C = p & 1;
                g_cpu.S++; uint8_t lo = g_ram[0x100 + g_cpu.S];
                g_cpu.S++; uint8_t hi = g_ram[0x100 + g_cpu.S];
                uint16_t ret = (uint16_t)(((uint16_t)hi << 8) | lo);  /* RTI does NOT +1 */
                g_rti_target = ret;
                g_rti_source = ipc;
                g_rti_bank = g_current_bank;
                g_rts_target = 0;
                if (stop_on_stack_lift && g_cpu.S > S_floor) {
                    result = make_exit(NES_INTERP_EXIT_RTI,
                                       entry, ret, entry_s);
                    goto done;
                }
                next = ret;
                break;
            }

            /* ---- BRK / illegal: mirror codegen (BRK hook; illegal = sized skip) ---- */
            case MN_BRK:
                if (g_interp_hw_brk) {
                    /* Hardware BRK: push PC+2 and P (B set), set I, vector through $FFFE.
                     * The handler's RTI pops back to PC+2. Games with inline-data
                     * routines can execute a stray $00 and rely on this. */
                    uint16_t ret = (uint16_t)(ipc + 2);
                    uint8_t p = (uint8_t)((g_cpu.N << 7) | (g_cpu.V << 6) | 0x30 |
                                          (g_cpu.D << 3) | (g_cpu.I << 2) | (g_cpu.Z << 1) | g_cpu.C);
                    g_ram[0x100 + g_cpu.S] = (uint8_t)(ret >> 8); g_cpu.S--;
                    g_ram[0x100 + g_cpu.S] = (uint8_t)(ret & 0xFF); g_cpu.S--;
                    g_ram[0x100 + g_cpu.S] = p; g_cpu.S--;
                    g_cpu.I = 1;
                    next = nes_read16(0xFFFE);
                    break;
                }
                nes_brk_executed(ipc);
                result = make_exit(NES_INTERP_EXIT_BRK,
                                   entry, ipc, entry_s);
                goto done;   /* codegen returns from the enclosing fn at BRK */
            case MN_ILLEGAL:
            default:
                /* code_generator.c treats MN_ILLEGAL as a sized NOP skip — match it. */
                break;
        }

        ipc = next;

        if (max_steps && this_run >= max_steps) {
            result = make_exit(NES_INTERP_EXIT_NATIVE_ESCAPE,
                               entry, ipc, entry_s);
            goto done;
        }

        /* Miss fallback boundary rule: any instruction that lifts S above the
         * entry frame means we've returned to native code. Explicit continuation
         * resumes disable this because their entry may intentionally restore
         * state with PLA/PLP-style stack reads. */
        if (stop_on_stack_lift && g_cpu.S > S_floor) {
            result = make_exit(NES_INTERP_EXIT_STACK_ESCAPE,
                               entry, next, entry_s);
            goto done;
        }
    }

done:
    {   /* NESRECOMP_RUN_TRACE=frame_from: log the start/end of every interpreter run from that frame on */
        static int s_rt = -2; static long s_rt_lines;
        if (s_rt == -2) { const char *e = getenv("NESRECOMP_RUN_TRACE"); s_rt = e ? atoi(e) : -1; }
        if (s_rt >= 0 && (long long)g_frame_count >= s_rt && s_rt_lines < 4000) {
            s_rt_lines++;
            fprintf(stderr, "[run] END f=%llu depth=%d entry=$%04X stop=%d kind=%d entry_s=%02X exit_s=%02X next=$%04X n=%u rti=%04X rts=%04X\n",
                    (unsigned long long)g_frame_count, s_depth, entry, stop_on_stack_lift, (int)result.kind, entry_s, g_cpu.S,
                    result.next_pc, this_run, g_rti_target, g_rts_target);
        }
    }
    if (this_run > s_stats.max_instrs_run) s_stats.max_instrs_run = this_run;
    interp_note_hotspot(entry, entry_bank, this_run);
    s_active_handoff_mode = previous_handoff_mode;
    s_depth--;
    result.exit_s = g_cpu.S;
    s_last_exit = result;
    return result;
}

static NesInterpExit interp_run(uint16_t entry) {
    return interp_run_ex(entry, 1, nes_interp_get_native_handoff_mode(), 0);
}

static int interp_exit_handled(NesInterpExit exit) {
    return exit.kind != NES_INTERP_EXIT_DECLINED;
}

int nes_interp_step_tail(uint16_t addr, int caller_bank) {
    interp_lazy_init();
    static int s_step_trace = -1;
    static unsigned s_step_trace_count = 0;
    if (s_step_trace < 0) {
        const char *e = getenv("NESRECOMP_INTERP_STEP_TRACE");
        s_step_trace = (e && *e && *e != '0') ? 1 : 0;
    }
    unsigned trace_id = s_step_trace_count++;
    if (s_step_trace && trace_id < 128) {
        fprintf(stderr,
                "[InterpStep] #%u enter pc=$%04X op=$%02X bank=%d window=$%04X S=$%02X caller=%d\n",
                trace_id, addr, interp_fetch(addr), g_current_bank,
                g_code_window_base, g_cpu.S, caller_bank);
    }

    /* A Mapper 40 instruction which straddles an 8KB PRG window cannot be
     * emitted safely as native C. Execute exactly that one instruction, then
     * resume through the normal bank-aware tail dispatcher. Interpreting the
     * whole island here can swallow a frame-driving loop before native code
     * regains control. This is intentional execution, not a dispatch miss. */
    NesInterpExit exit =
        interp_run_ex(addr, 0, NES_INTERP_HANDOFF_ISLAND, 1);
    if (s_step_trace && trace_id < 128) {
        fprintf(stderr,
                "[InterpStep] #%u exit kind=%d next=$%04X bank=%d window=$%04X S=$%02X\n",
                trace_id, (int)exit.kind, exit.next_pc, g_current_bank,
                g_code_window_base, g_cpu.S);
    }
    if (!interp_exit_handled(exit))
        return 0;

    if (exit.kind == NES_INTERP_EXIT_NATIVE_ESCAPE && exit.next_pc != 0)
        return call_by_address_tail(exit.next_pc, caller_bank);

    return 1;
}

/* ---- Entry from the generated dispatcher ----
 * Bank-aware form: cpu_addr is the live 6502 target (what the interpreter
 * must execute — it fetches through the live windows), gen_addr/bank are the
 * recompiler-layout coordinates for miss recording ([[extra_func]] advice). */
int nes_interp_dispatch_bank(uint16_t cpu_addr, uint16_t gen_addr, int bank) {
    interp_lazy_init();

    uint16_t addr = cpu_addr;

    /* Per-game manual override still wins (e.g. Zelda SRAM remap). */
    if (game_dispatch_override(addr)) return 1;

    /* Covered-ness probe answer for an in-flight interp_run dispatch. */
    if (s_probe_armed && addr == s_probe_addr) {
        s_probe_armed = 0;
        if (addr >= 0x8000)
            nes_record_dispatch_miss_bank(gen_addr, cpu_addr, bank);
        return 0;   /* "miss" — interp_run will handle the target inline */
    }

    /* CPU RAM and cartridge SRAM code cannot have a generated function. It is
     * intentional dynamic execution rather than static-discovery fallback, so
     * keep it available under fallback=off. Mapper 40 is the exception: its
     * $6000-$7FFF window is PRG ROM and must retain normal strict dispatch. */
    if (addr < 0x2000 ||
        (addr >= 0x6000 && addr < 0x8000 && mapper_get_type() != 40)) {
        if (interp_exit_handled(interp_run(addr)))
            return 1;
        fprintf(stderr, "[Interp] RAM/SRAM entry $%04X could not be interpreted\n", addr);
        return 0;
    }

    if (s_enabled == 1) {
        if (!nes_dispatch_miss_last_target_is_code()) {
            s_stats.policy_traps++;
            nes_dispatch_miss_interp_declined(addr, "non-code dispatch target");
            return 0;
        }
        if (interp_exit_handled(interp_run(addr))) return 1;   /* handled by fallback */
        s_stats.policy_traps++;
        nes_dispatch_miss_interp_declined(addr, s_last_decline_reason);
        return 0;
    }

    nes_record_dispatch_miss_bank(gen_addr, cpu_addr, bank);
    nes_dispatch_miss_apply_policy(addr);
    return 0;
}

/* Decompiled code (nes_decomp.h) hands routines it cannot express as a C function (inline arguments,
 * pulled return addresses, computed dispatch, undecoded targets) to the interpreter, which runs them
 * as an island until control comes back to a point the C code knows:
 *   stop_pc != 0     : the continuation after the routine's inline bytes, with S == stop_s
 *   floor_valid      : the routine (or its dispatch target) returned past the C function's own frame (S > floor_s)
 * Returns 0 if the interpreter declined, 1 if it stopped at stop_pc, 2 if it returned past the frame (floor). */
int nes_interp_run_until(uint16_t entry, uint16_t stop_pc, uint8_t stop_s, int floor_valid, uint8_t floor_s) {
    interp_lazy_init();
    int  o_active = s_ru_active, o_valid = s_ru_floor_valid, o_depth = s_ru_depth;
    uint16_t o_pc = s_ru_pc; uint8_t o_s = s_ru_s, o_floor = s_ru_floor;
    s_ru_active = 1; s_ru_pc = stop_pc; s_ru_s = stop_s; s_ru_floor_valid = floor_valid; s_ru_floor = floor_s;
    s_ru_depth = s_depth + 1;
    s_ru_reason = 0;
    /* The island's RTS/RTI leave their targets in the sidecars the interpreter uses to resume after a native
     * callee; native (C) code never sets them, so an island must not leak them to the interpreter frame below. */
    uint16_t o_rts = g_rts_target, o_rti = g_rti_target, o_rtis = g_rti_source; int o_rtib = g_rti_bank;
    NesInterpExit ex = interp_run_ex(entry, 0, NES_INTERP_HANDOFF_ISLAND, 0);
    /* floor mode = the island is the rest of the C function: its final RTS/RTI is the function's return and its
     * target must reach the interpreter frame below; stop-address mode = the island was only a call inside it. */
    if (!floor_valid) { g_rts_target = o_rts; g_rti_target = o_rti; g_rti_source = o_rtis; g_rti_bank = o_rtib; }
    s_ru_active = o_active; s_ru_pc = o_pc; s_ru_s = o_s; s_ru_floor_valid = o_valid; s_ru_floor = o_floor; s_ru_depth = o_depth;
    if (!interp_exit_handled(ex)) return 0;
    return s_ru_reason ? s_ru_reason : 1;
}

/* Reference execution for differential checks of decompiled code: run the routine at `addr` purely in the
 * interpreter (island mode, no native handoff, ignoring an armed covered-ness probe). */
int nes_interp_run_island(uint16_t addr) {
    interp_lazy_init();
    int armed = s_probe_armed;
    s_probe_armed = 0;
    int ok = interp_exit_handled(interp_run_ex(addr, 1, NES_INTERP_HANDOFF_ISLAND, 0));
    s_probe_armed = armed;
    return ok;
}

/* Legacy entry: cpu==gen address, g_current_bank attribution. */
int nes_interp_dispatch(uint16_t addr) {
    extern int g_current_bank;
    return nes_interp_dispatch_bank(addr, addr, g_current_bank);
}

int nes_interp_force_bank(uint16_t cpu_addr, uint16_t gen_addr, int bank) {
    interp_lazy_init();

    /* A forced wrapper can be reached by the covered-ness probe from an
     * already-running interpreter island. The generated wrapper is void, so
     * the dispatcher cannot propagate a "miss" answer back to the probe.
     * Execute the forced island here so the caller observes the real RTS/RTI
     * sidecar state instead of treating an unpopped JSR as native success. */
    if (s_probe_armed && cpu_addr == s_probe_addr) {
        s_probe_armed = 0;
        return interp_exit_handled(
            interp_run_ex(cpu_addr, 1, NES_INTERP_HANDOFF_ISLAND, 0));
    }

    /* Interrupt handlers may drive the whole game (Castlevania III runs its logic inside NMI): when the native
     * handoff policy allows balanced JSR handoffs (decompiled functions installed), the handler cooperates with
     * them like the main program does; otherwise it stays an island. */
    if (interp_exit_handled(
            interp_run_ex(cpu_addr, 1, (NesInterpHandoffMode)s_native_handoff_mode, 0)))
        return 1;

    char reason[160];
    const char *why = s_last_decline_reason;
    snprintf(reason, sizeof(reason),
             "forced interpreter entry $%04X (generated $%04X bank=%d) could not execute: %s",
             cpu_addr, gen_addr, bank, why);
    fprintf(stderr, "[Interp] %s\n", reason);
    s_stats.policy_traps++;
    nes_write_runtime_fault(reason);
    debug_server_request_pause(reason);
    return 0;
}

int nes_interp_force_generated(uint16_t gen_addr, int bank) {
    uint16_t cpu_addr = gen_addr;
    int mapper = mapper_get_type();
    if ((mapper == 4 || mapper == 40) && gen_addr >= 0x8000)
        cpu_addr = (uint16_t)(g_code_window_base | (gen_addr & 0x1FFF));
    return nes_interp_force_bank(cpu_addr, gen_addr, bank);
}

int nes_interp_force(uint16_t addr) {
    return nes_interp_force_bank(addr, addr, g_current_bank);
}

int nes_interp_interrupt(uint16_t addr) {
    interp_lazy_init();

    /* RAM/SRAM interrupt vectors are intentional code entries, not missed
     * generated functions. Keep dispatch_misses.log reserved for discovery
     * defects while still executing the handler against live memory. */
    if (interp_exit_handled(
            interp_run_ex(addr, 1, (NesInterpHandoffMode)s_native_handoff_mode, 0)))
        return 1;

    fprintf(stderr, "[Interp] RAM/SRAM interrupt vector $%04X could not be interpreted\n", addr);
    return 0;
}

int nes_interp_resume(uint16_t addr) {
    interp_lazy_init();
    /* A restored PC is the continuation of the whole suspended program, not
     * a missed function with a native caller waiting above it. It therefore
     * cooperates with covered native JSRs while interpreter fallbacks nested
     * beneath those native calls still install their own island policy. */
    NesInterpHandoffMode mode =
        (s_native_handoff_mode == NES_INTERP_HANDOFF_LEGACY)
        ? NES_INTERP_HANDOFF_LEGACY : NES_INTERP_HANDOFF_SAFE;
    uint16_t pc = addr;
    for (;;) {
        NesInterpExit exit = interp_run_ex(pc, 0, mode, 0);
        if (exit.kind != NES_INTERP_EXIT_NATIVE_ESCAPE)
            return interp_exit_handled(exit);

        /*
         * A cooperative native call may unwind without an RTS/RTI sidecar.
         * This is not a guest-program exit: generated tail-cycle flattening
         * deliberately discards host C frames while the architectural 6502
         * continuation remains live. Resume from the instruction boundary
         * recorded by the runtime instead of returning out of the permanent
         * save-state driver.
         */
        uint16_t resume_pc = 0;
        int tick_charged = 0;
        if (!runtime_get_savestate_resume(&resume_pc, &tick_charged) ||
            resume_pc == 0) {
            interp_note_decline(pc, exit.next_pc,
                                "native escape had no guest continuation");
            s_last_exit = make_exit(NES_INTERP_EXIT_DECLINED, pc,
                                    exit.next_pc, exit.entry_s);
            return 0;
        }

        s_stats.native_resume_reentries++;
        runtime_prepare_guest_resume(resume_pc, tick_charged);
        pc = resume_pc;
    }
}

void nes_interp_reset_context(void) {
    s_probe_armed = 0;
    s_probe_addr = 0;
    s_depth = 0;
    s_active_handoff_mode = -1;
    s_last_exit = make_exit(NES_INTERP_EXIT_DECLINED, 0, 0, g_cpu.S);
}
