#!/usr/bin/env python3
"""Decompilatore 6502 -> C per Just Breed (MMC5).

  python decompile.py ROM.nes OUTDIR [cov.bin ...] [--readable]

Per ogni routine (ingresso = bersaglio di JSR, vettore, chiamata far / tabella, oppure codice orfano) produce
una funzione C `void fUU_AAAA(void)`:
  * i flag N/Z/C/V/D/I vengono calcolati solo quando qualcuno li legge (analisi di vivezza) e i salti
    condizionati si leggono come condizioni ( if (rA == 0x05) ... );
  * JSR tra routine decompilate = chiamata C diretta; il resto (argomenti inline, tabelle di salto, indirizzi
    di ritorno estratti dallo stack, codice non decodificato) passa dall'interprete come 'isola' (nes_decomp.h);
  * le routine che non si possono esprimere come funzione C (stack non bilanciato, TSX/TXS, RTS come salto)
    non vengono generate: le esegue l'interprete.
Senza --readable ogni istruzione chiama NB_STEP (campionamento NMI, copertura): e' la versione eseguibile.
Con --readable NB_STEP sparisce: e' il sorgente da leggere (non va compilato).
"""
import os, sys, collections, re
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import disasm

UNIT = disasm.UNIT
OPS = disasm.OPS
LEN = disasm.LEN
FLAGS = "NZCVDI"
ALL = set(FLAGS)
BR_FLAG = {"BCC": ("C", 0), "BCS": ("C", 1), "BEQ": ("Z", 1), "BNE": ("Z", 0), "BMI": ("N", 1), "BPL": ("N", 0),
           "BVC": ("V", 0), "BVS": ("V", 1)}
NZ_DEF = {"LDA", "LDX", "LDY", "AND", "ORA", "EOR", "TAX", "TAY", "TXA", "TYA", "INX", "INY", "DEX", "DEY", "INC", "DEC", "PLA"}
DEFS = {"ASL": "NZC", "LSR": "NZC", "ROL": "NZC", "ROR": "NZC", "CMP": "NZC", "CPX": "NZC", "CPY": "NZC",
        "ADC": "NZCV", "SBC": "NZCV", "BIT": "NZV", "CLC": "C", "SEC": "C", "CLD": "D", "SED": "D", "CLI": "I",
        "SEI": "I", "CLV": "V", "PLP": "NZCVDI"}
for n in NZ_DEF:
    DEFS[n] = "NZ"
USES = {"ADC": "C", "SBC": "C", "ROL": "C", "ROR": "C", "PHP": "NZCVDI"}
HW = disasm.HW


def fnv(data, h=2166136261):
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def ranges_of(body):
    """(offset, lunghezza) dei byte di istruzione della funzione, fusi quando contigui."""
    spans = sorted((i.off, i.ln) for i in body.values())
    out = []
    for o, ln in spans:
        if out and out[-1][0] + out[-1][1] == o:
            out[-1][1] += ln
        else:
            out.append([o, ln])
    return out
RUNNABLE = True


def defs_of(name):
    return set(DEFS.get(name, ""))


def uses_of(name):
    if name in BR_FLAG:
        return {BR_FLAG[name][0]}
    return set(USES.get(name, ""))


class Ins:
    __slots__ = ("u", "off", "pc", "name", "mode", "ops", "ln", "op", "succ", "kind", "extra", "live_out", "label", "fused", "temp", "callee", "jcallee", "carry_const")

    def __repr__(self):
        return "%04X %s" % (self.pc, self.name)


def load_code(rom_path, covs):
    rom = disasm.Rom(rom_path, covs)
    extra = disasm.load_seeds(rom)
    code, targets, ext = disasm.analyze(rom, extra)
    return rom, code, targets


def callee_kind(rom, u, off, ln, tu, to, learned, inline):
    """Come una JSR a (tu,to) usa i byte dopo di se': ('n', N) byte inline, ('table',), oppure None."""
    en = disasm.site_skip_emu(rom, u, off, tu, to)
    if en is None and (tu, to) not in learned and disasm.is_table_routine(rom, tu, to):
        return ("table",)
    lr = learned.get((tu, to))
    if en is not None and en > 0:
        lr = ("n", en)
    if lr and lr[0] == "n":
        return ("n", lr[1])
    if lr or (tu, to) in inline:
        p = off + ln
        while p < UNIT and rom.prg[u * UNIT + p] != 0:
            p += 1
        return ("n", p + 1 - (off + ln))
    return None


