#!/usr/bin/env python3
"""Emissione C del decompilatore: blocchi base, fusione delle condizioni, flag tra funzioni, strutturazione, nomi.

Usato da tools/decompile.py. Due 'sapori' dallo stesso IR:
  runnable : eseguibile e collaudato (NB_STEP per istruzione, ogni accesso a memoria via nes_read/nes_write)
  readable : da leggere (niente NB_STEP; RAM/registri hardware come variabili con nome)
"""
import collections, os, re

FLAGS = "NZCVDI"
ALL = frozenset(FLAGS + "axy")          # flag N Z C V D I + registri a x y (minuscoli)
BR_FLAG = {"BCC": ("C", 0), "BCS": ("C", 1), "BEQ": ("Z", 1), "BNE": ("Z", 0), "BMI": ("N", 1), "BPL": ("N", 0),
           "BVC": ("V", 0), "BVS": ("V", 1)}
NZ_DEF = {"LDA", "LDX", "LDY", "AND", "ORA", "EOR", "TAX", "TAY", "TXA", "TYA", "INX", "INY", "DEX", "DEY", "INC", "DEC", "PLA", "TSX"}
DEFS = {"ASL": "NZC", "LSR": "NZC", "ROL": "NZC", "ROR": "NZC", "CMP": "NZC", "CPX": "NZC", "CPY": "NZC",
        "ADC": "NZCV", "SBC": "NZCV", "BIT": "NZV", "CLC": "C", "SEC": "C", "CLD": "D", "SED": "D", "CLI": "I",
        "SEI": "I", "CLV": "V", "PLP": "NZCVDI"}
for _n in NZ_DEF:
    DEFS[_n] = "NZ"
USES = {"ADC": "C", "SBC": "C", "ROL": "C", "ROR": "C", "PHP": "NZCVDI"}
REG_WRITES = {"LDA": "A", "TXA": "A", "TYA": "A", "PLA": "A", "AND": "A", "ORA": "A", "EOR": "A", "ADC": "A", "SBC": "A",
              "LDX": "X", "TAX": "X", "INX": "X", "DEX": "X", "TSX": "X", "LDY": "Y", "TAY": "Y", "INY": "Y", "DEY": "Y"}
REG_OF = {"CMP": "rA", "CPX": "rX", "CPY": "rY"}


def defs_of(name):
    return frozenset(DEFS.get(name, ""))


def uses_of(name):
    if name in BR_FLAG:
        return frozenset(BR_FLAG[name][0])
    return frozenset(USES.get(name, ""))


REG_USE = {"STA": "a", "STX": "x", "STY": "y", "TAX": "a", "TAY": "a", "TXA": "x", "TYA": "y", "TXS": "x",
           "ADC": "a", "SBC": "a", "AND": "a", "ORA": "a", "EOR": "a", "CMP": "a", "BIT": "a", "CPX": "x", "CPY": "y",
           "INX": "x", "DEX": "x", "INY": "y", "DEY": "y", "PHA": "a"}
REG_DEF = {"LDA": "a", "TXA": "a", "TYA": "a", "PLA": "a", "ADC": "a", "SBC": "a", "AND": "a", "ORA": "a", "EOR": "a",
           "LDX": "x", "TAX": "x", "INX": "x", "DEX": "x", "TSX": "x", "LDY": "y", "TAY": "y", "INY": "y", "DEY": "y"}


def uses_i(i):
    """Flag e registri letti da un'istruzione (modo di indirizzamento compreso)."""
    n = i.name
    u = set(uses_of(n))
    if n in REG_USE:
        u.add(REG_USE[n])
    if n in ("ASL", "LSR", "ROL", "ROR") and i.mode == "acc":
        u.add("a")
    m = i.mode
    if m in ("zpx", "abx", "inx"):
        u.add("x")
    elif m in ("zpy", "aby", "iny"):
        u.add("y")
    return frozenset(u)


def defs_i(i):
    n = i.name
    d = set(defs_of(n))
    if n in REG_DEF:
        d.add(REG_DEF[n])
    if n in ("ASL", "LSR", "ROL", "ROR") and i.mode == "acc":
        d.add("a")
    return frozenset(d)


def writes_reg(i):
    """Registri modificati da un'istruzione (per la fusione delle condizioni)."""
    n = i.name
    if n in ("ASL", "LSR", "ROL", "ROR") and i.mode == "acc":
        return {"A"}
    if n in REG_WRITES:
        return {REG_WRITES[n]}
    if n in ("JSR", "JMP", "RTS", "RTI", "BRK"):
        return {"A", "X", "Y"}
    return set()


# ------------------------------------------------------------------ nomi simbolici
class Symbols:
    """Nomi per indirizzi RAM/ROM e funzioni. Sorgente: analysis/symbols.tsv (modificabile) + euristiche."""

    def __init__(self, hw, path=None):
        self.hw = hw
        self.ram = {}      # addr -> nome
        self.func = {}     # (unit, addr) -> nome
        self.auto = {}
        if path and os.path.exists(path):
            for l in open(path, encoding="utf-8"):
                l = l.split("#")[0].strip()
                if not l:
                    continue
                p = l.split()
                if len(p) >= 3 and p[0] == "R":
                    self.ram[int(p[1], 16)] = self.clean(p[2])
                elif len(p) >= 3 and p[0] == "F":
                    u, a = p[1].split(":")
                    self.func[(int(u), int(a, 16))] = self.clean(p[2])

    @staticmethod
    def clean(n):
        return re.sub(r"\W", "_", n)

    def mem(self, a):
        """Nome di un indirizzo (RAM/WRAM/ROM/hardware)."""
        if a in self.hw:
            return self.hw[a]
        if a in self.ram:
            return self.ram[a]
        if a in self.auto:
            return self.auto[a]
        if a < 0x100:
            return "zp_%02X" % a
        if a < 0x800:
            return "ram_%04X" % a
        if 0x6000 <= a < 0x8000:
            return "wram_%04X" % a
        if a >= 0x8000:
            return "rom_%04X" % a
        return "io_%04X" % a


# ------------------------------------------------------------------ operandi
class Ctx:
    """Impostazioni di emissione condivise."""

    def __init__(self, runnable, syms, exact, ipa):
        self.runnable = runnable
        self.syms = syms
        self.exact = exact       # flag: nessuna potatura
        self.ipa = ipa           # ret_live dai chiamanti noti (mondo chiuso)


