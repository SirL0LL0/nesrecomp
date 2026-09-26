/*
 * nes_blocks.h - translated basic blocks for the interpreter tier (MMC5 / any banked mapper).
 *
 * The static recompiler assumes 16KB banks + a fixed last bank, which MMC5 (four 8KB windows,
 * WRAM-mappable, switchable at run time) does not fit. Instead of one C function per 6502 routine,
 * tools/mmc5_blocks.py emits one C function per *basic block*, keyed by (8KB PRG unit, offset).
 * The interpreter (interp.c) looks up the block for the live PC through the live window mapping
 * (g_mmc5_win_bank8k), runs it natively and keeps owning JSR/JMP/RTS/RTI/BRK and the S-floor
 * boundary contract - so a translated block is exactly what the interpreter would have done,
 * only without decode/dispatch. Anything without a block (new code, RAM code) still interprets.
 *
 * A block ends BEFORE a control instruction (JSR/JMP/RTS/RTI/BRK), AFTER a conditional branch, and
 * AFTER PLA/PLP/TXS (so the interpreter's "stack lifted above the entry frame" check still happens
 * at the same instruction). It returns the next 6502 PC.
 */
#ifndef NES_BLOCKS_H
#define NES_BLOCKS_H

#include "nes_runtime.h"

typedef uint16_t (*NesBlockFn)(void);

typedef struct {
    uint8_t    unit;    /* 8KB PRG unit */
    uint8_t    win;     /* CPU window index the block was generated for ((pc-$8000)>>13) */
    uint16_t   off;     /* offset inside the unit */
    uint16_t   ninsn;   /* instructions in the block */
    NesBlockFn fn;
    uint16_t   nbytes;  /* code bytes covered, starting at `off` */
    uint32_t   hash;    /* FNV-1a of those bytes in the ROM the block was generated from */
} NesBlockEntry;

/* FNV-1a, used to check generated code against the ROM that is actually running (translated ROMs patch code). */
static inline uint32_t nes_fnv1a(const uint8_t *p, uint32_t n, uint32_t h) {
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
#define NES_FNV_INIT 2166136261u

/* Control transfers (JSR/JMP/RTS/RTI/BRK) the executor performs itself: the generator lists each one with its bytes, so the
 * executor does not have to read and decode ROM to run them. Checked against the loaded ROM at install time. */
typedef struct { uint8_t unit, win; uint16_t off; uint8_t op, a, b; } NesCtlEntry;
void nes_blocks_install_ctl(const NesCtlEntry *tab, int n);

/* codebits: nunits * 1024 bytes; bit set = an instruction starts there in the translated code set. */
void nes_blocks_install(const NesBlockEntry *tab, int n, const uint8_t *codebits, int nunits);

/* Per-instruction bookkeeping the interpreter does (NMI sampling, coverage, stats). */
void nes_interp_step_hook(uint16_t pc, uint8_t opcode);

/* ---- flag / ALU helpers: identical to interp.c ---- */
#define NB_NZ(v) do { g_cpu.N = ((uint8_t)(v) >> 7) & 1; g_cpu.Z = ((uint8_t)(v) == 0) ? 1 : 0; } while (0)
#define NB_RD(a) nes_read(a)
#define NB_ADC(m_) do { uint8_t m = (m_); uint16_t r = (uint16_t)(g_cpu.A + m + g_cpu.C); \
    g_cpu.C = (r > 0xFF) ? 1 : 0; g_cpu.N = (r >> 7) & 1; g_cpu.Z = ((r & 0xFF) == 0) ? 1 : 0; \
    g_cpu.V = (~((g_cpu.A) ^ (m)) & ((g_cpu.A) ^ r) & 0x80) ? 1 : 0; g_cpu.A = (uint8_t)(r & 0xFF); } while (0)
#define NB_SBC(m_) do { uint8_t m = (m_); int16_t r = (int16_t)(g_cpu.A - m - (1 - g_cpu.C)); \
    g_cpu.C = (r >= 0) ? 1 : 0; g_cpu.N = ((r & 0xFF) >> 7); g_cpu.Z = ((r & 0xFF) == 0) ? 1 : 0; \
    g_cpu.V = (((g_cpu.A) ^ (m)) & ((g_cpu.A) ^ r) & 0x80) ? 1 : 0; g_cpu.A = (uint8_t)(r & 0xFF); } while (0)