def build(rom, code, outdir, readable):
    learned = rom.learned
    inline = disasm.inline_routines(rom)
    prg = rom.prg
    ins = {}                                    # (u, off) -> Ins
    # solo le unita' in cui si e' visto eseguire codice: nelle altre il "codice" trovato dall'analisi e' quasi sempre dati
    code_units = {u for u in range(rom.nunits) if any(rom.op[u * UNIT:(u + 1) * UNIT])}
    for u in range(rom.nunits):
        if u not in code_units:
            continue
        for off, ln in code[u].items():
            i = Ins()
            i.u, i.off, i.ln = u, off, ln
            i.pc = rom.base[u] + off
            i.op = prg[u * UNIT + off]
            i.name, i.mode = OPS[i.op]
            i.ops = prg[u * UNIT + off + 1:u * UNIT + off + ln]
            i.kind = None
            i.extra = None
            i.callee = None
            i.jcallee = None
            i.carry_const = None
            i.fused = None
            i.temp = None
            ins[(u, off)] = i

    # ---- chiamate: tipo dei siti JSR, ingressi di funzione ----
    entries = set()
    fixed = prg[rom.last * UNIT:]
    for vec in (0x1FFA, 0x1FFC, 0x1FFE):
        a = fixed[vec] | (fixed[vec + 1] << 8)
        entries.add((rom.last, a - 0xE000))
    for (u, off), i in ins.items():
        if i.name == "JSR" and i.mode == "abs":
            a = i.ops[0] | (i.ops[1] << 8)
            tu, to = rom.unit_of(a, u)
            if tu is None:
                i.kind = ("dyn", a)
                continue
            k = callee_kind(rom, u, off, i.ln, tu, to, learned, inline)
            if k and k[0] == "n":
                i.kind = ("inline", a, off + i.ln + k[1])
                for kk in range(0, k[1] - 2):                 # chiamata far: (banco|$80, lo, hi)
                    b0 = prg[u * UNIT + off + i.ln + kk]
                    a2 = prg[u * UNIT + off + i.ln + kk + 1] | (prg[u * UNIT + off + i.ln + kk + 2] << 8)
                    v = b0 & 0x3F
                    if b0 & 0x80 and v < rom.nunits and rom.base[v] <= a2 < rom.base[v] + UNIT:
                        entries.add((v, a2 - rom.base[v]))
            elif k and k[0] == "table":
                i.kind = ("table", a)
                p = off + i.ln
                dd = prg[u * UNIT:(u + 1) * UNIT]
                while p + 1 < UNIT and p not in code[u]:
                    w = dd[p] | (dd[p + 1] << 8)
                    hit = None
                    for delta in (0, 1):
                        t2u, t2o = rom.unit_of((w + delta) & 0xFFFF, u)
                        if t2u is not None and disasm.plausible_code(rom, t2u, t2o):
                            hit = (t2u, t2o)
                            break
                    if not hit:
                        break
                    entries.add(hit)
                    p += 2
            else:
                # $C000-$DFFF da un'altra unita': quale unita' e' mappata dipende da $5116 -> dinamico
                if 0xC000 <= a < 0xE000 and rom.base[u] != 0xC000:
                    i.kind = ("dyn", a)
                else:
                    i.kind = ("call", tu, to)
                    entries.add((tu, to))
            if i.kind and i.kind[0] in ("call", "inline", "table"):
                i.callee = (tu, to)
    for (u, off), i in ins.items():
        if i.name == "JMP" and i.mode == "abs":
            a = i.ops[0] | (i.ops[1] << 8)
            tu, to = rom.unit_of(a, u)
            if tu is not None and not (0xC000 <= a < 0xE000 and rom.base[u] != 0xC000):
                i.jcallee = (tu, to)
    entries = {e for e in entries if e in ins}
    entries_all = set(entries)

    # ---- successori ----
    def successors(i):
        u = i.u
        n = i.name
        nxt = (u, i.off + i.ln)
        if n in BR_FLAG:
            rel = i.ops[0] - 256 if i.ops[0] > 127 else i.ops[0]
            t = i.off + 2 + rel
            out = [nxt]
            if 0 <= t < UNIT:
                out.append((u, t))
            else:
                i.extra = ("esc_branch", (i.pc + 2 + rel) & 0xFFFF)
            return out
        if n == "JMP" and i.mode == "abs":
            a = i.ops[0] | (i.ops[1] << 8)
            tu, to = rom.unit_of(a, u)
            if tu == u:
                return [(u, to)]
            return []
        if n in ("JMP", "RTS", "RTI", "BRK"):
            return []
        if n == "JSR":
            if i.kind and i.kind[0] == "table":
                return []
            if i.kind and i.kind[0] == "inline":
                return [(u, i.kind[2])]
        return [nxt]

    def build_function(entry):
        """Istruzioni raggiungibili dall'ingresso (senza attraversare altri ingressi via JMP)."""
        body = {}
        work = [entry]
        while work:
            k = work.pop()
            if k in body or k not in ins:
                continue
            body[k] = ins[k]
            for s in successors(ins[k]):
                # un JMP verso l'ingresso di un'altra funzione e' una tail call, non parte di questa
                if ins[k].name == "JMP" and s in entries_all and s != entry:
                    continue
                work.append(s)
        return body

    funcs = {}
    hard = {}
    for e in sorted(entries):
        funcs[e] = build_function(e)
    return rom, ins, entries_all, funcs, successors