def opv(i):
    return i.ops[0] if len(i.ops) == 1 else (i.ops[0] | (i.ops[1] << 8)) if i.ops else 0


def mem_read(c, i):
    """Espressione di lettura dell'operando di i (valore uint8)."""
    m = i.mode
    a = opv(i)
    nm = c.syms.mem
    if m == "imm":
        return "0x%02X" % i.ops[0]
    if c.runnable:
        return "nes_read(%s)" % addr_expr(c, i)
    # leggibile
    if m in ("zpg", "abs"):
        return nm(a)
    if m in ("zpx", "abx"):
        return "%s[X]" % nm(a)
    if m in ("zpy", "aby"):
        return "%s[Y]" % nm(a)
    if m == "inx":
        return "*ptr_%02X[X]" % a
    if m == "iny":
        return "ptr_%02X[Y]" % a
    raise ValueError(m)


def addr_expr(c, i):
    """Indirizzo dell'operando (per nes_write / RMW)."""
    m = i.mode
    a = opv(i)
    nm = c.syms.mem
    if m == "zpg":
        return "0x%02X" % a if a not in c.syms.ram and a not in c.syms.auto else nm(a)
    if m == "abs":
        return nm(a) if (a in c.syms.hw or a in c.syms.ram or a in c.syms.auto) else "0x%04X" % a
    if m == "zpx":
        return "(uint8_t)(%s + rX)" % (nm(a) if a in c.syms.ram or a in c.syms.auto else "0x%02X" % a)
    if m == "zpy":
        return "(uint8_t)(%s + rY)" % (nm(a) if a in c.syms.ram or a in c.syms.auto else "0x%02X" % a)
    if m == "abx":
        return "(uint16_t)(%s + rX)" % (nm(a) if (a in c.syms.ram or a in c.syms.auto) else "0x%04X" % a)
    if m == "aby":
        return "(uint16_t)(%s + rY)" % (nm(a) if (a in c.syms.ram or a in c.syms.auto) else "0x%04X" % a)
    if m == "inx":
        return "nes_read16zp((uint8_t)(0x%02X + rX))" % a
    if m == "iny":
        return "(uint16_t)(nes_read16zp(0x%02X) + rY)" % a
    raise ValueError(m)


def mem_write(c, i, val):
    if c.runnable:
        return "nes_write(%s, %s);" % (addr_expr(c, i), val)
    return "%s = %s;" % (mem_read(c, i), val)


# ------------------------------------------------------------------ istruzioni -> C
READABLE = False        # impostato da decompile.py: sapore leggibile


def nz(v, L):
    s = []
    if "N" in L:
        s.append("N = (%s) >> 7;" % v if READABLE else "g_cpu.N = ((uint8_t)(%s) >> 7) & 1;" % v)
    if "Z" in L:
        s.append("Z = (%s) == 0;" % v if READABLE else "g_cpu.Z = ((uint8_t)(%s) == 0);" % v)
    return s


def rmw(c, i, L, kind):
    acc = i.mode == "acc"
    if READABLE and not c.runnable and not (L & set("NZC")) and kind in ("INC", "DEC", "ASL", "LSR"):
        tgt = "rA" if acc else mem_read(c, i)
        return {"INC": "%s++;", "DEC": "%s--;", "ASL": "%s <<= 1;", "LSR": "%s >>= 1;"}[kind] % tgt
    if READABLE and not c.runnable and kind in ("INC", "DEC") and not (L & set("C")):
        tgt = "rA" if acc else mem_read(c, i)
        return " ".join([{"INC": "%s++;", "DEC": "%s--;"}[kind] % tgt] + nz(tgt, L))
    st = []
    tmp = getattr(i, "temp", None)
    cur = "rA" if acc else "v"
    if not acc:
        if c.runnable:
            st.append("uint16_t a = %s;" % addr_expr(c, i))
            st.append("uint8_t v = nes_read(a);")
        else:
            st.append("uint8_t v = %s;" % mem_read(c, i))
    if kind == "ASL":
        if "C" in L: st.append("g_cpu.C = (%s >> 7) & 1;" % cur)
        st.append("%s = (uint8_t)(%s << 1);" % (cur, cur))
    elif kind == "LSR":
        if "C" in L: st.append("g_cpu.C = %s & 1;" % cur)
        st.append("%s >>= 1;" % cur)
    elif kind == "ROL":
        cc = getattr(i, "carry_const", None)
        st.append("{ uint8_t c = %s;" % ("g_cpu.C" if cc is None else str(cc)))
        if "C" in L: st.append("g_cpu.C = (%s >> 7) & 1;" % cur)
        st.append("%s = (uint8_t)((%s << 1) | c); }" % (cur, cur))
    elif kind == "ROR":
        cc = getattr(i, "carry_const", None)
        st.append("{ uint8_t c = %s;" % ("g_cpu.C" if cc is None else str(cc)))
        if "C" in L: st.append("g_cpu.C = %s & 1;" % cur)
        st.append("%s = (uint8_t)((%s >> 1) | (c << 7)); }" % (cur, cur))
    elif kind == "INC":
        st.append("%s = (uint8_t)(%s + 1);" % (cur, cur))
    elif kind == "DEC":
        st.append("%s = (uint8_t)(%s - 1);" % (cur, cur))
    if tmp:
        st.append("%s = %s;" % (tmp, cur))
    if not acc:
        st.append("nes_write(a, v);" if c.runnable else "%s = v;" % mem_read(c, i))
    st += nz(cur, L)
    if acc:
        return " ".join(st)
    return "{ " + " ".join(st) + " }" if c.runnable or len(st) > 3 else " ".join(st)


