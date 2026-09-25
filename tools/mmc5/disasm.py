#!/usr/bin/env python3
"""Disassemblatore per giochi MMC5: analisi ricorsiva per unita' PRG da 8K + copertura registrata.

  python disasm.py ROM.nes OUTDIR [cov1.bin cov2.bin ...]

Per ogni banco 8K scrive OUTDIR/unitNN.asm (sorgente in stile ca65 con etichette) e OUTDIR/summary.txt.
  * base della finestra CPU di ogni banco: da <cov>.win (finestra in cui il banco ha eseguito) oppure stimata
    dagli operandi JSR/JMP; l'ultima unita' sta a $E000 (fissa), quelle viste a $C000 nella copertura restano li'.
  * semi: vettori RESET/NMI/IRQ, ingressi (bit1) e opcode (bit0) della copertura, tabelle di puntatori
    note dal file analysis/seeds.txt del gioco (una riga "unit addr" esadecimale).
  * routine con dati inline (iniziano con PLA/STA...): dopo la JSR i byte sono dati (stringa fino a 00).
  * il resto e' emesso come .byte.
"""
import os, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from py65.devices.mpu6502 import MPU

UNIT = 8192
LEN = {"imp": 1, "acc": 1, "imm": 2, "zpg": 2, "zpx": 2, "zpy": 2, "rel": 2, "inx": 2, "iny": 2, "abs": 3, "abx": 3, "aby": 3, "ind": 3}
OPS = MPU().disassemble
HW = {0x2000: "PPUCTRL", 0x2001: "PPUMASK", 0x2002: "PPUSTATUS", 0x2003: "OAMADDR", 0x2004: "OAMDATA", 0x2005: "PPUSCROLL",
      0x2006: "PPUADDR", 0x2007: "PPUDATA", 0x4014: "OAMDMA", 0x4015: "APUSTATUS", 0x4016: "JOY1", 0x4017: "JOY2",
      0x5100: "MMC5_PRGMODE", 0x5101: "MMC5_CHRMODE", 0x5102: "MMC5_RAMPROT1", 0x5103: "MMC5_RAMPROT2", 0x5104: "MMC5_EXRAMMODE",
      0x5105: "MMC5_NTMAP", 0x5106: "MMC5_FILLTILE", 0x5107: "MMC5_FILLATTR", 0x5113: "MMC5_WRAMBANK", 0x5114: "MMC5_PRG8000",
      0x5115: "MMC5_PRGA000", 0x5116: "MMC5_PRGC000", 0x5117: "MMC5_PRGE000", 0x5130: "MMC5_CHRUPPER", 0x5200: "MMC5_SPLITMODE",
      0x5201: "MMC5_SPLITSCROLL", 0x5202: "MMC5_SPLITBANK", 0x5203: "MMC5_IRQCMP", 0x5204: "MMC5_IRQSTATUS",
      0x5205: "MMC5_MULA", 0x5206: "MMC5_MULB", 0x5015: "MMC5_AUDIOSTATUS", 0x5010: "MMC5_PCM"}
for i in range(8):
    HW[0x5120 + i] = "MMC5_CHR_A%d" % i
for i in range(4):
    HW[0x5128 + i] = "MMC5_CHR_B%d" % i


