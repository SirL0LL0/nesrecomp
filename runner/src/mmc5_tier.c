/*
 * mmc5_tier.c - nesrecomp: esecuzione dei giochi MMC5 senza il modello di banchi del recompiler.
 *
 * Sostituisce il codice che NESRecomp.exe genererebbe (func_RESET / func_NMI / func_IRQ / call_by_address).
 * Ogni chiamata dinamica passa all'interprete 6502 del runner, che legge il codice attraverso la mappatura
 * MMC5 viva (mapper_peek_prg): qualsiasi banco, finestra o WRAM funziona. Sopra l'interprete, generati da
 * tools/mmc5/ (nesrecomp_mmc5_tier() in runner.cmake):
 *   - blocchi base tradotti in C  (NESRECOMP_MMC5_BLOCKS)   nes_mmc5_blocks_init()
 *   - funzioni C decompilate      (NESRECOMP_MMC5_DECOMP)   nes_mmc5_decomp_init()
 * Ogni blocco/funzione porta l'hash dei propri byte e viene usato solo se coincide con la ROM caricata
 * (le ROM tradotte/patchate restano corrette: il resto va all'interprete).
 *
 * Variabili d'ambiente utili: NESRECOMP_BLOCKS=0, NESRECOMP_DECOMP=0 (esclusione), NESRECOMP_INTERP_HIST=N,
 * NESRECOMP_BOUNDARY_LOG=file[,max], NESRECOMP_DECOMP_VERIFY=N, NESRECOMP_STEP_TRACE=pc,pc, NESRECOMP_RU_DEBUG=1.
 */
#include "nes_runtime.h"
#include "interp.h"
#include "mapper.h"
#include <stdio.h>
#include <stdlib.h>

int g_recomp_push_all_jsr = 1;

#ifdef NESRECOMP_MMC5_DECOMP
#include "nes_decomp.h"
/* Funzione decompilata per l'indirizzo (secondo la mappatura MMC5 live) oppure interprete. */
#include "savestate.h"
#include <string.h>
int nes_interp_run_island(uint16_t addr);

/* NESRECOMP_DECOMP_VERIFY=N: le prime N chiamate di ogni funzione decompilata vengono eseguite due volte dallo
 * stesso stato (C e interprete puro) e confrontate: registri, RAM e savestate completo. Lo stato che prosegue
 * e' quello dell'interprete. Serve a trovare le differenze di un decompilatore. */
static int s_verify_n = -1;
static uint8_t *s_vcount = NULL;          /* [unit<<13 | off] contatore per funzione (unita' 64 x 8K) */
static unsigned long s_vchecked = 0, s_vbad = 0;