def emit_ins(c, i, L):
    """Testo C di un'istruzione non di controllo; None = non modellata. L = flag vivi dopo di essa."""
    n = i.name
    tmp = getattr(i, "temp", None)
    reg = {"LDA": "rA", "LDX": "rX", "LDY": "rY"}
    if n in reg:
        return " ".join(["%s = %s;" % (reg[n], mem_read(c, i))] + nz(reg[n], L))
    if n == "STA": return mem_write(c, i, "rA")
    if n == "STX": return mem_write(c, i, "rX")
    if n == "STY": return mem_write(c, i, "rY")
    simple = {"TAX": ("rX", "rA"), "TAY": ("rY", "rA"), "TXA": ("rA", "rX"), "TYA": ("rA", "rY")}
    if n in simple:
        d, s = simple[n]
        return " ".join(["%s = %s;" % (d, s)] + nz(d, L))
    if n in ("INX", "INY", "DEX", "DEY"):
        r = "rX" if n.endswith("X") else "rY"
        return " ".join(["%s%s;" % (r, "++" if n.startswith("IN") else "--")] + nz(r, L))
    if n in ("AND", "ORA", "EOR"):
        op = {"AND": "&=", "ORA": "|=", "EOR": "^="}[n]
        return " ".join(["rA %s %s;" % (op, mem_read(c, i))] + nz("rA", L))
    cc = getattr(i, "carry_const", None)
    cin = "g_cpu.C" if cc is None else str(cc)
    if n == "ADC":
        rdv = mem_read(c, i)
        if not (L & set("CVNZ")):
            return "rA = (uint8_t)(rA + %s%s);" % (rdv, "" if cc == 0 else " + " + cin)
        if READABLE and not c.runnable:
            k = "" if cc == 0 else " + " + cin
            out = []
            if "V" in L: out.append("V = (~(rA ^ %s) & (rA ^ (rA + %s%s)) & 0x80) != 0;" % (rdv, rdv, k))
            if "C" in L: out.append("C = (rA + %s%s) > 0xFF;" % (rdv, k))
            out.append("rA = rA + %s%s;" % (rdv, k))
            return " ".join(out + nz("rA", L))
        s = ["uint8_t m = %s;" % rdv, "uint16_t r = rA + m + %s;" % cin]
        if "C" in L: s.append("g_cpu.C = r > 0xFF;")
        if "V" in L: s.append("g_cpu.V = (~(rA ^ m) & (rA ^ r) & 0x80) != 0;")
        s.append("rA = (uint8_t)r;")
        s += nz("rA", L)
        return "{ " + " ".join(s) + " }"
    if n == "SBC":
        rdv = mem_read(c, i)
        if not (L & set("CVNZ")):
            return "rA = (uint8_t)(rA - %s%s);" % (rdv, "" if cc == 1 else " - (1 - %s)" % cin)
        if READABLE and not c.runnable:
            k = "" if cc == 1 else " - (1 - %s)" % cin
            out = []
            if "V" in L: out.append("V = ((rA ^ %s) & (rA ^ (rA - %s%s)) & 0x80) != 0;" % (rdv, rdv, k))
            if "C" in L: out.append("C = (rA - %s%s) >= 0;" % (rdv, k))
            out.append("rA = rA - %s%s;" % (rdv, k))
            return " ".join(out + nz("rA", L))
        s = ["uint8_t m = %s;" % rdv, "int16_t r = rA - m - (1 - %s);" % cin]
        if "C" in L: s.append("g_cpu.C = r >= 0;")
        if "V" in L: s.append("g_cpu.V = ((rA ^ m) & (rA ^ r) & 0x80) != 0;")
        s.append("rA = (uint8_t)r;")
        s += nz("rA", L)
        return "{ " + " ".join(s) + " }"
    if n in ("CMP", "CPX", "CPY"):
        r = REG_OF[n]
        if tmp:                                        # operando in memoria letto una volta sola
            s = ["%s = %s;" % (tmp, mem_read(c, i))]
            m = tmp
        else:
            s = []
            m = mem_read(c, i) if i.mode == "imm" else "m"
            if i.mode != "imm":
                s.append("uint8_t m = %s;" % mem_read(c, i))
        if READABLE and not c.runnable:
            out = []
            if "C" in L: out.append("C = %s >= %s;" % (r, m))
            if "Z" in L: out.append("Z = %s == %s;" % (r, m))
            if "N" in L: out.append("N = (uint8_t)(%s - %s) >> 7;" % (r, m))
            return " ".join(out)
        if "C" in L: s.append("g_cpu.C = %s >= %s;" % (r, m))
        if "N" in L or "Z" in L:
            s.append("uint8_t t = (uint8_t)(%s - %s);" % (r, m))
            s += nz("t", L)
        return "{ " + " ".join(s) + " }" if any(x.startswith("uint8_t") for x in s) else " ".join(s)
    if n == "BIT" and READABLE and not c.runnable:
        m = mem_read(c, i)
        out = []
        if "Z" in L: out.append("Z = (rA & %s) == 0;" % m)
        if "N" in L: out.append("N = %s >> 7;" % m)
        if "V" in L: out.append("V = (%s >> 6) & 1;" % m)
        return " ".join(out)
    if n == "BIT":
        if tmp:
            s = ["%s = %s;" % (tmp, mem_read(c, i))]
            m = tmp
        else:
            s = ["uint8_t m = %s;" % mem_read(c, i)]
            m = "m"
        if "Z" in L: s.append("g_cpu.Z = (rA & %s) == 0;" % m)
        if "N" in L: s.append("g_cpu.N = (%s >> 7) & 1;" % m)
        if "V" in L: s.append("g_cpu.V = (%s >> 6) & 1;" % m)
        return "{ " + " ".join(s) + " }" if not tmp else " ".join(s)
    if n in ("ASL", "LSR", "ROL", "ROR", "INC", "DEC"):
        return rmw(c, i, L, n)
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


# ------------------------------------------------------------------ condizioni
def cond_pair(i):
    """Condizione (testo, testo negato) del salto i letta dai flag materializzati."""
    f, v = BR_FLAG[i.name]
    t = "g_cpu.%s" % f
    return (t, "!" + t) if v else ("!" + t, t)


def fused_cond(br, setter, c):
    """(testo, negato) per il salto br derivato dal setter (istruzione che ha definito il flag)."""
    f, v = BR_FLAG[br.name]
    n = setter.name
    tmp = getattr(setter, "temp", None)

    def pos(neg_t, pos_t):
        return (pos_t, neg_t) if v else (neg_t, pos_t)
    if n in ("CMP", "CPX", "CPY"):
        r = REG_OF[n]
        m = tmp if tmp else mem_read(c, setter)
        if f == "Z":
            return pos("(%s != %s)" % (r, m), "(%s == %s)" % (r, m))
        if f == "C":
            return pos("(%s < %s)" % (r, m), "(%s >= %s)" % (r, m))
        if f == "N":
            return pos("((int8_t)(uint8_t)(%s - %s) >= 0)" % (r, m), "((int8_t)(uint8_t)(%s - %s) < 0)" % (r, m))
    if n == "BIT":
        m = tmp if tmp else mem_read(c, setter)
        if f == "Z":
            return pos("((rA & %s) != 0)" % m, "((rA & %s) == 0)" % m)
        if f == "N":
            return pos("!(%s & 0x80)" % m, "(%s & 0x80)" % m)
        if f == "V":
            return pos("!(%s & 0x40)" % m, "(%s & 0x40)" % m)
    if n in NZ_DEF:
        if n in ("INC", "DEC"):
            r = tmp if tmp else mem_read(c, setter)
        else:
            r = {"LDA": "rA", "AND": "rA", "ORA": "rA", "EOR": "rA", "PLA": "rA", "TXA": "rA", "TYA": "rA", "LDX": "rX", "TAX": "rX",
                 "INX": "rX", "DEX": "rX", "TSX": "rX", "LDY": "rY", "TAY": "rY", "INY": "rY", "DEY": "rY"}[n]
        if f == "Z":
            return pos("(%s != 0)" % r, "(%s == 0)" % r)
        if f == "N":
            return pos("((int8_t)%s >= 0)" % r, "((int8_t)%s < 0)" % r)
    return None