class Rom:
    def __init__(self, path, covs):
        d = open(path, "rb").read()
        self.prg = d[16:16 + d[4] * 16384]
        self.nunits = len(self.prg) // UNIT
        n = len(self.prg)
        self.op = bytearray(n)       # bit0 opcode visto (copertura)
        self.ent = bytearray(n)      # bit0 entry visto
        self.win = [0] * 256         # maschera finestre
        for c in covs:
            b = open(c, "rb").read()
            if len(b) == n:
                for i, v in enumerate(b):
                    if v & 1: self.op[i] = 1
                    if v & 2: self.ent[i] = 1
            w = c + ".win"
            if os.path.exists(w):
                for i, v in enumerate(open(w, "rb").read()):
                    self.win[i] |= v
        self.last = self.nunits - 1                     # unita' fissa a $E000 ($5117)
        # unita' viste eseguire a $C000 (commutabili con $5116); senza copertura: la penultima
        c0 = {u for u in range(self.nunits) if self.win[u] & 4}
        self.c000set = c0 or {self.nunits - 2}
        self.special = set(self.c000set) | {self.last}
        self.base = {}
        for u in range(self.nunits):
            self.base[u] = self.guess_base(u)
        self.c000 = max(self.c000set, key=lambda u: sum(self.op[u * UNIT:(u + 1) * UNIT]))

    def guess_base(self, u):
        if u == self.last: return 0xE000
        if u in self.c000set: return 0xC000
        m = self.win[u] & 7
        if m:
            for w in range(3):
                if m & (1 << w):
                    return 0x8000 + 0x2000 * w
        # stima: conta JSR/JMP abs i cui operandi cadono nella finestra candidata
        d = self.prg[u * UNIT:(u + 1) * UNIT]
        best, bw = -1, 0x8000
        for cand in (0x8000, 0xA000):
            n = 0
            for i in range(len(d) - 2):
                if d[i] in (0x20, 0x4C):
                    t = d[i + 1] | (d[i + 2] << 8)
                    if cand <= t < cand + UNIT:
                        n += 1
            if n > best:
                best, bw = n, cand
        return bw

    def unit_of(self, addr, cur):
        """unita' che contiene un indirizzo CPU, dato il contesto (unita' corrente)."""
        if addr >= 0xE000: return self.last, addr - 0xE000
        if 0xC000 <= addr < 0xE000 and cur not in self.c000set and self.base[cur] != 0xC000:
            return self.c000, addr - 0xC000
        if self.base[cur] <= addr < self.base[cur] + UNIT:
            return cur, addr - self.base[cur]
        return None, None


def load_seeds(rom, game_dir=None):
    """Semi noti del gioco: analysis/seeds.txt (o seeds.txt) nella cartella del gioco, righe 'unita' indirizzo' in esadecimale."""
    game_dir = game_dir or os.getcwd()
    out = []
    for p in (os.path.join(game_dir, "analysis", "seeds.txt"), os.path.join(game_dir, "seeds.txt"),
              os.path.join(game_dir, "tools", "seeds.txt")):
        if os.path.exists(p):
            for l in open(p):
                l = l.split("#")[0].split()
                if len(l) == 2:
                    u, a = int(l[0], 16), int(l[1], 16)
                    tu, to = rom.unit_of(a, u)
                    if tu is not None:
                        out.append((tu, to))
            break
    return out


def inline_routines(rom):
    """Routine che estraggono l'indirizzo di ritorno (PLA...STA nei primi passi): (unit,addr) -> True."""
    out = set()
    for u in range(rom.nunits):
        d = rom.prg[u * UNIT:(u + 1) * UNIT]
        for i in range(len(d) - 8):
            if d[i] == 0x68 and d[i + 1] in (0x85, 0x8D) or (d[i] == 0x68 and d[i + 1] == 0x68):
                out.add((u, i))
    return out


def learn_inline(rom, maxk=48):
    """Routine con argomenti inline dopo la JSR, dedotte dalla copertura.

    Per ogni JSR eseguita (bit opcode) il cui byte successivo (site+3) NON e' mai stato eseguito, il ritorno e'
    avvenuto piu' avanti: il primo opcode con bit 'ingresso' entro maxk byte e' il punto di ripresa.
    Restituisce {(unita', off routine): ('n', N) | ('str',)}: N byte fissi, oppure stringa fino a 00.
    """
    votes = {}
    for u in range(rom.nunits):
        base = u * UNIT
        for off in range(UNIT - 3):
            i = base + off
            if not rom.op[i] or rom.prg[i] != 0x20:
                continue
            a = rom.prg[i + 1] | (rom.prg[i + 2] << 8)
            tu, to = rom.unit_of(a, u)
            if tu is None:
                continue
            s = off + 3
            k = 0
            if not rom.op[base + s]:
                for j in range(1, maxk):
                    if s + j < UNIT and rom.op[base + s + j] and rom.ent[base + s + j]:
                        k = j
                        break
                if not k:
                    continue
            zero = rom.prg[base + s:base + s + maxk + 1].find(b"\x00")
            votes.setdefault((tu, to), []).append((k, zero + 1 if zero >= 0 else -1))
    out = {}
    for key, v in votes.items():
        ks = [k for k, _ in v]
        nz = [k for k in ks if k]
        if not nz or len(nz) < 0.8 * len(ks):
            continue                       # nessun salto, o conflitto (salto condizionale)
        best = max(set(nz), key=nz.count)
        if nz.count(best) >= 0.7 * len(nz):
            out[key] = ("n", best)         # voti isolati diversi = rumore (ingresso vicino non correlato)
        elif all(k == t for k, t in v if k):
            out[key] = ("str",)
    return out