extern void (*g_boundary_trace_fn)(uint16_t pc, int cycles);
/* NESRECOMP_BOUNDARY_LOG=file[,max]: registra (pc,A,X,Y,S) a ogni istruzione: confronto tra esecuzioni (interprete vs decompilato) */
static FILE *s_blog = NULL; static unsigned long s_blog_n = 0, s_blog_max = 0;
static void blog_rec(uint16_t pc, int cy) {
    (void)cy;
    if (s_blog_n >= s_blog_max) return;
    unsigned char rec[12] = { (unsigned char)pc, (unsigned char)(pc >> 8), g_cpu.A, g_cpu.X, g_cpu.Y, g_cpu.S,
                             g_ram[0x100 + (uint8_t)(g_cpu.S + 1)], g_ram[0x100 + (uint8_t)(g_cpu.S + 2)],
                             (unsigned char)g_mmc5_win_bank8k[0], (unsigned char)g_mmc5_win_bank8k[1],
                             (unsigned char)g_mmc5_win_bank8k[2], (unsigned char)g_mmc5_win_bank8k[3] };
    fwrite(rec, 1, 12, s_blog);
    s_blog_n++;
    if ((s_blog_n & 0xFFF) == 0) fflush(s_blog);
}
static void blog_close(void) { if (s_blog) { fclose(s_blog); s_blog = NULL; } }
static void blog_init(void) {
    const char *e = getenv("NESRECOMP_BOUNDARY_LOG");
    if (!e || !*e) return;
    char path[512]; snprintf(path, sizeof path, "%s", e);
    s_blog_max = 30000000UL;
    char *comma = strchr(path, 44);
    if (comma) { *comma = 0; s_blog_max = strtoul(comma + 1, NULL, 10); }
    s_blog = fopen(path, "wb");
    if (s_blog) { g_boundary_trace_fn = blog_rec; atexit(blog_close); }
}
#define VTR_MAX 200000
static uint16_t s_vt_pc[2][VTR_MAX]; static int s_vt_cy[2][VTR_MAX]; static int s_vt_n[2], s_vt_cur;
static void vt_rec(uint16_t pc, int cy) { if (s_vt_n[s_vt_cur] < VTR_MAX) { s_vt_pc[s_vt_cur][s_vt_n[s_vt_cur]] = pc; s_vt_cy[s_vt_cur][s_vt_n[s_vt_cur]] = cy; s_vt_n[s_vt_cur]++; } }
static int s_in_verify = 0;
static int verify_call(uint16_t addr, NesDecompFn f) {
    s_in_verify = 1;
    const char *T1 = "mmc5_verify_a.sav", *T2 = "mmc5_verify_b.sav", *T3 = "mmc5_verify_c.sav";
    CPU6502State c0 = g_cpu;
    savestate_save(T1);
    s_vt_n[0] = s_vt_n[1] = 0; s_vt_cur = 0; g_boundary_trace_fn = vt_rec;
    f();
    g_boundary_trace_fn = NULL;
    CPU6502State ca = g_cpu; uint8_t ra[2048]; memcpy(ra, g_ram, 2048);
    savestate_save(T2);
    savestate_load(T1);
    g_cpu = c0;                       /* il savestate e' a confine di frame: la CPU a meta' routine la ripristino a mano */
    s_vt_cur = 1; g_boundary_trace_fn = vt_rec;
    nes_interp_run_island(addr);
    g_boundary_trace_fn = NULL;
    {   int m = s_vt_n[0] < s_vt_n[1] ? s_vt_n[0] : s_vt_n[1], k = 0;
        while (k < m && s_vt_pc[0][k] == s_vt_pc[1][k] && s_vt_cy[0][k] == s_vt_cy[1][k]) k++;
        if (k < m || s_vt_n[0] != s_vt_n[1]) {
            fprintf(stderr, "[verify] $%04X sequenza istruzioni: nativo %d, interp %d, prima differenza all'indice %d\n", addr, s_vt_n[0], s_vt_n[1], k);
            for (int q = (k > 3 ? k - 3 : 0); q < k + 4; q++)
                fprintf(stderr, "[verify]   #%d nativo %s%04X/%d  interp %s%04X/%d\n", q,
                        q < s_vt_n[0] ? "" : "--", q < s_vt_n[0] ? s_vt_pc[0][q] : 0, q < s_vt_n[0] ? s_vt_cy[0][q] : 0,
                        q < s_vt_n[1] ? "" : "--", q < s_vt_n[1] ? s_vt_pc[1][q] : 0, q < s_vt_n[1] ? s_vt_cy[1][q] : 0);
        }
    }
    { NesInterpExit ex; nes_interp_get_last_exit(&ex); if (getenv("NESRECOMP_VERIFY_DEBUG")) fprintf(stderr, "[verify] isola $%04X: exit kind=%d entry=%04X next=%04X entry_s=%02X exit_s=%02X\n", addr, (int)ex.kind, ex.entry_pc, ex.next_pc, ex.entry_s, ex.exit_s); }
    CPU6502State cb = g_cpu; uint8_t rb[2048]; memcpy(rb, g_ram, 2048);
    savestate_save(T3);
    s_vchecked++;
    int bad = 0;
    if (memcmp(&ca, &cb, sizeof ca)) {
        fprintf(stderr, "[verify] $%04X (win %d,%d,%d,%d) CPU: nativo A=%02X X=%02X Y=%02X S=%02X N%d Z%d C%d V%d | interp A=%02X X=%02X Y=%02X S=%02X N%d Z%d C%d V%d\n",
                addr, g_mmc5_win_bank8k[0], g_mmc5_win_bank8k[1], g_mmc5_win_bank8k[2], g_mmc5_win_bank8k[3],
                ca.A, ca.X, ca.Y, ca.S, ca.N, ca.Z, ca.C, ca.V, cb.A, cb.X, cb.Y, cb.S, cb.N, cb.Z, cb.C, cb.V);
        bad = 1;
    }
    if (memcmp(ra, rb, 2048)) {
        int n = 0;
        for (int i = 0; i < 2048 && n < 6; i++)
            if (ra[i] != rb[i]) { fprintf(stderr, "[verify] $%04X RAM[%04X]: nativo %02X interp %02X\n", addr, i, ra[i], rb[i]); n++; }
        bad = 1;
    }
    if (!bad) {
        FILE *fa = fopen(T2, "rb"), *fb = fopen(T3, "rb");
        if (fa && fb) {
            int ca2, cb2; long pos = 0; int nd = 0;
            for (;;) {
                ca2 = fgetc(fa); cb2 = fgetc(fb);
                if (ca2 == EOF || cb2 == EOF) break;
                if (ca2 != cb2) { if (nd < 12) fprintf(stderr, "[verify] $%04X savestate byte %ld: nativo %02X interp %02X\n", addr, pos, ca2, cb2); nd++; bad = 1; }
                pos++;
            }
            if (nd) fprintf(stderr, "[verify] $%04X savestate: %d byte diversi\n", addr, nd);
        }
        if (fa) fclose(fa);
        if (fb) fclose(fb);
    }
    if (bad) s_vbad++;
    s_in_verify = 0;
    return 1;
}