def find_setter(k, flag, body, preds):
    """Setter del flag per il salto in k: catena a predecessore unico, con i registri usati intatti."""
    cur = k
    between = []
    for _ in range(64):
        ps = preds.get(cur, [])
        if len(ps) != 1:
            return None
        p = body[ps[0]]
        if flag in defs_of(p.name):
            return p, between
        if p.name in ("JSR", "JMP", "RTS", "RTI", "BRK") or p.extra is not None:
            return None
        between.append(p)
        cur = (p.u, p.off)
    return None


def setter_inputs(setter):
    n = setter.name
    if n in ("CMP", "CPX", "CPY"):
        return {REG_OF[n][1]}
    if n == "BIT":
        return {"A"}
    if n in NZ_DEF:
        return {"A"} if n in ("LDA", "AND", "ORA", "EOR", "PLA", "TXA", "TYA") else \
               ({"X"} if n in ("LDX", "TAX", "INX", "DEX", "TSX") else ({"Y"} if n in ("LDY", "TAY", "INY", "DEY") else set()))
    return None


def mark_fusion(c, body, preds):
    """Segna i salti condizionati che si possono leggere dagli operandi del setter invece che dai flag."""
    for k, br in body.items():
        br.fused = None
        br.temp = None
        if br.name not in BR_FLAG:
            continue
        flag = BR_FLAG[br.name][0]
        if flag not in "NZCV":
            continue
        r = find_setter(k, flag, body, preds)
        if r is None:
            continue
        setter, between = r
        ins_ = setter_inputs(setter)
        if ins_ is None:
            continue
        # registri da mantenere: quelli letti dalla condizione (CMP: il registro; NZ: il registro risultato)
        if any(writes_reg(b) & ins_ for b in between):
            continue
        if c.runnable and (setter.name in ("BIT", "INC", "DEC") or (setter.name in ("CMP", "CPX", "CPY") and setter.mode != "imm")):
            if not getattr(setter, "temp", None):
                setter.temp = "t_%04X" % setter.pc
        if setter.name in ("INC", "DEC") and setter.mode == "acc":
            continue
        # BIT usa A: registrato in ins_; INC/DEC: valore in temp
        if fused_cond(br, setter, c) is None:
            continue
        br.fused = setter


def mark_carry(body, preds):
    """ADC/SBC/ROL/ROR il cui carry in ingresso e' una costante (CLC/SEC nella catena a predecessore unico)."""
    for k, i in body.items():
        i.carry_const = None
        if i.name in ("ADC", "SBC", "ROL", "ROR"):
            r = find_setter(k, "C", body, preds)
            if r and r[0].name in ("CLC", "SEC"):
                i.carry_const = 1 if r[0].name == "SEC" else 0


def reg_value(c, body, preds, k, reg):
    """Espressione leggibile del registro reg ('a','x','y') subito prima dell'istruzione k, o 'A'/'X'/'Y' se ignota."""
    cur = k
    name = reg.upper()
    for _ in range(24):
        ps = preds.get(cur, [])
        if len(ps) != 1:
            break
        p = body[ps[0]]
        d = defs_i(p)
        if reg in d:
            n = p.name
            if n in ("LDA", "LDX", "LDY"):
                return mem_read(c, p) if p.mode in ("imm", "zpg", "abs") else "%s" % mem_read(c, p)
            if n in ("TAX", "TAY"): return "A"
            if n in ("TXA",): return "X"
            if n in ("TYA",): return "Y"
            break
        if p.name in ("JSR", "JMP", "RTS", "RTI", "BRK") or p.extra is not None:
            break
        cur = (p.u, p.off)
    return name


# ------------------------------------------------------------------ analisi flag tra funzioni
class Summaries:
    def __init__(self, fn_ok, rom=None):
        self.entry_live = {e: frozenset() for e in fn_ok}
        self.must_def = {e: ALL for e in fn_ok}
        self.ret_live = {e: ALL for e in fn_ok}
        self.may_def_regs = {}            # funzione -> registri che puo' modificare (a,x,y)
        self.reads_mode = False           # True: ret_live trattato come vuoto (si calcolano solo i flag LETTI dalla funzione)
        self.by_addr = collections.defaultdict(list)   # indirizzo CPU -> funzioni con quell'ingresso (per le chiamate dinamiche)
        if rom is not None:
            for e in fn_ok:
                self.by_addr[rom.base[e[0]] + e[1]].append(e)

    def dyn_cands(self, i):
        """Funzioni note all'indirizzo di una JSR/JMP dinamica (vuoto se sconosciuto)."""
        if i.name == "JSR" and i.kind and i.kind[0] == "dyn":
            return self.by_addr.get(i.kind[1], [])
        if i.name == "JMP" and i.mode == "abs" and i.jcallee is None:
            return self.by_addr.get(opv(i), [])
        return []


def call_target(i, fn):
    """Funzione (decompilata o no) chiamata da una JSR con bersaglio noto (o None)."""
    g = i.callee if i.name == "JSR" else None
    if g is not None and i.kind and i.kind[0] != "table" and g in fn:
        return g
    return None


def tail_target(i, rom, fn_ok):
    if i.name == "JMP" and i.mode == "abs":
        a = opv(i)
        tu, to = rom.unit_of(a, i.u)
        if tu is not None and (tu, to) in fn_ok and not (0xC000 <= a < 0xE000 and rom.base[i.u] != 0xC000):
            return (tu, to)
    return None