LONG_RUN = 24


_pull_cache = {}


def pulls_return(rom, u, off):
    """La routine legge/estrae l'indirizzo di ritorno dallo stack (PLA senza PHA prima, oppure TSX + $01xx,X)."""
    key = (id(rom), u, off)
    if key in _pull_cache:
        return _pull_cache[key]
    d = rom.prg[u * UNIT:(u + 1) * UNIT]
    pushed = False
    r = False
    o, n = off, 0
    while o < UNIT and n < 30:
        name, mode = OPS[d[o]]
        if name == "???" or o + LEN[mode] > UNIT:
            break
        if name == "PHA":
            pushed = True
        if name == "PLA" and not pushed:
            r = True
            break
        if name == "TSX":
            r = True
            break
        if name in ("RTS", "RTI") or name == "JMP":
            break
        o += LEN[mode]
        n += 1
    _pull_cache[key] = r
    return r


def site_skip_emu(rom, u, site_off, tu, to):
    """Byte inline al sito, per emulazione con due insiemi di registri (devono concordare); con cache."""
    c = getattr(rom, "_emu", None)
    if c is None:
        c = rom._emu = {}
    k = (u, site_off)
    if k not in c:
        c[k] = None
        if pulls_return(rom, tu, to):
            a = emu_inline_skip(rom, u, site_off, tu, to, regs=(0, 0, 0))
            b = emu_inline_skip(rom, u, site_off, tu, to, regs=(0x35, 0x11, 0x22)) if a is not None else None
            c[k] = a if a is not None and a == b else None
    return c[k]


def emu_inline_skip(rom, u, site_off, tu, to, max_steps=60000, regs=(0, 0, 0)):
    """Esegue la routine chiamata (py65) sui byte reali del sito: dove RTS ritorna dice quanti byte inline salta.
    Ritorna il numero di byte inline (>=0) o None se l'emulazione non e' affidabile."""
    try:
        from py65.devices.mpu6502 import MPU
    except Exception:
        return None
    mem = bytearray(65536)

    def load(unit, base):
        mem[base:base + UNIT] = rom.prg[unit * UNIT:(unit + 1) * UNIT]

    for unit, base in ((tu, rom.base[tu]), (u, rom.base[u])):
        load(unit, base)
    if u not in rom.special and tu not in rom.special:
        load(rom.c000, 0xC000)
    load(rom.last, 0xE000)
    mpu = MPU(memory=mem)
    site = rom.base[u] + site_off
    ret = (site + 2) & 0xFFFF
    mem[0x1FF] = ret >> 8
    mem[0x1FE] = ret & 0xFF
    mpu.sp = 0xFD
    mpu.pc = rom.base[tu] + to
    mpu.a, mpu.x, mpu.y = regs
    for _ in range(max_steps):
        pc = mpu.pc
        op = mem[pc]
        if op == 0x00 or OPS[op][0] == "???" or pc < 0x8000:
            return None
        if op == 0x60 and mpu.sp == 0xFD:        # RTS che ripristina il ritorno originale (dopo i 2 byte estratti)
            pass
        was = mpu.sp
        mpu.step()
        if (op == 0x60 or op == 0x6C) and mpu.sp == 0xFF:   # RTS / JMP (ind) con lo stack "pulito": ripresa
            skip = mpu.pc - (site + 3)
            return skip if 0 <= skip <= 250 else None
        if op == 0x60 and mpu.sp > 0xFF:
            return None
        if mpu.sp > 0xFF or (mpu.pc < 0x8000):
            return None
    return None


_table_cache = {}