def analyze_function(entry, body, ins, entries, rom, successors):
    """Profondita' dello stack e giudizio 'hard' (non esprimibile come funzione C). Ritorna (ok, motivo)."""
    depth = {}
    work = [(entry, 0)]
    while work:
        k, d = work.pop()
        if k not in body:
            continue
        if k in depth:
            if depth[k] != d:
                return False, "profondita' stack incoerente a %04X" % body[k].pc
            continue
        depth[k] = d
        i = body[k]
        n = i.name
        nd = d
        if n in ("PHA", "PHP"):
            nd = d + 1
        elif n in ("PLA", "PLP"):
            nd = d - 1
            if nd < 0:
                return False, "PL* sotto il livello d'ingresso a %04X" % i.pc
        elif n in ("TSX", "TXS"):
            return False, "%s a %04X" % (n, i.pc)
        elif n == "JSR" and i.kind and i.kind[0] == "table":
            continue
        if n == "RTS" and d != 0:
            return False, "RTS con stack non bilanciato a %04X (RTS come salto?)" % i.pc
        if n == "JMP" and i.mode == "abs":
            a = i.ops[0] | (i.ops[1] << 8)
            tu, to = rom.unit_of(a, i.u)
            if (tu, to) not in body and d != 0:
                return False, "JMP fuori con stack non bilanciato a %04X" % i.pc
        for s in successors(i):
            work.append((s, nd))
    return True, ""


def is_exit(i, body, successors):
    """L'istruzione lascia la funzione (o cade in codice non decodificato): tutti i flag restano vivi."""
    n = i.name
    if n in ("RTS", "RTI", "BRK"):
        return True
    if i.extra is not None:
        return True
    if n == "JMP":
        return not any(s in body for s in successors(i))
    if n == "JSR" and i.kind and i.kind[0] == "table":
        return True
    if n in BR_FLAG:
        return (i.u, i.off + i.ln) not in body
    nk = (i.u, i.kind[2]) if (n == "JSR" and i.kind and i.kind[0] == "inline") else (i.u, i.off + i.ln)
    return nk not in body


def live_in(i):
    if i.name == "JSR":
        return set(ALL)              # il chiamato puo' leggere i flag; poi ne restituisce di suoi
    return uses_of(i.name) | (i.live_out - defs_of(i.name))


def liveness(entry, body, successors):
    """live_out di ogni istruzione (flag ancora letti dopo di essa)."""
    for i in body.values():
        i.live_out = set()
    changed = True
    order = sorted(body, key=lambda k: -k[1])
    while changed:
        changed = False
        for k in order:
            i = body[k]
            out = set()
            for s in successors(i):
                if s in body:
                    out |= live_in(body[s])
            if is_exit(i, body, successors):
                out |= ALL
            if out != i.live_out:
                i.live_out = out
                changed = True


# ------------------------------------------------------------------ emissione C
def mem_ref(i):
    """Operando di memoria: ('ram', lvalue) oppure ('bus', indirizzo)."""
    m = i.mode
    a = i.ops[0] if len(i.ops) == 1 else (i.ops[0] | (i.ops[1] << 8)) if i.ops else 0
    # Versione eseguibile: OGNI accesso passa da nes_read/nes_write (ognuno fa avanzare l'orologio di bus:
    # PPU/APU/IRQ). Versione leggibile: la RAM si scrive come g_ram[...].
    ram = "bus" if RUNNABLE else "ram"
    if m == "zpg":
        return ram, ("0x%02X" % a) if RUNNABLE else "g_ram[0x%02X]" % a
    if m == "zpx":
        return ram, ("(uint8_t)(0x%02X + rX)" % a) if RUNNABLE else "g_ram[(uint8_t)(0x%02X + rX)]" % a
    if m == "zpy":
        return ram, ("(uint8_t)(0x%02X + rY)" % a) if RUNNABLE else "g_ram[(uint8_t)(0x%02X + rY)]" % a
    if m == "abs":
        if a < 0x800:
            return ram, ("0x%04X" % a) if RUNNABLE else "g_ram[0x%04X]" % a
        return "bus", hw_name(a)
    if m in ("abx", "aby"):
        r = "rX" if m == "abx" else "rY"
        if a + 0xFF < 0x800:
            return ram, ("(uint16_t)(0x%04X + %s)" % (a, r)) if RUNNABLE else "g_ram[0x%04X + %s]" % (a, r)
        return "bus", "(uint16_t)(%s + %s)" % (hw_name(a), r)
    if m == "inx":
        return "bus", "nes_read16zp((uint8_t)(0x%02X + rX))" % a
    if m == "iny":
        return "bus", "(uint16_t)(nes_read16zp(0x%02X) + rY)" % a
    raise ValueError(m)


def hw_name(a):
    return HW.get(a) or "0x%04X" % a