def tail_all(i, fn):
    """Bersaglio noto di un JMP fuori dalla funzione (per le sintesi dei flag)."""
    g = i.jcallee if i.name == "JMP" and i.mode == "abs" else None
    return g if g is not None and g in fn else None


def live_in_of(i, body, succ_live, S, entry, rom, fn_ok, jmp_internal):
    """live_in di un'istruzione. succ_live = live_out."""
    n = i.name
    if n in BR_FLAG:
        u = frozenset() if getattr(i, "fused", None) else uses_of(n)
        return u | succ_live
    if n == "JSR":
        g = call_target(i, fn_ok)
        if g is None:
            cands = S.dyn_cands(i)
            if not cands:
                return ALL
            el = frozenset().union(*[S.entry_live[x] for x in cands])
            md = ALL
            for x in cands:
                md = md & S.must_def[x]
            return el | (succ_live - md)
        return S.entry_live[g] | (succ_live - S.must_def[g])
    if n == "RTS":
        return frozenset() if S.reads_mode else S.ret_live[entry]
    if n == "JMP":
        if jmp_internal:
            return succ_live
        g = tail_all(i, fn_ok)
        rl = frozenset() if S.reads_mode else S.ret_live[entry]
        if g is None:
            cands = S.dyn_cands(i)
            if not cands:
                return ALL
            el = frozenset().union(*[S.entry_live[x] for x in cands])
            md = ALL
            for x in cands:
                md = md & S.must_def[x]
            return el | (rl - md)
        return S.entry_live[g] | (rl - S.must_def[g])
    if n in ("RTI", "BRK"):
        return ALL
    u = uses_i(i)
    if getattr(i, "carry_const", None) is not None:
        u = u - {"C"}
    return u | (succ_live - defs_i(i))


def function_liveness(entry, body, successors, rom, fn_ok, S, exact):
    """live_out per istruzione; ritorna anche la live_in dell'ingresso."""
    for i in body.values():
        i.live_out = frozenset()
    order = sorted(body, key=lambda k: -k[1])
    changed = True
    while changed:
        changed = False
        for k in order:
            i = body[k]
            succ = [s for s in successors(i) if s in body]
            out = frozenset()
            for s in succ:
                si = body[s]
                jm = si.name == "JMP" and any(x in body for x in successors(si))
                out |= live_in_of(si, body, si.live_out, S, entry, rom, fn_ok, jm)
            if is_exit_ins(i, body, successors):
                if not S.reads_mode:
                    out |= ALL if (i.name != "RTS" and i.name != "JMP") or i.extra is not None else frozenset()
                elif (i.name not in ("RTS", "JMP")) or i.extra is not None:
                    out |= ALL
            if out != i.live_out:
                i.live_out = out
                changed = True
    e = body[entry]
    jm = e.name == "JMP" and any(x in body for x in successors(e))
    return live_in_of(e, body, e.live_out, S, entry, rom, fn_ok, jm)


def is_exit_ins(i, body, successors):
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


def must_def_of(entry, body, successors, rom, fn_ok, S):
    """Flag definiti su TUTTI i cammini fino alle uscite (RTS / tail call)."""
    D = {}
    work = [(entry, frozenset())]
    exits = []
    while work:
        k, din = work.pop()
        if k not in body:
            continue
        if k in D:
            new = D[k] & din
            if new == D[k]:
                continue
            D[k] = new
            din = new
        else:
            D[k] = din
        i = body[k]
        n = i.name
        dout = din | defs_i(i)
        if n == "JSR":
            g = call_target(i, fn_ok)
            if g:
                dout = din | S.must_def[g]
            else:
                cands = S.dyn_cands(i)
                md = ALL
                for x in cands:
                    md = md & S.must_def[x]
                dout = din | (md if cands else frozenset())
        succ = [s for s in successors(i) if s in body]
        if n == "RTS":
            exits.append(dout)
        elif n == "JMP" and not succ:
            g = tail_all(i, fn_ok)
            exits.append(dout | (S.must_def[g] if g else frozenset()))
        elif is_exit_ins(i, body, successors) and not succ:
            exits.append(frozenset())
        for s in succ:
            work.append((s, dout))
    if not exits:
        return frozenset()
    r = ALL
    for x in exits:
        r &= x
    return frozenset(r)


def ipa(c, fn_emit, successors, rom, open_fns, fn_ok):
    """Punto fisso di entry_live / must_def / ret_live su tutte le funzioni."""
    S = Summaries(fn_ok, rom)
    if c.exact:
        for e, body in fn_ok.items():
            for i in body.values():
                i.fused = None
        return S
    callers = collections.defaultdict(list)     # g -> [(funzione, istruzione, tipo)]
    for e, body in fn_ok.items():
        for i in body.values():
            g = call_target(i, fn_ok)
            if g:
                callers[g].append((e, i, "call"))
            elif i.name == "JSR":
                for x in S.dyn_cands(i):
                    callers[x].append((e, i, "call"))
            t = tail_all(i, fn_ok)
            if t:
                callers[t].append((e, i, "tail"))
            elif i.name == "JMP":
                for x in S.dyn_cands(i):
                    callers[x].append((e, i, "tail"))
    converged = False
    for it in range(30):
        changed = False
        S.reads_mode = True
        for e, body in fn_ok.items():
            el = function_liveness(e, body, successors, rom, fn_ok, S, c.exact)
            if el != S.entry_live[e]:
                S.entry_live[e] = el
                changed = True
        S.reads_mode = False
        for e, body in fn_ok.items():
            md = must_def_of(e, body, successors, rom, fn_ok, S)
            if md != S.must_def[e]:
                S.must_def[e] = md
                changed = True
        if c.ipa:
            for e, body in fn_ok.items():             # live_out reali (con ret_live corrente) per i chiamanti
                function_liveness(e, body, successors, rom, fn_ok, S, c.exact)
            for g in fn_ok:
                if g in open_fns:
                    continue
                rl = frozenset()
                for (e, i, kind) in callers.get(g, []):
                    rl |= i.live_out if kind == "call" else S.ret_live[e]
                if not callers.get(g):
                    rl = ALL
                if rl != S.ret_live[g]:
                    S.ret_live[g] = rl
                    changed = True
        if not changed:
            converged = True
            break
    if not converged:                                  # sicurezza: nessuna potatura
        for e in fn_ok:
            S.entry_live[e] = ALL
            S.must_def[e] = frozenset()
            S.ret_live[e] = ALL
    S.reads_mode = False
    for e, body in fn_ok.items():
        function_liveness(e, body, successors, rom, fn_ok, S, c.exact)
    # registri modificabili: definiti nel corpo o da un chiamato
    md = {e: {r for i in b.values() for r in defs_i(i) if r in "axy"} for e, b in fn_ok.items()}
    for _ in range(8):
        ch = False
        for e, b in fn_ok.items():
            for i in b.values():
                g = call_target(i, fn_ok) or tail_all(i, fn_ok)
                if g and not md[g] <= md[e]:
                    md[e] |= md[g]
                    ch = True
        if not ch:
            break
    S.may_def_regs = md
    return S