def is_table_routine(rom, u, off):
    """Routine 'JSR tab; .word a,b,c...': estrae l'indirizzo di ritorno con PLA (senza PHA prima) e salta con JMP (ind)."""
    key = (id(rom), u, off)
    if key in _table_cache:
        return _table_cache[key]
    d = rom.prg[u * UNIT:(u + 1) * UNIT]
    pulled = pushed_first = False
    has_ind = False
    n = 0
    o = off
    while o < UNIT and n < 40:
        name, mode = OPS[d[o]]
        if name == "???" or o + LEN[mode] > UNIT:
            break
        if name == "PHA" and not pulled:
            pushed_first = True
        if name == "PLA" and not pushed_first:
            pulled = True
        if name == "JMP" and mode == "ind":
            has_ind = True
        if name in ("RTS", "RTI") or (name == "JMP" and mode == "abs"):
            break
        if name == "JMP":
            break
        o += LEN[mode]
        n += 1
    r = pulled and has_ind
    _table_cache[key] = r
    return r


def _inline_skip(rom, u, d, off, name, a):
    """Offset dopo i byte inline se JSR a una routine con argomenti inline nota (dalla copertura), altrimenti None."""
    if name != "JSR":
        return None
    lr = getattr(rom, "learned", None) or {}
    tu, to = rom.unit_of(a, u)
    v = lr.get((tu, to)) if tu is not None else None
    if not v and tu is not None:
        n = site_skip_emu(rom, u, off, tu, to)
        if n is not None:
            return off + 3 + n
    if not v:
        return None
    if v[0] == "n":
        return off + 3 + v[1]
    p = off + 3
    while p < UNIT and d[p] != 0:
        p += 1
    return p + 1


def plausible_code(rom, u, off, min_insns=3, max_insns=80):
    """True se da off si decodifica una sequenza sensata che termina con RTS/RTI/JMP entro max_insns."""
    d = rom.prg[u * UNIT:(u + 1) * UNIT]
    n = 0
    while off < UNIT and n < max_insns:
        name, mode = OPS[d[off]]
        if name == "???" or name == "BRK":
            return False
        ln = LEN[mode]
        if off + ln > UNIT:
            return False
        if mode == "rel":
            t = off + 2 + (d[off + 1] - 256 if d[off + 1] > 127 else d[off + 1])
            if not 0 <= t < UNIT:
                return False
        nxt = off + ln
        if mode in ("abs", "abx", "aby", "ind") and name in ("JSR", "JMP"):
            a = d[off + 1] | (d[off + 2] << 8)
            if a < 0x8000:
                return False
            sk = _inline_skip(rom, u, d, off, name, a)
            if sk is not None:
                nxt = sk
        n += 1
        if name in ("RTS", "RTI") or (name == "JMP"):
            return n >= min_insns
        off = nxt
    return n >= LONG_RUN         # nessun terminatore, ma una lunga sequenza valida non e' quasi mai dato


def _win(t):
    return 0x8000 + 0x2000 * ((t - 0x8000) >> 13) if t >= 0x8000 else None


def pointer_table_seeds(rom, code):
    """Tabelle di parole a 16 bit consecutive (>=4) che puntano alla stessa finestra CPU (o +1: dispatch RTS).
    La finestra puo' essere quella del banco che contiene la tabella oppure un'altra ($8000/$A000): in tal caso
    l'unita' e' una qualsiasi unita' di codice con quella base per cui la decodifica e' plausibile."""
    seeds = set()
    code_units = [v for v in range(rom.nunits) if sum(code[v].values()) >= 1200 and v not in rom.special]

    def cands(t, u):
        w = _win(t)
        if rom.base[u] <= t < rom.base[u] + UNIT:
            return [(u, t - rom.base[u])] if plausible_code(rom, u, t - rom.base[u]) else []
        if t >= 0xE000:
            return [(rom.last, t - 0xE000)] if plausible_code(rom, rom.last, t - 0xE000) else []
        if t >= 0xC000:
            return [(rom.c000, t - 0xC000)] if plausible_code(rom, rom.c000, t - 0xC000) else []
        return [(v, t - rom.base[v]) for v in code_units
                if v != u and rom.base[v] == w and strict_plausible(rom, v, t - rom.base[v])]

    for u in range(rom.nunits):
        d = rom.prg[u * UNIT:(u + 1) * UNIT]
        i = 0
        while i < UNIT - 8:
            run = []
            j = i
            w0 = None
            while j + 1 < UNIT:
                w = d[j] | (d[j + 1] << 8)
                ok = None
                for delta in (0, 1):
                    t = w + delta
                    if 0x8000 <= t < 0x10000 and (w0 is None or _win(t) == w0):
                        ok = t
                        break
                if ok is None:
                    break
                if w0 is None:
                    w0 = _win(ok)
                run.append(ok)
                j += 2
            if len(run) >= 4:
                for t in run:
                    for c in cands(t, u):
                        seeds.add(c)
                i = j
            else:
                i += 2
    return seeds