#define NB_CMP(reg, m_) do { uint8_t m = (m_); int r = (reg) - m; g_cpu.C = ((reg) >= m) ? 1 : 0; NB_NZ(r & 0xFF); } while (0)
#define NB_BIT(m_) do { uint8_t m = (m_); g_cpu.Z = (g_cpu.A & m) ? 0 : 1; g_cpu.N = (m >> 7) & 1; g_cpu.V = (m >> 6) & 1; } while (0)

#define NB_ASL_A() do { g_cpu.C = (g_cpu.A >> 7) & 1; g_cpu.A = (uint8_t)(g_cpu.A << 1); NB_NZ(g_cpu.A); } while (0)
#define NB_LSR_A() do { g_cpu.C = g_cpu.A & 1; g_cpu.A >>= 1; NB_NZ(g_cpu.A); } while (0)
#define NB_ROL_A() do { uint8_t c = g_cpu.C; g_cpu.C = (g_cpu.A >> 7) & 1; g_cpu.A = (uint8_t)((g_cpu.A << 1) | c); NB_NZ(g_cpu.A); } while (0)
#define NB_ROR_A() do { uint8_t c = g_cpu.C; g_cpu.C = g_cpu.A & 1; g_cpu.A = (uint8_t)((g_cpu.A >> 1) | (c << 7)); NB_NZ(g_cpu.A); } while (0)
#define NB_ASL_M(ea) do { uint16_t a = (ea); uint8_t v = nes_read(a); g_cpu.C = (v >> 7) & 1; v = (uint8_t)(v << 1); nes_write(a, v); NB_NZ(v); } while (0)
#define NB_LSR_M(ea) do { uint16_t a = (ea); uint8_t v = nes_read(a); g_cpu.C = v & 1; v >>= 1; nes_write(a, v); NB_NZ(v); } while (0)
#define NB_ROL_M(ea) do { uint16_t a = (ea); uint8_t v = nes_read(a); uint8_t c = g_cpu.C; g_cpu.C = (v >> 7) & 1; v = (uint8_t)((v << 1) | c); nes_write(a, v); NB_NZ(v); } while (0)
#define NB_ROR_M(ea) do { uint16_t a = (ea); uint8_t v = nes_read(a); uint8_t c = g_cpu.C; g_cpu.C = v & 1; v = (uint8_t)((v >> 1) | (c << 7)); nes_write(a, v); NB_NZ(v); } while (0)
#define NB_INC_M(ea) do { uint16_t a = (ea); uint8_t v = (uint8_t)(nes_read(a) + 1); nes_write(a, v); NB_NZ(v); } while (0)
#define NB_DEC_M(ea) do { uint16_t a = (ea); uint8_t v = (uint8_t)(nes_read(a) - 1); nes_write(a, v); NB_NZ(v); } while (0)

#define NB_PHA() do { g_ram[0x100 + g_cpu.S] = g_cpu.A; g_cpu.S--; } while (0)
#define NB_PLA() do { g_cpu.S++; g_cpu.A = g_ram[0x100 + g_cpu.S]; NB_NZ(g_cpu.A); } while (0)
#define NB_PHP() do { uint8_t p = (uint8_t)((g_cpu.N << 7) | (g_cpu.V << 6) | 0x30 | (g_cpu.D << 3) | (g_cpu.I << 2) | (g_cpu.Z << 1) | g_cpu.C); \
    g_ram[0x100 + g_cpu.S] = p; g_cpu.S--; } while (0)
#define NB_PLP() do { g_cpu.S++; uint8_t p = g_ram[0x100 + g_cpu.S]; \
    g_cpu.N = (p >> 7) & 1; g_cpu.V = (p >> 6) & 1; g_cpu.D = (p >> 3) & 1; \
    g_cpu.I = (p >> 2) & 1; g_cpu.Z = (p >> 1) & 1; g_cpu.C = p & 1; } while (0)

#define NB_STEP(pc, op) nes_interp_step_hook((uint16_t)(pc), (uint8_t)(op))

#endif /* NES_BLOCKS_H */