# ------------------------------------------------------------------ blocchi base
class Block:
    __slots__ = ("key", "insns", "term", "kind", "succs", "taken", "fall", "target", "cond", "esc", "ins_term")

    def __init__(self, key):
        self.key = key
        self.insns = []
        self.term = None


def make_blocks(entry, body, successors):
    preds = collections.defaultdict(list)
    for k, i in body.items():
        for s in successors(i):
            if s in body and k not in preds[s]:
                preds[s].append(k)
    leaders = {entry}
    for k, i in body.items():
        succ = [s for s in successors(i) if s in body]
        for s in succ:
            p = preds[s]
            if len(p) != 1 or len([x for x in successors(i) if x in body]) != 1 or i.name in ("JMP",) or i.name in BR_FLAG:
                leaders.add(s)
    blocks = {}
    for L in sorted(leaders):
        b = Block(L)
        cur = L
        while True:
            i = body[cur]
            succ = [s for s in successors(i) if s in body]
            n = i.name
            is_term = n in BR_FLAG or n in ("JMP", "RTS", "RTI", "BRK") or (n == "JSR" and i.kind and i.kind[0] == "table") \
                or i.extra is not None
            if is_term:
                b.ins_term = i
                break
            b.insns.append(i)
            nxt = succ[0] if succ else None
            if nxt is None or nxt in leaders:
                b.ins_term = None
                if nxt is None:                    # prosegue in codice non decodificato: si conserva il suo indirizzo
                    nxt = (i.u, i.kind[2]) if (n == "JSR" and i.kind and i.kind[0] == "inline") else (i.u, i.off + i.ln)
                b.fall = nxt
                break
            cur = nxt
        blocks[L] = b
    return blocks, preds


def block_succs(b, body, successors):
    """(lista di chiavi successore dentro il corpo)."""
    if b.ins_term is None:
        return [b.fall] if b.fall in body else []
    i = b.ins_term
    return [s for s in successors(i) if s in body]


def compute_dom(nodes, succs, entry):
    """Insiemi di dominatori (algoritmo iterativo, funzioni piccole)."""
    preds = collections.defaultdict(list)
    for n in nodes:
        for s in succs[n]:
            preds[s].append(n)
    dom = {n: set(nodes) for n in nodes}
    dom[entry] = {entry}
    changed = True
    while changed:
        changed = False
        for n in nodes:
            if n == entry:
                continue
            ps = [dom[p] for p in preds[n] if p in dom]
            new = set.intersection(*ps) | {n} if ps else {n}
            if new != dom[n]:
                dom[n] = new
                changed = True
    return dom


def compute_pdom(nodes, succs):
    """Post-dominatori con un'uscita virtuale 'EXIT' (blocchi senza successori)."""
    EXIT = "EXIT"
    rs = {n: list(succs[n]) or [EXIT] for n in nodes}
    allp = set(nodes) | {EXIT}
    pd = {n: set(allp) for n in nodes}
    pd[EXIT] = {EXIT}
    changed = True
    while changed:
        changed = False
        for n in nodes:
            ps = [pd[s] for s in rs[n]]
            new = set.intersection(*ps) | {n}
            if new != pd[n]:
                pd[n] = new
                changed = True
    return pd


def ipdom_of(n, pd):
    """Post-dominatore immediato di n (None se e' l'uscita virtuale)."""
    strict = pd[n] - {n}
    best = None
    for c in strict:
        if c == "EXIT":
            continue
        # c e' immediato se domina strettamente tutti gli altri strict (cioe' pd[c] contiene tutti gli altri)
        if strict - {c} <= pd[c]:
            best = c
    return best