def chain_seeds(rom, min_chain=10, min_share=0.25):
    """Semi euristici: catene di >= min_chain istruzioni valide che finiscono con RTS/RTI (solo nei banchi 'da codice')."""
    seeds = []
    for u in range(rom.nunits):
        d = rom.prg[u * UNIT:(u + 1) * UNIT]
        chains, i, good = [], 0, 0
        while i < UNIT:
            j, c = i, 0
            while j < UNIT:
                name, mode = OPS[d[j]]
                if name in ("???", "BRK"):
                    break
                ln = LEN[mode]
                if j + ln > UNIT:
                    break
                if mode == "rel":
                    t = j + 2 + (d[j + 1] - 256 if d[j + 1] > 127 else d[j + 1])
                    if not 0 <= t < UNIT:
                        break
                j += ln; c += 1
                if name in ("RTS", "RTI"):
                    break
            if c >= min_chain:
                chains.append(i); good += j - i; i = j
            else:
                i += 1
        if good / UNIT >= min_share:
            seeds += [(u, s) for s in chains]
    return seeds


def mem_ok(a, name):
    """Indirizzo assoluto plausibile per un'istruzione (RAM, PPU/APU, registri MMC5, WRAM, ROM in lettura)."""
    if a < 0x0800 or 0x2000 <= a <= 0x2007 or a in (0x4014, 0x4015, 0x4016, 0x4017) or 0x4000 <= a <= 0x4013:
        return True
    if 0x5000 <= a <= 0x5015 or 0x5100 <= a <= 0x5107 or 0x5113 <= a <= 0x5117 or 0x5120 <= a <= 0x512B or \
            0x5130 == a or 0x5200 <= a <= 0x5206 or 0x5C00 <= a <= 0x5FFF:
        return True
    if 0x6000 <= a < 0x8000:
        return True
    if a >= 0x8000:
        return name not in ("STA", "STX", "STY", "INC", "DEC", "ASL", "LSR", "ROL", "ROR")   # ROM: niente scritture
    return False


def strict_plausible(rom, u, off, min_insns=4, max_insns=120):
    d = rom.prg[u * UNIT:(u + 1) * UNIT]
    n = 0
    while off < UNIT and n < max_insns:
        name, mode = OPS[d[off]]
        if name == "???" or name == "BRK":
            return False
        ln = LEN[mode]
        if off + ln > UNIT:
            return False
        if mode == "rel":
            t = off + 2 + (d[off + 1] - 256 if d[off + 1] > 127 else d[off + 1])
            if not 0 <= t < UNIT:
                return False
        nxt = off + ln
        if mode in ("abs", "abx", "aby", "ind"):
            a = d[off + 1] | (d[off + 2] << 8)
            if name in ("JSR", "JMP"):
                if a < 0x8000:
                    return False
                sk = _inline_skip(rom, u, d, off, name, a)
                if sk is not None:
                    nxt = sk
            elif not mem_ok(a, name):
                return False
        n += 1
        if name in ("RTS", "RTI") or name == "JMP":
            return n >= min_insns
        off = nxt
    return n >= LONG_RUN


def xwin_seeds(rom, code, ext):
    """Chiamate verso un'altra finestra ($8000/$A000): l'unita' e' quella mappata a run time.
    Candidate = unita' di codice con quella base per cui la decodifica a quell'offset e' rigorosamente plausibile."""
    code_units = [v for v in range(rom.nunits) if sum(code[v].values()) >= 1200 and v not in rom.special]
    out = set()
    for u, addrs in ext.items():
        for a in addrs:
            if not 0x8000 <= a < 0xC000:
                continue
            for v in code_units:
                if v == u or not rom.base[v] <= a < rom.base[v] + UNIT:
                    continue
                if strict_plausible(rom, v, a - rom.base[v]):
                    out.add((v, a - rom.base[v]))
    return out