def rd_expr(i):
    if i.mode == "imm":
        return "0x%02X" % i.ops[0]
    k, e = mem_ref(i)
    return e if k == "ram" else "nes_read(%s)" % e


def wr_stmt(i, val):
    k, e = mem_ref(i)
    return "%s = %s;" % (e, val) if k == "ram" else "nes_write(%s, %s);" % (e, val)


def nz(v, L):
    s = []
    if "N" in L:
        s.append("g_cpu.N = ((uint8_t)(%s) >> 7) & 1;" % v)
    if "Z" in L:
        s.append("g_cpu.Z = ((uint8_t)(%s) == 0);" % v)
    return s


def rmw(i, L, kind):
    """ASL/LSR/ROL/ROR/INC/DEC su memoria o accumulatore."""
    if i.mode == "acc":
        v, pre, post = "rA", "", ""
        target = "rA"
        pieces = []
    else:
        k, e = mem_ref(i)
        target = None
    body = []
    if i.mode == "acc":
        cur = "rA"
    else:
        body.append("uint16_t a = %s;" % e if k == "bus" else "")
        cur = "v"
    stm = []
    if i.mode != "acc":
        stm.append("uint8_t v = %s;" % ("nes_read(a)" if k == "bus" else e))
    if kind == "ASL":
        if "C" in L: stm.append("g_cpu.C = (%s >> 7) & 1;" % cur)
        stm.append("%s = (uint8_t)(%s << 1);" % (cur, cur))
    elif kind == "LSR":
        if "C" in L: stm.append("g_cpu.C = %s & 1;" % cur)
        stm.append("%s >>= 1;" % cur)
    elif kind == "ROL":
        stm.append("{ uint8_t c = g_cpu.C;")
        if "C" in L: stm.append("g_cpu.C = (%s >> 7) & 1;" % cur)
        stm.append("%s = (uint8_t)((%s << 1) | c); }" % (cur, cur))
    elif kind == "ROR":
        stm.append("{ uint8_t c = g_cpu.C;")
        if "C" in L: stm.append("g_cpu.C = %s & 1;" % cur)
        stm.append("%s = (uint8_t)((%s >> 1) | (c << 7)); }" % (cur, cur))
    elif kind == "INC":
        stm.append("%s = (uint8_t)(%s + 1);" % (cur, cur))
    elif kind == "DEC":
        stm.append("%s = (uint8_t)(%s - 1);" % (cur, cur))
    if i.mode != "acc":
        stm.append("nes_write(a, v);" if k == "bus" else "%s = v;" % e)
    stm += nz(cur, L)
    if i.mode == "acc":
        return " ".join(stm)
    pre = "uint16_t a = %s; " % e if k == "bus" else ""
    return "{ " + pre + " ".join(stm) + " }"


def emit_ins(i, L, fname, label_of, runnable, entries_names):
    """Testo C di un'istruzione (senza STEP). L = flag vivi dopo l'istruzione."""
    n = i.name
    reg = {"LDA": "rA", "LDX": "rX", "LDY": "rY"}
    if n in reg:
        return " ".join(["%s = %s;" % (reg[n], rd_expr(i))] + nz(reg[n], L))
    if n == "STA": return wr_stmt(i, "rA")
    if n == "STX": return wr_stmt(i, "rX")
    if n == "STY": return wr_stmt(i, "rY")
    simple = {"TAX": ("rX", "rA"), "TAY": ("rY", "rA"), "TXA": ("rA", "rX"), "TYA": ("rA", "rY")}
    if n in simple:
        d, s = simple[n]
        return " ".join(["%s = %s;" % (d, s)] + nz(d, L))
    if n in ("INX", "INY", "DEX", "DEY"):
        r = "rX" if n.endswith("X") else "rY"
        return " ".join(["%s%s;" % (r, "++" if n.startswith("IN") else "--")] + nz(r, L))
    if n in ("AND", "ORA", "EOR"):
        op = {"AND": "&=", "ORA": "|=", "EOR": "^="}[n]
        return " ".join(["rA %s %s;" % (op, rd_expr(i))] + nz("rA", L))
    if n == "ADC":
        s = ["uint8_t m = %s;" % rd_expr(i), "uint16_t r = rA + m + g_cpu.C;"]
        if "C" in L: s.append("g_cpu.C = r > 0xFF;")
        if "V" in L: s.append("g_cpu.V = (~(rA ^ m) & (rA ^ r) & 0x80) != 0;")
        s.append("rA = (uint8_t)r;")
        s += nz("rA", L)
        return "{ " + " ".join(s) + " }"
    if n == "SBC":
        s = ["uint8_t m = %s;" % rd_expr(i), "int16_t r = rA - m - (1 - g_cpu.C);"]
        if "C" in L: s.append("g_cpu.C = r >= 0;")
        if "V" in L: s.append("g_cpu.V = ((rA ^ m) & (rA ^ r) & 0x80) != 0;")
        s.append("rA = (uint8_t)r;")
        s += nz("rA", L)
        return "{ " + " ".join(s) + " }"
    if n in ("CMP", "CPX", "CPY"):
        r = {"CMP": "rA", "CPX": "rX", "CPY": "rY"}[n]
        s = ["uint8_t m = %s;" % rd_expr(i)]
        if "C" in L: s.append("g_cpu.C = %s >= m;" % r)
        if "N" in L or "Z" in L:
            s.append("uint8_t t = (uint8_t)(%s - m);" % r)
            s += nz("t", L)
        return "{ " + " ".join(s) + " }"
    if n == "BIT":
        s = ["uint8_t m = %s;" % rd_expr(i)]
        if "Z" in L: s.append("g_cpu.Z = (rA & m) == 0;")
        if "N" in L: s.append("g_cpu.N = (m >> 7) & 1;")
        if "V" in L: s.append("g_cpu.V = (m >> 6) & 1;")
        return "{ " + " ".join(s) + " }"
    if n in ("ASL", "LSR", "ROL", "ROR", "INC", "DEC"):
        return rmw(i, L, n)
    if n == "PHA": return "NB_PHA();"
    if n == "PHP": return "NB_PHP();"
    if n == "PLA":
        return " ".join(["g_cpu.S++; rA = g_ram[0x100 + g_cpu.S];"] + nz("rA", L))
    if n == "PLP": return "NB_PLP();"
    flag_ops = {"CLC": ("C", 0), "SEC": ("C", 1), "CLD": ("D", 0), "SED": ("D", 1), "CLI": ("I", 0), "SEI": ("I", 1),
                "CLV": ("V", 0)}
    if n in flag_ops:
        f, v = flag_ops[n]
        return "g_cpu.%s = %d;" % (f, v) if f in L else ""
    if n == "NOP":
        return ""
    return None


