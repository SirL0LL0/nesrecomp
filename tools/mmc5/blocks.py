#!/usr/bin/env python3
"""Traduce in C tutto il codice noto di Just Breed (MMC5), a blocchi base per unita' PRG da 8K.

  python blocks.py ROM.nes OUTDIR [cov1.bin cov2.bin ...]

Il recompiler standard assume banchi da 16K + ultimo banco fisso: MMC5 (4 finestre da 8K, WRAM, cambio
a run time) non ci sta. Qui ogni blocco base diventa una funzione C `uint16_t mmc5_b_UU_AAAA(void)` che
esegue le istruzioni (stesse macro dell'interprete: nesrecomp/runner/include/nes_blocks.h) e ritorna
il prossimo PC. L'interprete del runner cerca il blocco per (unita' mappata nella finestra live, offset),
lo esegue e continua a gestire JSR/JMP/RTS/RTI/BRK e il contratto dello stack.

Codice trovato = analisi statica (disasm.py) + copertura + analysis/seeds.txt.
Il file *_miss.txt scritto da NESRECOMP_BLOCK_MISS_FILE ("UU OFFSET") si puo' riaggiungere a seeds.txt.
"""
import os, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import disasm

UNIT = disasm.UNIT
OPS = disasm.OPS
LEN = disasm.LEN
CONTROL = ("JSR", "JMP", "RTS", "RTI", "BRK")
BRANCH = {"BCC": "!g_cpu.C", "BCS": "g_cpu.C", "BEQ": "g_cpu.Z", "BNE": "!g_cpu.Z",
          "BMI": "g_cpu.N", "BPL": "!g_cpu.N", "BVC": "!g_cpu.V", "BVS": "g_cpu.V"}
STACK_LIFT = ("PLA", "PLP", "TXS")   # il blocco finisce qui: l'interprete controlla il pavimento dello stack


def fnv(data, h=2166136261):
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def ea(mode, ops):
    a = ops[0] if len(ops) == 1 else (ops[0] | (ops[1] << 8)) if ops else 0
    return {
        "zpg": "0x%02X" % a,
        "zpx": "(uint8_t)(%d+g_cpu.X)" % a,
        "zpy": "(uint8_t)(%d+g_cpu.Y)" % a,
        "abs": "0x%04X" % a,
        "abx": "(uint16_t)(%d+g_cpu.X)" % a,
        "aby": "(uint16_t)(%d+g_cpu.Y)" % a,
        "inx": "nes_read16zp((uint8_t)(%d+g_cpu.X))" % a,
        "iny": "(uint16_t)(nes_read16zp(%d)+g_cpu.Y)" % a,
    }[mode]


def rd(mode, ops):
    return "0x%02X" % ops[0] if mode == "imm" else "nes_read(%s)" % ea(mode, ops)