def analyze(rom, seeds_extra):
    """Analisi a livelli di affidabilita': ogni livello e' una chiusura completa del flusso e non puo' spezzare
    (sovrapporsi a) le istruzioni dei livelli precedenti.
      0 opcode eseguiti, vettori, semi noti;  1 chiamate far/xwin;  2 tabelle di puntatori e funzioni dopo un terminatore;
      3 catene euristiche (spesso dati)."""
    chain = chain_seeds(rom)
    seeds_extra = list(seeds_extra)
    t1, t2 = set(), set()
    code, targets, ext = analyze_pass(rom, [seeds_extra, [], [], chain])
    for _ in range(8):
        if getattr(rom, "xwin", True):
            t1 |= xwin_seeds(rom, code, ext)
        for s in pointer_table_seeds(rom, code):
            t2.add(s)
        for u in range(rom.nunits):        # funzioni subito dopo un terminatore
            cs = code[u]
            d = rom.prg[u * UNIT:(u + 1) * UNIT]
            for off, ln in list(cs.items()):
                if OPS[d[off]][0] in ("RTS", "RTI", "JMP"):
                    nxt = off + ln
                    if nxt < UNIT and nxt not in cs and plausible_code(rom, u, nxt):
                        t2.add((u, nxt))
        new = [s for s in (t1 | t2) if s[1] not in code[s[0]]]
        if not new:
            break
        code, targets, ext = analyze_pass(rom, [seeds_extra, sorted(t1), sorted(t2), chain])
    return code, targets, ext