def cond_text(i):
    f, v = BR_FLAG[i.name]
    return "g_cpu.%s" % f if v else "!g_cpu.%s" % f


def emit_function(entry, body, ins, rom, entries, fn_ok, runnable, fname_of, fn_index):
    u = entry[0]
    lines = []
    name = fname_of(entry)
    # etichette
    labels = set()
    for k, i in body.items():
        if i.name in BR_FLAG:
            rel = i.ops[0] - 256 if i.ops[0] > 127 else i.ops[0]
            t = i.off + 2 + rel
            if (u, t) in body:
                labels.add((u, t))
        elif i.name == "JMP" and i.mode == "abs":
            a = i.ops[0] | (i.ops[1] << 8)
            tu, to = rom.unit_of(a, u)
            if tu == u and (u, to) in body:
                labels.add((u, to))
    order = sorted(body, key=lambda x: x[1])

    def fallthrough(i):
        n = i.name
        if n in ("JMP", "RTS", "RTI", "BRK"):
            return None
        if n == "JSR" and i.kind and i.kind[0] == "table":
            return None
        if n == "JSR" and i.kind and i.kind[0] == "inline":
            return (i.u, i.kind[2])
        return (i.u, i.off + i.ln)

    goto_after = {}                           # istruzione -> chiave dove deve proseguire (non e' la successiva emessa)
    for idx, k in enumerate(order):
        ft = fallthrough(body[k])
        if ft is not None and ft in body and (idx + 1 >= len(order) or order[idx + 1] != ft):
            goto_after[k] = ft
            labels.add(ft)
    if order and order[0] != entry:
        labels.add(entry)
    needs_s0 = False
    out = []
    last_off = None
    if order and order[0] != entry:
        out.append("    goto L_%04X;" % (rom.base[u] + entry[1]))
    for k in order:
        i = body[k]
        if last_off is not None and i.off != last_off:
            out.append("")                                  # salto nel flusso lineare
        last_off = i.off + i.ln
        if isinstance(i.kind, tuple) and i.kind[0] == "inline":
            last_off = i.kind[2]
        if k in labels:
            out.append("L_%04X:;" % i.pc)
        step = "NB_STEP(0x%04X, 0x%02X); " % (i.pc, i.op) if runnable else ""
        n = i.name
        txt = None
        if n in BR_FLAG:
            rel = i.ops[0] - 256 if i.ops[0] > 127 else i.ops[0]
            t = i.off + 2 + rel
            if (u, t) in body:
                txt = "if (%s) goto L_%04X;" % (cond_text(i), rom.base[u] + t)
            else:
                needs_s0 = True
                txt = "if (%s) ESCAPE(0x%04X);" % (cond_text(i), (i.pc + 2 + rel) & 0xFFFF)
        elif n == "JSR":
            a = i.ops[0] | (i.ops[1] << 8)
            ret = (i.pc + 2) & 0xFFFF
            kd = i.kind
            if kd is None or kd[0] == "dyn":
                needs_s0 = True
                txt = "JSR_DYN(0x%04X, 0x%04X);" % (a, ret)
            elif kd[0] == "inline":
                cont = rom.base[u] + kd[2]
                txt = "JSR_INLINE(0x%04X, 0x%04X, 0x%04X);" % (a, ret, cont)
            elif kd[0] == "table":
                needs_s0 = True
                txt = "JSR_TABLE(0x%04X, 0x%04X);" % (a, ret)
            else:
                tgt = (kd[1], kd[2])
                if tgt in fn_ok:
                    txt = "JSR(%s, 0x%04X);" % (fname_of(tgt), ret)
                else:
                    needs_s0 = True
                    txt = "JSR_DYN(0x%04X, 0x%04X);" % (a, ret)
        elif n == "JMP":
            if i.mode == "abs":
                a = i.ops[0] | (i.ops[1] << 8)
                tu, to = rom.unit_of(a, u)
                if tu == u and (u, to) in body:
                    txt = "goto L_%04X;" % a
                elif tu is not None and (tu, to) in fn_ok and not (0xC000 <= a < 0xE000 and rom.base[u] != 0xC000):
                    txt = "JMP_FN(%s);" % fname_of((tu, to))
                else:
                    needs_s0 = True
                    txt = "JMP_DYN(0x%04X);" % a
            else:
                a = i.ops[0] | (i.ops[1] << 8)
                needs_s0 = True
                txt = "JMP_IND(0x%04X);" % a
        elif n == "RTS":
            txt = "RTS();"
        elif n in ("RTI", "BRK"):
            needs_s0 = True
            txt = "ESCAPE(0x%04X);" % i.pc
        else:
            txt = emit_ins(i, i.live_out, name, None, runnable, None)
            if txt is None:
                needs_s0 = True
                txt = "ESCAPE(0x%04X);" % i.pc
        out.append("    " + (step + txt).rstrip())
        if k in goto_after:
            out.append("    goto L_%04X;" % (rom.base[u] + goto_after[k][1]))
        # istruzione che cade fuori dal corpo (successore non decodificato): esce nell'interprete
        if n not in ("JMP", "RTS", "RTI", "BRK") and not (n in BR_FLAG and False):
            nk = (u, i.off + i.ln) if not (isinstance(i.kind, tuple) and i.kind[0] == "inline") else (u, i.kind[2])
            if not (n == "JSR" and isinstance(i.kind, tuple) and i.kind[0] == "table"):
                if nk not in body:
                    needs_s0 = True
                    out.append("    ESCAPE(0x%04X);  /* codice non decodificato */" % ((rom.base[u] + nk[1]) & 0xFFFF))
    head = ["/* unita' %d  $%04X */" % (u, rom.base[u] + entry[1]), "void %s(void) {" % name]
    if needs_s0:
        head.append("    uint8_t _s0 = g_cpu.S;")
    if runnable:
        head.append("    DEC_GUARD(%d, 0x%04X);" % (fn_index[entry], rom.base[u] + entry[1]))
    return "\n".join(head + out + ["}", ""])