# ------------------------------------------------------------------ strutturazione e stampa
class Fn:
    """Stato di emissione di una funzione."""

    def __init__(self, c, entry, body, successors, rom, fn_ok, fname_of, fn_index, preds):
        self.c = c
        self.entry = entry
        self.body = body
        self.rom = rom
        self.fn_ok = fn_ok
        self.fname_of = fname_of
        self.successors = successors
        self.needs_s0 = False
        self.temps = set()
        self.goto_targets = set()
        self.emitted = set()
        self.u = entry[0]
        self.preds = preds

    # -- istruzioni
    def step(self, i):
        return "NB_STEP(0x%04X, 0x%02X)" % (i.pc, i.op)

    def ins_lines(self, i):
        """Righe C per un'istruzione non terminale."""
        c = self.c
        n = i.name
        st = None
        if n == "JSR":
            st = self.call_stmt(i)
        else:
            st = emit_ins(c, i, i.live_out)
            if st is None:
                self.needs_s0 = True
                st = "ESCAPE(0x%04X);" % i.pc
        if getattr(i, "temp", None):
            self.temps.add(i.temp)
        pre = (self.step(i) + "; ") if c.runnable else ""
        if not st and not c.runnable:
            return []
        return [(pre + st).rstrip()]

    def call_stmt(self, i):
        a = i.ops[0] | (i.ops[1] << 8)
        ret = (i.pc + 2) & 0xFFFF
        kd = i.kind
        nm = self.c.syms
        if kd is None or kd[0] == "dyn":
            self.needs_s0 = True
            return "JSR_DYN(0x%04X, 0x%04X);" % (a, ret) if self.c.runnable else "call_dyn(%s);" % self.faddr(a)
        if kd[0] == "inline":
            cont = self.rom.base[self.u] + kd[2]
            if self.c.runnable:
                return "JSR_INLINE(0x%04X, 0x%04X, 0x%04X);" % (a, ret, cont)
            n_in = kd[2] - (i.off + i.ln)
            raw = self.rom.prg[self.u * 8192 + i.off + i.ln:self.u * 8192 + kd[2]]
            if n_in == 3 and (raw[0] & 0x80) and raw[0] & 0x3F < self.rom.nunits:
                v = raw[0] & 0x3F
                tgt = raw[1] | (raw[2] << 8)
                tu2 = self.rom.base[v]
                nm2 = self.fname_of((v, tgt - tu2)) if (v, tgt - tu2) in self.fn_ok else "0x%04X" % tgt
                return "%s(unit_%d, %s);  /* far call */" % (self.faddr(a), v, nm2)
            data = ", ".join("0x%02X" % b for b in raw[:12])
            return "%s_inline(%s);  /* %d byte inline */" % (self.faddr(a), data, n_in)
        if kd[0] == "table":
            self.needs_s0 = True
            return "JSR_TABLE(0x%04X, 0x%04X);" % (a, ret)
        tgt = (kd[1], kd[2])
        if tgt in self.fn_ok:
            if self.c.runnable:
                return "JSR(%s, 0x%04X);" % (self.fname_of(tgt), ret)
            S = getattr(self.c, "S", None)
            args = ""
            outs = ""
            if S is not None and tgt in S.entry_live:
                params = [r for r in "axy" if r in S.entry_live[tgt]]
                args = ", ".join("%s=%s" % (r.upper(), reg_value(self.c, self.body, self.preds, (i.u, i.off), r)) for r in params)
                o = [r.upper() for r in "axy" if r in S.ret_live[tgt] and r in S.may_def_regs.get(tgt, set())]
                if o:
                    outs = "  /* -> %s */" % ", ".join(o)
            return "%s(%s);%s" % (self.fname_of(tgt), args, outs)
        self.needs_s0 = True
        return "JSR_DYN(0x%04X, 0x%04X);" % (a, ret) if self.c.runnable else "call_dyn(%s);" % self.faddr(a)

    def faddr(self, a):
        tu, to = self.rom.unit_of(a, self.u)
        if tu is not None and (tu, to) in self.fn_ok:
            return self.fname_of((tu, to))
        return "0x%04X" % a

    def term_stmt(self, b):
        """(righe prima, condizione (pos, neg) | None, tipo)."""
        i = b.ins_term
        c = self.c
        n = i.name
        pre = (self.step(i) + "; ") if c.runnable else ""
        if n in BR_FLAG:
            if getattr(i, "fused", None):
                return pre, fused_cond(i, i.fused, c)
            return pre, cond_pair(i)
        return pre, None

    def terminal_text(self, b):
        """Testo dell'istruzione terminale non condizionale, oppure None se e' una 'goto' interna."""
        i = b.ins_term
        n = i.name
        c = self.c
        pre = (self.step(i) + "; ") if c.runnable else ""
        if n == "RTS":
            return pre + "RTS();" if c.runnable else "return;"
        if n in ("RTI", "BRK"):
            self.needs_s0 = True
            return pre + "ESCAPE(0x%04X);" % i.pc
        if n == "JSR":       # tabella
            a = opv(i)
            self.needs_s0 = True
            return pre + ("JSR_TABLE(0x%04X, 0x%04X);" % (a, (i.pc + 2) & 0xFFFF) if c.runnable
                          else "jump_table_%s();  /* tabella di puntatori inline */" % self.faddr(a))
        if n == "JMP":
            a = opv(i)
            if i.mode == "ind":
                self.needs_s0 = True
                return pre + ("JMP_IND(0x%04X);" % a if c.runnable else "goto *(%s);  /* salto indiretto */" % c.syms.mem(a))
            g = tail_target(i, self.rom, self.fn_ok)
            if g:
                return pre + ("JMP_FN(%s);" % self.fname_of(g) if c.runnable else "return %s();  /* tail call */" % self.fname_of(g))
            self.needs_s0 = True
            return pre + ("JMP_DYN(0x%04X);" % a if c.runnable else "return call_dyn(%s);  /* tail call */" % self.faddr(a))
        if i.extra is not None:
            return None
        return None