def emit_insn(name, mode, ops):
    """Corpo C di un'istruzione non di controllo (specchio di interp.c)."""
    n = name
    if n == "LDA": return "g_cpu.A = %s; NB_NZ(g_cpu.A);" % rd(mode, ops)
    if n == "LDX": return "g_cpu.X = %s; NB_NZ(g_cpu.X);" % rd(mode, ops)
    if n == "LDY": return "g_cpu.Y = %s; NB_NZ(g_cpu.Y);" % rd(mode, ops)
    if n == "STA": return "nes_write(%s, g_cpu.A);" % ea(mode, ops)
    if n == "STX": return "nes_write(%s, g_cpu.X);" % ea(mode, ops)
    if n == "STY": return "nes_write(%s, g_cpu.Y);" % ea(mode, ops)
    simple = {
        "TAX": "g_cpu.X = g_cpu.A; NB_NZ(g_cpu.X);", "TAY": "g_cpu.Y = g_cpu.A; NB_NZ(g_cpu.Y);",
        "TXA": "g_cpu.A = g_cpu.X; NB_NZ(g_cpu.A);", "TYA": "g_cpu.A = g_cpu.Y; NB_NZ(g_cpu.A);",
        "TSX": "g_cpu.X = g_cpu.S; NB_NZ(g_cpu.X);", "TXS": "g_cpu.S = g_cpu.X;",
        "PHA": "NB_PHA();", "PLA": "NB_PLA();", "PHP": "NB_PHP();", "PLP": "NB_PLP();",
        "INX": "g_cpu.X = (uint8_t)(g_cpu.X + 1); NB_NZ(g_cpu.X);", "DEX": "g_cpu.X = (uint8_t)(g_cpu.X - 1); NB_NZ(g_cpu.X);",
        "INY": "g_cpu.Y = (uint8_t)(g_cpu.Y + 1); NB_NZ(g_cpu.Y);", "DEY": "g_cpu.Y = (uint8_t)(g_cpu.Y - 1); NB_NZ(g_cpu.Y);",
        "CLC": "g_cpu.C = 0;", "SEC": "g_cpu.C = 1;", "CLD": "g_cpu.D = 0;", "SED": "g_cpu.D = 1;",
        "CLI": "g_cpu.I = 0;", "SEI": "g_cpu.I = 1;", "CLV": "g_cpu.V = 0;", "NOP": "",
    }
    if n in simple: return simple[n]
    if n == "ADC": return "NB_ADC(%s);" % rd(mode, ops)
    if n == "SBC": return "NB_SBC(%s);" % rd(mode, ops)
    if n == "AND": return "g_cpu.A &= %s; NB_NZ(g_cpu.A);" % rd(mode, ops)
    if n == "ORA": return "g_cpu.A |= %s; NB_NZ(g_cpu.A);" % rd(mode, ops)
    if n == "EOR": return "g_cpu.A ^= %s; NB_NZ(g_cpu.A);" % rd(mode, ops)
    if n == "CMP": return "NB_CMP(g_cpu.A, %s);" % rd(mode, ops)
    if n == "CPX": return "NB_CMP(g_cpu.X, %s);" % rd(mode, ops)
    if n == "CPY": return "NB_CMP(g_cpu.Y, %s);" % rd(mode, ops)
    if n == "BIT": return "NB_BIT(%s);" % rd(mode, ops)
    if n in ("ASL", "LSR", "ROL", "ROR"):
        return "NB_%s_A();" % n if mode == "acc" else "NB_%s_M(%s);" % (n, ea(mode, ops))
    if n in ("INC", "DEC"): return "NB_%s_M(%s);" % (n, ea(mode, ops))
    return None