def readable_line(s):
    """Sintassi da leggere: registri A/X/Y, flag N/Z/C/V, chiamate senza macro."""
    s = re.sub(r"\brA\b", "A", s)
    s = re.sub(r"\brX\b", "X", s)
    s = re.sub(r"\brY\b", "Y", s)
    s = re.sub(r"g_cpu\.([NZCVDI])\b", r"\1", s)
    s = s.replace("call_dyn(", "call(")
    s = s.replace("(uint8_t)", "").replace("(uint16_t)", "")
    return s


def asm_text(i, syms, rom):
    n = i.name.lower()
    a = i.ops[0] if len(i.ops) == 1 else (i.ops[0] | (i.ops[1] << 8)) if i.ops else 0
    m = i.mode
    if m in ("imp",):
        return n
    if m == "acc":
        return n + " a"
    if m == "imm":
        return "%s #$%02X" % (n, a)
    if m == "rel":
        rel = i.ops[0] - 256 if i.ops[0] > 127 else i.ops[0]
        return "%s $%04X" % (n, (i.pc + 2 + rel) & 0xFFFF)
    nm = syms.mem(a) if (a in HW or a in syms.ram or a in syms.auto) else ("$%04X" % a if a > 0xFF else "$%02X" % a)
    return {"abs": "%s %s", "abx": "%s %s,x", "aby": "%s %s,y", "zpg": "%s %s", "zpx": "%s %s,x", "zpy": "%s %s,y",
            "ind": "%s (%s)", "inx": "%s (%s,x)", "iny": "%s (%s),y"}[m] % (n, nm)


def instr_preds(body, successors):
    preds = collections.defaultdict(list)
    for k, i in body.items():
        for s in successors(i):
            if s in body and k not in preds[s]:
                preds[s].append(k)
    return preds