int call_by_address(uint16_t addr) {
    NesDecompFn f = nes_decomp_lookup(addr);
    if (!f) return nes_interp_dispatch(addr);
    if (s_verify_n < 0) { const char *e = getenv("NESRECOMP_DECOMP_VERIFY"); s_verify_n = e ? atoi(e) : 0; }
    if (s_verify_n > 0 && !s_in_verify) {
        if (!s_vcount) s_vcount = (uint8_t *)calloc((size_t)256 * 8192, 1);
        int unit = g_mmc5_win_bank8k[(addr - 0x8000) >> 13];
        uint8_t *c = &s_vcount[((unsigned)unit << 13) | (addr & 0x1FFF)];
        if (*c < s_verify_n && *c < 250) {
            (*c)++;
            return verify_call(addr, f);
        }
    }
    f();
    return 1;
}
#else
int call_by_address(uint16_t addr)                      { return nes_interp_dispatch(addr); }
#endif
int call_by_address_cb(uint16_t addr, int caller_bank)  { (void)caller_bank; return call_by_address(addr); }

#include <stdio.h>
#include <stdlib.h>
extern int g_interp_hw_brk;
#ifdef NESRECOMP_MMC5_DECOMP
static void verify_report(void);
#endif
void func_RESET(void) {
    /* BRK come sull'hardware: alcune routine con dati inline ne dipendono.
     * NESRECOMP_NO_HW_BRK=1 lo disattiva (solo per confronti A/B). */
    g_interp_hw_brk = (getenv("NESRECOMP_NO_HW_BRK") || getenv("JB_NO_HW_BRK")) ? 0 : 1;
    /* Un NMI che dura piu' di un frame viene interrotto dal successivo, come sull'hardware (Castlevania III lo prevede:
     * $1B distingue il percorso rientrante). Il vecchio comportamento (saltare il gestore e scrivere 1 in $1A/$20, tarato
     * su altri giochi) corrompe la RAM del gioco. NESRECOMP_LEGACY_NESTED_NMI=1 lo ripristina per confronti. */
    if (!getenv("NESRECOMP_LEGACY_NESTED_NMI")) g_nested_nmi_policy = NESTED_NMI_RUN_HANDLER;
#ifdef NESRECOMP_MMC5_BLOCKS
    { extern void nes_mmc5_blocks_init(void); nes_mmc5_blocks_init(); }
#endif
#ifdef NESRECOMP_MMC5_DECOMP
    { extern void nes_mmc5_decomp_init(void); nes_mmc5_decomp_init();
      blog_init();
      atexit(verify_report);
      /* le funzioni decompilate si chiamano da JSR bilanciate: 'safe' = handoff per JSR, JMP resta all'interprete */
      if (!getenv("NESRECOMP_INTERP_NATIVE_HANDOFF")) nes_interp_set_native_handoff_mode(NES_INTERP_HANDOFF_SAFE); }
#endif
    nes_interp_resume(nes_read16(0xFFFC));
    /* Non dovrebbe mai tornare: se succede, dice perche' (kind: 0 declined, 1 return, 2 rti, 3 native, 4 stack, 5 brk). */
    NesInterpExit ex;
    nes_interp_get_last_exit(&ex);
    fprintf(stderr, "[mmc5] interprete uscito dal ciclo principale: kind=%d entry=$%04X next=$%04X S=$%02X->$%02X frame=%llu\n",
            (int)ex.kind, ex.entry_pc, ex.next_pc, ex.entry_s, ex.exit_s, (unsigned long long)g_frame_count);
}
#ifdef NESRECOMP_MMC5_DECOMP
static void verify_report(void) { if (s_vchecked) fprintf(stderr, "[verify] chiamate confrontate: %lu, con differenze: %lu\n", s_vchecked, s_vbad); }
#endif
void func_NMI(void)   { nes_interp_force(nes_read16(0xFFFA)); }
void func_IRQ(void)   { nes_interp_force(nes_read16(0xFFFE)); }


/* Stato che il codice generato definirebbe; l'interprete lo usa per i confini RTS/RTI. */
uint16_t g_rts_target = 0;
uint16_t g_rti_target = 0;
uint16_t g_rti_source = 0;
int      g_rti_bank   = -1;