def analyze_pass(rom, tiers):
    code = {u: {} for u in range(rom.nunits)}      # unit -> {off: length}
    targets = {u: set() for u in range(rom.nunits)}
    ext = collections.defaultdict(set)              # riferimenti fuori unita': (unit, off)
    occ = {u: bytearray(UNIT) for u in range(rom.nunits)}   # byte occupati da istruzioni decodificate
    inline = inline_routines(rom)
    learned = getattr(rom, "learned", None)
    if learned is None:
        learned = rom.learned = learn_inline(rom)

    def place(u, off, ln):
        code[u][off] = ln
        for x in range(off, off + ln):
            occ[u][x] = 1

    # opcode realmente eseguiti: istruzioni certe, con i loro byte riservati prima di qualsiasi euristica
    for i, v in enumerate(rom.op):
        if v:
            u, off = divmod(i, UNIT)
            name, mode = OPS[rom.prg[i]]
            if name == "???":
                continue
            ln = LEN[mode]
            if off + ln <= UNIT and not any(occ[u][off:off + ln]):
                place(u, off, ln)

    def run(work):
        seen = set()
        while work:
            u, off = work.pop()
            if u is None or not (0 <= off < UNIT) or (u, off) in seen:
                continue
            seen.add((u, off))
            # decodifica lineare da off finche' non c'e' un terminatore
            while 0 <= off < UNIT:
                d = rom.prg[u * UNIT + off]
                name, mode = OPS[d]
                if name == "???" or (name == "BRK" and not rom.op[u * UNIT + off]):
                    break
                ln = LEN[mode]
                if off + ln > UNIT:
                    break
                if off not in code[u]:
                    if any(occ[u][off:off + ln]):
                        break                       # cadrebbe nel mezzo di un'istruzione gia' decodificata
                    place(u, off, ln)
                ops = rom.prg[u * UNIT + off + 1:u * UNIT + off + ln]
                base = rom.base[u]
                if mode == "rel":
                    t = off + 2 + (ops[0] - 256 if ops[0] > 127 else ops[0])
                    if 0 <= t < UNIT:
                        targets[u].add(t); work.append((u, t))
                    else:
                        tu, to = rom.unit_of((base + t) & 0xFFFF, u)
                        if tu is not None:
                            targets[tu].add(to); work.append((tu, to))
                elif name in ("JSR", "JMP") and mode == "abs":
                    a = ops[0] | (ops[1] << 8)
                    tu, to = rom.unit_of(a, u)
                    if tu is not None:
                        targets[tu].add(to); work.append((tu, to))
                    else:
                        ext[u].add(a)
                    en = site_skip_emu(rom, u, off, tu, to) if (name == "JSR" and tu is not None) else None
                    if name == "JSR" and tu is not None and en is None and (tu, to) not in learned and is_table_routine(rom, tu, to):
                        # tabella di puntatori inline: ogni voce (indirizzo o indirizzo-1) e' un ingresso; la JSR non ritorna
                        p = off + ln
                        dd = rom.prg[u * UNIT:(u + 1) * UNIT]
                        while p + 1 < UNIT and p not in code[u]:
                            w = dd[p] | (dd[p + 1] << 8)
                            hit = None
                            for delta in (0, 1):
                                t2u, t2o = rom.unit_of((w + delta) & 0xFFFF, u)
                                if t2u is not None and plausible_code(rom, t2u, t2o):
                                    hit = (t2u, t2o); break
                            if not hit:
                                break
                            targets[hit[0]].add(hit[1]); work.append(hit)
                            p += 2
                        break
                    lr = learned.get((tu, to)) if name == "JSR" and tu is not None else None
                    if en is not None and en > 0:
                        lr = ("n", en)
                    if lr and lr[0] == "n":
                        # chiamata "far": argomenti inline = (banco|$80, lo, hi) -> ingresso in un'altra unita'
                        for k in range(0, lr[1] - 2):
                            b0 = rom.prg[u * UNIT + off + ln + k]
                            a2 = rom.prg[u * UNIT + off + ln + k + 1] | (rom.prg[u * UNIT + off + ln + k + 2] << 8)
                            v = b0 & 0x3F
                            if b0 & 0x80 and v < rom.nunits and rom.base[v] <= a2 < rom.base[v] + UNIT:
                                targets[v].add(a2 - rom.base[v]); work.append((v, a2 - rom.base[v]))
                        off = off + ln + lr[1]
                        if off < UNIT:
                            targets[u].add(off)
                        continue
                    if name == "JSR" and tu is not None and (lr or (tu, to) in inline):
                        # dati inline: stringa fino a 00; il codice riprende dopo il terminatore
                        p = off + ln
                        while p < UNIT and rom.prg[u * UNIT + p] != 0:
                            p += 1
                        off = p + 1
                        if off < UNIT:
                            targets[u].add(off)
                        continue
                    if name == "JMP":
                        break
                elif name == "JMP" or name in ("RTS", "RTI"):
                    break
                off += ln

    # livello 0: vettori, ingressi e opcode visti, semi noti
    w0 = []
    fixed = rom.prg[rom.last * UNIT:]
    for vec in (0x1FFA, 0x1FFC, 0x1FFE):
        a = fixed[vec] | (fixed[vec + 1] << 8)
        w0.append((rom.last, a - 0xE000))
    for i, v in enumerate(rom.op):
        if v and rom.ent[i]:
            w0.append((i // UNIT, i % UNIT))
    for i, v in enumerate(rom.op):
        if v:
            w0.append((i // UNIT, i % UNIT))
    w0 += tiers[0]
    run(w0)
    for t in tiers[1:]:
        run(list(t))
    return code, targets, ext


def operand_text(rom, u, off, name, mode, ops, targets, labels):
    base = rom.base[u]
    def sym(a):
        if a in HW: return HW[a]
        tu, to = rom.unit_of(a, u)
        if tu is not None and a >= 0x8000:
            n = labels(tu, to)
            if n:
                return n
        return "$%04X" % a if a > 0xFF else "$%02X" % a
    if mode == "imp": return ""
    if mode == "acc": return "a"
    if mode == "imm": return "#$%02X" % ops[0]
    if mode == "rel":
        t = off + 2 + (ops[0] - 256 if ops[0] > 127 else ops[0])
        if not 0 <= t < UNIT:                       # salto oltre il bordo dell'unita' (es. da $DFxx a $E0xx)
            return sym((base + t) & 0xFFFF)
        return labels(u, t)
    a = ops[0] if len(ops) == 1 else ops[0] | (ops[1] << 8)
    if mode in ("abs", "abx", "aby", "ind"):
        s = sym(a)
        if a <= 0xFF and mode != "ind":
            s = "a:" + s              # forma assoluta di un indirizzo di pagina zero (ca65: a:$12)
    else:
        s = "$%02X" % a
    return {"abs": s, "abx": s + ",x", "aby": s + ",y", "ind": "(%s)" % s, "zpg": s, "zpx": s + ",x", "zpy": s + ",y",
            "inx": "(%s,x)" % s, "iny": "(%s),y" % s}[mode]


def emit(rom, code, targets, outdir):
    os.makedirs(outdir, exist_ok=True)
    summary = []
    foreign = {u: set() for u in range(rom.nunits)}      # nomi di etichette di altre unita' usate da u
    bodies = {}
    for u in range(rom.nunits):
        base = rom.base[u]
        def lab(tu, to, u=u):
            if to not in targets[tu]:
                return None                       # riferimento a dati: indirizzo numerico
            n = "u%02d_%04X" % (tu, rom.base[tu] + to)
            if tu != u:
                foreign[u].add(n)
            return n
        lines = []
        d = rom.prg[u * UNIT:(u + 1) * UNIT]
        off, n_code = 0, 0
        cs = code[u]
        while off < UNIT:
            if off in targets[u] or off in cs and off in targets[u]:
                lines.append("%s:" % lab(u, off))
            if off in cs:
                ln = cs[off]
                name, mode = OPS[d[off]]
                ops = d[off + 1:off + ln]
                op = operand_text(rom, u, off, name, mode, ops, targets[u], lab)
                lines.append("    %-4s %-22s ; %04X  %s" % (name.lower(), op, base + off, " ".join("%02X" % b for b in d[off:off + ln])))
                for k in range(1, ln):                      # bersaglio dentro un'istruzione: alias
                    if off + k in targets[u]:
                        lines.append("%s = * - %d" % (lab(u, off + k), ln - k))
                n_code += ln
                off += ln
                continue
            # dati: fino al prossimo byte di codice o etichetta, a righe da 16
            end = off
            while end < UNIT and end not in cs and (end == off or end not in targets[u]):
                end += 1
            p = off
            while p < end:
                q = min(end, p + 16)
                chunk = d[p:q]
                lines.append("    .byte %-64s ; %04X" % (",".join("$%02X" % b for b in chunk), base + p))
                p = q
            off = end
        bodies[u] = lines
        summary.append((u, n_code))
    used_elsewhere = set()
    for u in range(rom.nunits):
        used_elsewhere |= foreign[u]
    for u in range(rom.nunits):
        own = {("u%02d_%04X" % (u, rom.base[u] + o)) for o in targets[u]} & used_elsewhere
        glob = sorted(own | foreign[u])
        head = ["; unita' %d  (ROM 0x%05X-0x%05X)  finestra CPU $%04X" % (u, u * UNIT, u * UNIT + UNIT - 1, rom.base[u]),
                '.include "hw.inc"', '.segment "U%02d"' % u]
        head += [".global " + ", ".join(glob[i:i + 6]) for i in range(0, len(glob), 6)]
        open(os.path.join(outdir, "unit%02d.asm" % u), "w", encoding="utf-8").write("\n".join(head + [""] + bodies[u]) + "\n")
    with open(os.path.join(outdir, "hw.inc"), "w") as f:
        for a, n in sorted(HW.items()):
            f.write("%s = $%04X\n" % (n, a))
    with open(os.path.join(outdir, "layout.cfg"), "w") as f:
        f.write("# ld65: ROM PRG = unita' da 8K (le finestre MMC5 sono solo etichette: ogni unita' e' assemblata al proprio indirizzo)\n")
        f.write("MEMORY {\n")
        for u in range(rom.nunits):
            f.write("    M%02d: start = $%04X, size = $2000, file = \"unit%02d.bin\", fill = yes;\n" % (u, rom.base[u], u))
        f.write("}\nSEGMENTS {\n")
        for u in range(rom.nunits):
            f.write("    U%02d: load = M%02d, type = ro;\n" % (u, u))
        f.write("}\n")
    with open(os.path.join(outdir, "summary.txt"), "w") as f:
        tot = 0
        for u, n in summary:
            f.write("unit %2d  base $%04X  codice %5d / %d (%.0f%%)\n" % (u, rom.base[u], n, UNIT, 100 * n / UNIT))
            tot += n
        f.write("TOTALE codice: %d / %d (%.1f%%)\n" % (tot, rom.nunits * UNIT, 100 * tot / (rom.nunits * UNIT)))
    return summary


if __name__ == "__main__":
    rom = Rom(sys.argv[1], sys.argv[3:])
    code, targets, ext = analyze(rom, load_seeds(rom))
    s = emit(rom, code, targets, sys.argv[2])
    tot = sum(n for _, n in s)
    print("codice disassemblato: %d byte su %d (%.1f%%)" % (tot, rom.nunits * UNIT, 100 * tot / (rom.nunits * UNIT)))