def main():
    import decomp_emit as E
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    readable = "--readable" in sys.argv
    exact = "--exact" in sys.argv
    use_ipa = "--ipa" in sys.argv or "--closed" in sys.argv
    global RUNNABLE
    RUNNABLE = not readable
    rom_path, outdir, covs = args[0], args[1], args[2:]
    rom, code, targets = load_code(rom_path, covs)
    rom, ins, entries, funcs, successors = build(rom, code, outdir, readable)

    # scelta funzioni esprimibili
    fn_ok = {}
    hard_list = []
    hard_bodies = {}
    for e, body in sorted(funcs.items()):
        ok, why = analyze_function(e, body, ins, entries, rom, successors)
        if ok:
            fn_ok[e] = body
        else:
            hard_list.append((e, why))
            hard_bodies[e] = body
    # codice non coperto da nessuna funzione: frammenti orfani (raggiunti solo da chiamate dinamiche)
    covered = set()
    for body in fn_ok.values():
        covered |= set(body)
    orphans = 0
    orphan_set = set()
    succs = {k: successors(i) for k, i in ins.items()}
    while True:
        rest = [k for k in sorted(ins) if k not in covered]
        if not rest:
            break
        restset = set(rest)
        hasp = set()
        for k in rest:
            for s in succs[k]:
                if s in restset and s != k:
                    hasp.add(s)
        starts = [k for k in rest if k not in hasp] or [rest[0]]
        k = starts[0]
        body = {}
        work = [k]
        while work:
            x = work.pop()
            if x in body or x not in ins:
                continue                      # il codice condiviso con altre funzioni viene duplicato: nessun ESCAPE a meta'
            body[x] = ins[x]
            for s in succs[x]:
                work.append(s)
        entries.add(k)
        ok, why = analyze_function(k, body, ins, entries, rom, successors)
        covered |= set(body)
        if ok:
            fn_ok[k] = body
            orphans += 1
            orphan_set.add(k)
        else:
            hard_list.append((k, why))
            hard_bodies[k] = body
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        if f.startswith("mmc5_dec_") and (f.endswith(".c") or f.endswith(".h")):
            os.remove(os.path.join(outdir, f))

    syms_path = os.path.join(os.getcwd(), "analysis", "symbols.tsv")
    syms = E.Symbols(HW, syms_path)

    def fname_of(e):
        k = (e[0], rom.base[e[0]] + e[1])
        if k in syms.func:
            return syms.func[k]
        return "f%02d_%04X" % (e[0], rom.base[e[0]] + e[1])

    import decomp_names
    decomp_names.auto_names(syms, fn_ok, rom)
    fn_all = dict(hard_bodies)
    fn_all.update(fn_ok)

    E.READABLE = readable
    c = E.Ctx(not readable, syms, exact, use_ipa)
    # funzioni con chiamanti non tutti noti staticamente (ret_live = tutti i flag)
    static_called = set()
    dyn_addrs = set()
    for e, body in fn_ok.items():
        for i in body.values():
            if i.name == "JSR" and i.kind and i.kind[0] == "call":
                static_called.add((i.kind[1], i.kind[2]))
            g = E.tail_target(i, rom, fn_ok)
            if g:
                static_called.add(g)
            if i.name == "JSR" and i.kind and i.kind[0] == "dyn":
                dyn_addrs.add(i.kind[1])
            if i.name == "JMP" and E.tail_target(i, rom, fn_ok) is None:
                dyn_addrs.add(E.opv(i))
    open_fns = set(hard_bodies) | {e for e in fn_ok if e not in static_called or e in orphan_set or (rom.base[e[0]] + e[1]) in dyn_addrs}
    if not exact:
        for e, body in fn_all.items():
            pr = instr_preds(body, successors)
            E.mark_fusion(c, body, pr)
            E.mark_carry(body, pr)
    if "--closed" in sys.argv:              # ipotesi: nessun chiamante ignoto (i flag restituiti servono solo ai chiamanti noti)
        open_fns = set(hard_bodies)
    S = E.ipa(c, fn_ok, successors, rom, open_fns, fn_all)
    c.S = S
    if exact:
        for body in fn_ok.values():
            for i in body.values():
                i.live_out = E.ALL
    fn_index = {e: n for n, e in enumerate(sorted(fn_ok))}
    per_unit = collections.defaultdict(list)
    fallback = 0
    for e in sorted(fn_ok):
        try:
            F, lines = E.render_function(c, e, fn_ok[e], successors, rom, fn_ok, fname_of, fn_index)
        except Exception as ex:                       # struttura non gestita: nessuna funzione persa, ma si segnala
            print("ATTENZIONE: strutturazione fallita per %s: %r" % (fname_of(e), ex))
            raise
        u = e[0]
        head = ["/* unita' %d  $%04X */" % (u, rom.base[u] + e[1]), "void %s(void) {" % fname_of(e)]
        if F.needs_s0 and not readable:
            head.append("    uint8_t _s0 = g_cpu.S;")
        for t in sorted(F.temps):
            head.append("    uint8_t %s;" % t)
        if readable:
            ins_r = [r.upper() for r in "axy" if r in S.entry_live[e]]
            out_r = [r.upper() for r in "axy" if r in S.ret_live[e] and r in S.may_def_regs.get(e, set())]
            if ins_r or out_r:
                head.insert(0, "/* in: %s   out: %s */" % (", ".join(ins_r) or "-", ", ".join(out_r) or "-"))
        if not readable:
            head.append("    DEC_GUARD(%d, 0x%04X);" % (fn_index[e], rom.base[u] + e[1]))
        if readable:
            lines = [readable_line(x) for x in lines]
        per_unit[u].append("\n".join(head + lines + ["}", ""]))
    suffix = "_readable" if readable else ""
    for u, funcs_txt in per_unit.items():
        hdr = '#include "nes_decomp.h"\n#include "mmc5_dec_decls.h"\n\n' if not readable else ""
        open(os.path.join(outdir, "mmc5_dec_u%02d%s.c" % (u, suffix)), "w", encoding="utf-8").write(hdr + "\n".join(funcs_txt))
    if not readable:
        with open(os.path.join(outdir, "mmc5_dec_decls.h"), "w") as f:
            f.write('#include "nes_decomp.h"\n#include "mmc5_dec_syms.h"\n')
            for e in sorted(fn_ok):
                f.write("void %s(void);\n" % fname_of(e))
        with open(os.path.join(outdir, "mmc5_dec_syms.h"), "w") as f:
            for a, nme in sorted(HW.items()):
                f.write("#define %s 0x%04X\n" % (nme, a))
            for a, nme in sorted(list(syms.ram.items()) + list(syms.auto.items())):
                if a not in HW:
                    f.write("#define %s 0x%04X\n" % (nme, a))
        with open(os.path.join(outdir, "mmc5_dec_tab.c"), "w") as f:
            f.write('#include "mmc5_dec_decls.h"\n\n')
            for e in sorted(fn_ok):
                rs = ranges_of(fn_ok[e])
                f.write("static const uint16_t r%d[] = {%s0,0};\n" % (fn_index[e], "".join("%d,%d," % (o, l) for o, l in rs)))
            f.write("\nstatic const NesDecompEntry s_tab[] = {\n")
            for e in sorted(fn_ok):
                win = (rom.base[e[0]] - 0x8000) >> 13
                h = 2166136261
                for o, l in ranges_of(fn_ok[e]):
                    h = fnv(rom.prg[e[0] * UNIT + o:e[0] * UNIT + o + l], h)
                f.write("    {%d, %d, 0x%04X, %s, 0x%08XU, r%d},\n" % (e[0], win, e[1], fname_of(e), h, fn_index[e]))
            f.write("};\n\nuint8_t mmc5_dec_valid[%d];\nvoid nes_mmc5_decomp_init(void) { nes_decomp_install(s_tab, %d, mmc5_dec_valid); }\n" % (len(fn_ok), len(fn_ok)))
    else:
        with open(os.path.join(outdir, "mmc5_symbols.h"), "w", encoding="utf-8") as f:
            f.write("/* nomi simbolici usati dal sorgente leggibile (modificabili in analysis/symbols.tsv) */\n")
            for a, nme in sorted(list(syms.ram.items()) + list(syms.auto.items())):
                f.write("%-28s /* $%04X */\n" % (nme, a))
    if readable:
        # routine non esprimibili come funzione C: listato assembly commentato con il motivo
        why_of = dict(hard_list)
        with open(os.path.join(outdir, "mmc5_dec_hard_readable.c"), "w", encoding="utf-8") as f:
            f.write("/* Routine che non si possono scrivere come funzioni C (manipolano lo stack o i byte dopo la chiamata):\n"
                    " * nel gioco le esegue l'interprete. Qui sono elencate in assembly con il motivo. */\n\n")
            for e in sorted(hard_bodies):
                body = hard_bodies[e]
                f.write("/* unita' %d $%04X - %s */\nvoid %s(void) {\n" % (e[0], rom.base[e[0]] + e[1], why_of.get(e, "?"), fname_of(e)))
                for k in sorted(body, key=lambda x: x[1]):
                    i = body[k]
                    f.write("    asm(\"%s\");  /* %04X */\n" % (asm_text(i, syms, rom), i.pc))
                f.write("}\n\n")
    n_ins = sum(len(b) for b in fn_ok.values())
    print("funzioni decompilate: %d (di cui frammenti orfani %d), istruzioni %d/%d" % (len(fn_ok), orphans, n_ins, len(ins)))
    print("funzioni NON esprimibili (le esegue l'interprete): %d" % len(hard_list))
    fused = sum(1 for b in fn_ok.values() for i in b.values() if getattr(i, "fused", None))
    print("salti fusi in condizioni: %d" % fused)
    rc = collections.Counter(w[:44] for _, w in hard_list)
    for w, cn in rc.most_common(8):
        print("   %4d  %s" % (cn, w))


if __name__ == "__main__":
    main()