def render_function(c, entry, body, successors, rom, fn_ok, fname_of, fn_index, sym_names=None):
    blocks, preds = make_blocks(entry, body, successors)
    F = Fn(c, entry, body, successors, rom, fn_ok, fname_of, fn_index, preds)
    keys = sorted(blocks)
    succs = {k: block_succs(blocks[k], body, successors) for k in keys}
    dom = compute_dom(keys, succs, entry)
    pd = compute_pdom(keys, succs)
    bpreds = collections.defaultdict(list)
    for k in keys:
        for s in succs[k]:
            bpreds[s].append(k)
    loops = {}                                   # header -> insieme di blocchi del ciclo naturale
    for l in keys:
        for h in succs[l]:
            if h in dom[l]:                      # arco all'indietro l -> h
                bodyset = {h}
                stack = [l]
                while stack:
                    x = stack.pop()
                    if x in bodyset:
                        continue
                    bodyset.add(x)
                    stack.extend(bpreds[x])
                loops.setdefault(h, set()).update(bodyset)
    emitted = F.emitted
    ind = "    "
    base = rom.base[entry[0]]

    def label_of(x):
        return "L_%04X" % (base + x[1])

    def explicit(x, lp, depth):
        """Salto esplicito verso x (continue / break / goto)."""
        if lp:
            hdr, after, _, _ = lp[-1]
            if x == hdr:
                return [ind * depth + "continue;"]
            if x == after:
                return [ind * depth + "break;"]
        F.goto_targets.add(x)
        return [ind * depth + "goto %s;" % label_of(x)]

    def arrive(x, imps, lp, depth, lines):
        """Arrivo al blocco x: None = la regione finisce; altrimenti il blocco da emettere in sequenza."""
        if imps and x == imps[-1]:
            return None
        if x in imps or x in emitted:
            lines += explicit(x, lp, depth)
            return None
        if lp:
            hdr, after, _, bset = lp[-1]
            if x == hdr:
                lines.append(ind * depth + "continue;")
                return None
            if x == after:
                lines.append(ind * depth + "break;")
                return None
            if x not in bset:                    # esce dal ciclo per un'altra strada
                lines += explicit(x, lp, depth)
                return None
        return x

    def region(cur, imps, lp, depth, first=False):
        lines = []
        while cur is not None:
            if not first:
                cur = arrive(cur, imps, lp, depth, lines)
                if cur is None:
                    return lines
            else:
                if cur in emitted:
                    lines += explicit(cur, lp, depth)
                    return lines
            first = False
            if cur in loops and not (lp and lp[-1][0] == cur and lp[-1][2] == "inside"):
                lines_l, nxt = emit_loop(cur, imps, lp, depth)
                lines += lines_l
                cur = nxt
                if cur is not None:
                    first = False
                    cur = arrive(cur, imps, lp, depth, lines)
                    if cur is None:
                        return lines
                continue
            emitted.add(cur)
            b = blocks[cur]
            lines.append(("L", cur))
            for i in b.insns:
                for t in F.ins_lines(i):
                    lines.append(ind * depth + t)
            if b.ins_term is None:
                nxt = b.fall
                if nxt not in body:
                    F.needs_s0 = True
                    lines.append(ind * depth + "ESCAPE(0x%04X);  /* codice non decodificato */" % ((base + nxt[1]) & 0xFFFF))
                    return lines
                cur = nxt
                continue
            i = b.ins_term
            n = i.name
            if n in BR_FLAG:
                pre, cp = F.term_stmt(b)
                pos, neg = cp
                rel = i.ops[0] - 256 if i.ops[0] > 127 else i.ops[0]
                tkey = (i.u, i.off + 2 + rel)
                fkey = (i.u, i.off + i.ln)
                if pre:
                    lines.append(ind * depth + pre.rstrip())
                if tkey not in body:
                    F.needs_s0 = True
                    lines.append(ind * depth + "if (%s) ESCAPE(0x%04X);" % (pos, (i.pc + 2 + rel) & 0xFFFF))
                    if fkey not in body:
                        F.needs_s0 = True
                        lines.append(ind * depth + "ESCAPE(0x%04X);  /* codice non decodificato */" % ((i.pc + i.ln) & 0xFFFF))
                        return lines
                    cur = fkey
                    continue
                if fkey not in body:
                    F.needs_s0 = True
                    lines.append(ind * depth + "if (!(%s)) ESCAPE(0x%04X);" % (pos, (i.pc + i.ln) & 0xFFFF))
                    cur = tkey
                    continue
                j = ipdom_of(cur, pd)
                if lp and j is not None and j not in lp[-1][3] and j != lp[-1][1]:
                    j = None                     # il join sarebbe fuori dal ciclo: i rami usano break/goto
                if lp and j is not None and j == lp[-1][1]:
                    j = None
                imps2 = imps + (j,) if j is not None else imps + (object(),)
                then_l = arm(tkey, j, imps2, lp, depth + 1)
                else_l = arm(fkey, j, imps2, lp, depth + 1)
                emit_if(lines, pos, neg, then_l, else_l, depth)
                if j is None:
                    return lines
                cur = j
                continue
            t = F.terminal_text(b)
            if n == "JMP" and i.mode == "abs":
                a = opv(i)
                tu, to = rom.unit_of(a, i.u)
                if tu == i.u and (i.u, to) in body:
                    pre = (F.step(i) + "; ") if c.runnable else ""
                    if pre:
                        lines.append(ind * depth + pre.rstrip())
                    cur = (i.u, to)
                    continue
            if t is None:
                F.needs_s0 = True
                t = "ESCAPE(0x%04X);" % i.pc
            lines.append(ind * depth + t)
            return lines
        return lines

    def arm(t, j, imps2, lp, depth):
        """Ramo di un if: regione che finisce (in modo implicito) in j."""
        if t == j:
            return []
        lines = []
        nxt = arrive(t, imps2, lp, depth, lines)
        if nxt is None:
            return lines
        return lines + region(nxt, imps2, lp, depth, first=True)

    def strip(x):
        while x.startswith("(") and x.endswith(")"):
            d = 0
            ok = True
            for k, ch in enumerate(x):
                d += (ch == "(") - (ch == ")")
                if d == 0 and k < len(x) - 1:
                    ok = False
                    break
            if not ok:
                break
            x = x[1:-1]
        return x

    def emit_if(lines, pos, neg, then_l, else_l, depth):
        pos, neg = strip(pos), strip(neg)
        if not then_l and not else_l:
            return
        if not else_l:
            lines.append(ind * depth + "if (%s) {" % pos)
            lines += then_l
            lines.append(ind * depth + "}")
        elif not then_l:
            lines.append(ind * depth + "if (%s) {" % neg)
            lines += else_l
            lines.append(ind * depth + "}")
        else:
            lines.append(ind * depth + "if (%s) {" % pos)
            lines += then_l
            lines.append(ind * depth + "} else {")
            lines += else_l
            lines.append(ind * depth + "}")

    def emit_loop(h, imps, lp, depth):
        bodyset = loops[h]
        exits = []
        for x in sorted(bodyset):
            for s in succs[x]:
                if s not in bodyset and s not in exits:
                    exits.append(s)
        after = exits[0] if exits else None
        if len(exits) > 1:
            cnt = collections.Counter(s for x in bodyset for s in succs[x] if s not in bodyset)
            after = max(exits, key=lambda s: (cnt[s], -exits.index(s)))
        hb = blocks[h]
        lines = []
        if len(bodyset) == 1 and hb.ins_term is not None and hb.ins_term.name in BR_FLAG:
            i = hb.ins_term
            rel = i.ops[0] - 256 if i.ops[0] > 127 else i.ops[0]
            tkey = (i.u, i.off + 2 + rel)
            fkey = (i.u, i.off + i.ln)
            for stay, leave, use_neg in ((tkey, fkey, False), (fkey, tkey, True)):
                if stay == h and leave in body and leave not in bodyset:
                    pre, cp = F.term_stmt(hb)
                    cnd = cp[1] if use_neg else cp[0]
                    emitted.add(h)
                    lines.append(("L", h))
                    lines.append(ind * depth + "do {")
                    for x in hb.insns:
                        for t in F.ins_lines(x):
                            lines.append(ind * (depth + 1) + t)
                    if pre:
                        lines.append(ind * (depth + 1) + pre.rstrip())
                    lines.append(ind * depth + "} while (%s);" % cnd)
                    return lines, leave
        lp2 = lp + [(h, after, "inside", bodyset)]
        body_l = region(h, imps + (h,), lp2, depth + 1, first=True)
        lines.append(ind * depth + "while (1) {")
        lines += body_l
        lines.append(ind * depth + "}")
        return lines, after

    fl = region(entry, (), [], 1, first=True)
    while True:
        rest = [k for k in keys if k not in emitted]
        if not rest:
            break
        k = rest[0]
        F.goto_targets.add(k)
        fl += region(k, (), [], 1, first=True)
    res = []
    for x in fl:
        if isinstance(x, tuple):
            if x[1] in F.goto_targets:
                res.append("L_%04X:;" % (base + x[1][1]))
        else:
            res.append(x)
    return F, res
