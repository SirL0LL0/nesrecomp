/*
 * nes_decomp.h - runtime for C produced by tools/decompile.py (6502 -> readable C, MMC5 / banked mappers).
 *
 * A decompiled routine is an ordinary C function `void fUU_AAAA(void)` operating on the CPU state
 * (rA/rX/rY, g_cpu flags, g_ram). The calling convention is the framework's push_all_jsr one, so decompiled
 * functions and the interpreter can call each other freely:
 *   JSR  : push the 6502 return address, call, check the stack came back balanced
 *   RTS  : pop the 2-byte return address (S += 2) and return
 * Anything a C function cannot express (inline arguments after a JSR, pulled return addresses, computed
 * dispatch, undecoded targets) is run by the interpreter as an island until control returns to a point
 * the C code knows statically (see nes_interp_run_until in interp.c).
 */
#ifndef NES_DECOMP_H
#define NES_DECOMP_H

#include "nes_runtime.h"
#include "nes_blocks.h"

typedef void (*NesDecompFn)(void);
typedef struct {
    uint8_t     unit;
    uint8_t     win;     /* CPU window index the function was generated for */
    uint16_t    off;     /* offset of the entry inside the unit */
    NesDecompFn fn;
    uint32_t    hash;    /* FNV-1a over the instruction bytes of the function (ranges below), in the source ROM */
    const uint16_t *ranges;  /* (offset, length) pairs inside the unit, terminated by length 0 */
} NesDecompEntry;

/* registry lookup by live CPU address (uses the MMC5 window mapping); NULL if not a decompiled entry */
NesDecompFn nes_decomp_lookup(uint16_t addr);
void        nes_decomp_install(const NesDecompEntry *tab, int n, uint8_t *valid);
extern uint8_t mmc5_dec_valid[];   /* per function: its code matches the loaded ROM (else the body defers to the interpreter) */

int      nes_interp_run_until(uint16_t entry, uint16_t stop_pc, uint8_t stop_s, int floor_valid, uint8_t floor_s);
int      call_by_address(uint16_t addr);
uint64_t runtime_get_interrupt_epoch(void);
extern uint16_t g_rts_target, g_rti_target, g_rti_source;
extern int      g_rti_bank;

#define rA g_cpu.A
#define rX g_cpu.X
#define rY g_cpu.Y

#define PUSH16(v) do { g_ram[0x100 + g_cpu.S] = (uint8_t)((v) >> 8); g_cpu.S--; \
                       g_ram[0x100 + g_cpu.S] = (uint8_t)(v);        g_cpu.S--; } while (0)

/* JSR to a decompiled function */
#define JSR(fn, ret) do { uint8_t _cbs = g_cpu.S; uint64_t _ie = runtime_get_interrupt_epoch(); \
                          g_rti_target = 0; g_rti_source = 0; g_rti_bank = -1; PUSH16(ret); \
                          fn(); if (!nes_jsr_stack_ok_after_call(_cbs, _ie)) return; } while (0)
/* JSR to an address only known at run time (other window, switchable $C000 unit). A decompiled function is a
 * plain call; anything else runs in the interpreter, which may find the callee rewrote its return address
 * (inline arguments): the island then simply continues with the rest of this function as 6502 code until the
 * function returns past its frame (result 2) - or stops at the normal continuation (result 1). */
#define JSR_DYN(addr, ret) do { uint8_t _sb = g_cpu.S; NesDecompFn _df = nes_decomp_lookup(addr); PUSH16(ret); \
                                if (_df) { _df(); } \
                                else if (nes_interp_run_until(addr, (uint16_t)((ret) + 1), _sb, 1, _s0) == 2) return; } while (0)
/* JSR to a routine that reads inline bytes after the call: the interpreter runs it and stops at `cont`. Dispatch
 * routines (jump table after the JSR) may instead RTS past this function's frame: then the run ends by the floor
 * (S above the entry S) and this function returns, exactly like a JSR_DYN whose callee pops our return address. */
#define JSR_INLINE(addr, ret, cont) do { uint8_t _sb = g_cpu.S; PUSH16(ret); \
                                         if (nes_interp_run_until(addr, cont, _sb, 1, _s0) == 2) return; } while (0)
/* JSR to an inline dispatch table: control never comes back here, the routine returns past this frame */
#define JSR_TABLE(addr, ret) do { PUSH16(ret); nes_interp_run_until(addr, 0, 0, 1, _s0); return; } while (0)
/* first statement of every decompiled function: if its code differs in the running ROM, let the interpreter run it */
#define DEC_GUARD(id, pc) do { if (!mmc5_dec_valid[id]) { nes_interp_run_until(pc, 0, 0, 1, g_cpu.S); return; } } while (0)
/* RTS pops the return address for real and publishes it (g_rts_target), so an interpreter frame that called us
 * natively can resume at the popped address + 1 even when the routine rewrote its return address. */
#define RTS() do { uint8_t _lo, _hi; g_cpu.S++; _lo = g_ram[0x100 + g_cpu.S]; g_cpu.S++; _hi = g_ram[0x100 + g_cpu.S]; \
                   g_rts_target = (uint16_t)(((uint16_t)_hi << 8) | _lo); g_rti_target = 0; return; } while (0)
/* JMP out of the function */
#define JMP_FN(fn) do { fn(); return; } while (0)
#define JMP_DYN(addr) do { NesDecompFn _df = nes_decomp_lookup(addr); \
                           if (_df) _df(); else nes_interp_run_until(addr, 0, 0, 1, _s0); return; } while (0)
#define JMP_IND(a) do { uint16_t _t = nes_read16_jmpbug(a); NesDecompFn _df = nes_decomp_lookup(_t); \
                        if (_df) _df(); else nes_interp_run_until(_t, 0, 0, 1, _s0); return; } while (0)
/* leave the decompiled world: the interpreter finishes this function (undecoded code, RTI, BRK, ...) */
#define ESCAPE(pc) do { nes_interp_run_until(pc, 0, 0, 1, _s0); return; } while (0)

#endif /* NES_DECOMP_H */