def build(rom_path, outdir, covs):
    rom = disasm.Rom(rom_path, covs)
    extra = disasm.load_seeds(rom)
    code, targets, ext = disasm.analyze(rom, extra)
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        if f.startswith("mmc5_blocks_") and f.endswith(".c"):
            os.remove(os.path.join(outdir, f))

    table = []      # (unit, win, off, ninsn, name)
    ctl = []        # (unit, win, off, opcode, op1, op2): every JSR/JMP/RTS/RTI/BRK the interpreter has to execute
    codebits = bytearray(rom.nunits * 1024)
    n_insn = 0
    for u in range(rom.nunits):
        cs = code[u]
        if not cs:
            continue
        base = rom.base[u]
        win = (base - 0x8000) >> 13
        d = rom.prg[u * UNIT:(u + 1) * UNIT]
        offs = sorted(cs)
        for o in offs:
            codebits[u * 1024 + (o >> 3)] |= 1 << (o & 7)
        leaders = set(targets[u])
        for o in offs:                       # ingressi visti in gioco (arrivo non sequenziale): sempre inizio blocco
            if rom.ent[u * UNIT + o]:
                leaders.add(o)
        prev_end, prev_term = None, True
        for o in offs:
            name, _ = OPS[d[o]]
            if prev_end != o or prev_term:
                leaders.add(o)
            if prev_end is not None and prev_end == o and prevname == "JSR":
                leaders.add(o)
            if name in BRANCH or name in STACK_LIFT:      # the block returns here: the next instruction must be an entry too,
                leaders.add(o + cs[o])                    # otherwise the interpreter runs it until the next leader
            prev_end = o + cs[o]
            prev_term = name in ("RTS", "RTI", "JMP")
            prevname = name
        for o in offs:
            if OPS[d[o]][0] in CONTROL:
                ops3 = list(d[o + 1:o + cs[o]]) + [0, 0]
                ctl.append((u, win, o, d[o], ops3[0], ops3[1]))
        lines = ['#include "nes_blocks.h"', ""]
        for L in sorted(leaders):
            if L not in cs:
                continue
            body, o, cnt = [], L, 0
            end = L
            while True:
                name, mode = OPS[d[o]]
                ln = cs[o]
                pc = base + o
                ops = d[o + 1:o + ln]
                if name in CONTROL:
                    if cnt: body.append("    return 0x%04X;" % pc)
                    break
                step = "    NB_STEP(0x%04X, 0x%02X); " % (pc, d[o])
                if name in BRANCH:
                    rel = ops[0] - 256 if ops[0] > 127 else ops[0]
                    t = (pc + 2 + rel) & 0xFFFF
                    body.append(step + "return (%s) ? 0x%04X : 0x%04X;" % (BRANCH[name], t, (pc + 2) & 0xFFFF))
                    cnt += 1
                    end = o + ln
                    break
                c = emit_insn(name, mode, ops)
                if c is None:            # istruzione non modellata: il blocco finisce prima
                    if cnt: body.append("    return 0x%04X;" % pc)
                    break
                body.append(step + c)
                cnt += 1
                end = o + ln
                nxt = o + ln
                if name in STACK_LIFT or nxt in leaders or nxt not in cs:
                    body.append("    return 0x%04X;" % ((base + nxt) & 0xFFFF))
                    break
                o = nxt
            if not cnt:
                continue
            fn = "mmc5_b_%02X_%04X" % (u, L)
            lines.append("uint16_t %s(void) {" % fn)
            lines += body
            lines.append("}")
            table.append((u, win, L, cnt, fn, end - L, fnv(rom.prg[u * UNIT + L:u * UNIT + end])))
            n_insn += cnt
        with open(os.path.join(outdir, "mmc5_blocks_u%02d.c" % u), "w") as f:
            f.write("\n".join(lines) + "\n")

    with open(os.path.join(outdir, "mmc5_blocks_tab.c"), "w") as f:
        f.write('#include "nes_blocks.h"\n\n')
        for t in table:
            f.write("uint16_t %s(void);\n" % t[4])
        f.write("\nstatic const NesBlockEntry s_tab[] = {\n")
        for u, win, off, cnt, fn, nb, hs in table:
            f.write("    {%d, %d, 0x%04X, %d, %s, %d, 0x%08XU},\n" % (u, win, off, cnt, fn, nb, hs))
        f.write("};\n\nstatic const uint8_t s_codebits[%d] = {" % len(codebits))
        for i, b in enumerate(codebits):
            if i % 32 == 0: f.write("\n")
            f.write("%d," % b)
        f.write("\n};\n\n")
        f.write("static const NesCtlEntry s_ctl[] = {\n")
        for u, win, off, op, a, b in ctl:
            f.write("    {%d, %d, 0x%04X, 0x%02X, 0x%02X, 0x%02X},\n" % (u, win, off, op, a, b))
        f.write("};\n\n")
        f.write("void nes_mmc5_blocks_init(void) {\n    nes_blocks_install(s_tab, %d, s_codebits, %d);\n    nes_blocks_install_ctl(s_ctl, %d);\n}\n" % (len(table), rom.nunits, len(ctl)))
    ncode = sum(len(c) for c in code.values())
    print("istruzioni tradotte: %d in %d blocchi (istruzioni nel codice trovato: %d)" % (n_insn, len(table), ncode))


if __name__ == "__main__":
    build(sys.argv[1], sys.argv[2], sys.argv[3:])
