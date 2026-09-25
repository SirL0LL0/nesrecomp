#!/usr/bin/env python3
"""Verifica che il sorgente prodotto da tools/disasm.py, riassemblato, dia esattamente la ROM (PRG).

  python tools/asm_verify.py baserom_jp.nes DISASM_DIR

Piccolo assemblatore 6502 per il sottoinsieme che disasm.py emette (ca65: etichette, `a:$12`, `.byte`,
`name = * - n`, `hw.inc`). Non serve ca65: se qui coincide byte per byte, il sorgente e' completo e fedele.
"""
import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import disasm

UNIT = disasm.UNIT
ENC = {}
for op in range(256):
    n, m = disasm.OPS[op]
    if n != "???":
        ENC.setdefault((n.lower(), m), op)
BRANCH = {"bcc", "bcs", "beq", "bne", "bmi", "bpl", "bvc", "bvs"}
LINE = re.compile(r"^\s*(?:(?P<label>[A-Za-z_]\w*):)?\s*(?P<body>[^;]*?)\s*(?:;.*)?$")


def parse_val(tok, syms):
    tok = tok.strip()
    if tok.startswith("$"):
        return int(tok[1:], 16)
    return syms[tok]


def classify(mn, opnd):
    """(mode, operand_expr) senza risolvere i simboli."""
    if mn in BRANCH:
        return "rel", opnd
    if opnd == "":
        return "imp", None
    if opnd == "a":
        return "acc", None
    if opnd.startswith("#"):
        return "imm", opnd[1:]
    m = re.fullmatch(r"\((.+),x\)", opnd)
    if m: return "inx", m.group(1)
    m = re.fullmatch(r"\((.+)\),y", opnd)
    if m: return "iny", m.group(1)
    m = re.fullmatch(r"\((.+)\)", opnd)
    if m: return "ind", m.group(1)
    idx = None
    if opnd.endswith(",x"): idx, opnd = "x", opnd[:-2]
    elif opnd.endswith(",y"): idx, opnd = "y", opnd[:-2]
    forced_abs = opnd.startswith("a:")
    if forced_abs: opnd = opnd[2:]
    zp = (not forced_abs) and re.fullmatch(r"\$[0-9A-Fa-f]{2}", opnd) is not None
    if zp:
        return {None: "zpg", "x": "zpx", "y": "zpy"}[idx], opnd
    return {None: "abs", "x": "abx", "y": "aby"}[idx], opnd


def size_of(mode):
    return {"imp": 1, "acc": 1, "imm": 2, "zpg": 2, "zpx": 2, "zpy": 2, "rel": 2, "inx": 2, "iny": 2,
            "abs": 3, "abx": 3, "aby": 3, "ind": 3}[mode]


def load(dirpath):
    syms = {}
    for l in open(os.path.join(dirpath, "hw.inc")):
        n, v = l.split("=")
        syms[n.strip()] = int(v.strip()[1:], 16)
    return syms


def assemble_unit(path, u, base, syms, second):
    items = []                      # (kind, payload)
    pc = base
    out = bytearray()
    for raw in open(path, encoding="utf-8"):
        m = LINE.match(raw.rstrip("\n"))
        if not m:
            raise ValueError("riga non valida: " + raw)
        lab, body = m.group("label"), m.group("body").strip()
        if lab:
            if second: pass
            else: syms[lab] = pc
        if not body or body.startswith((".include", ".segment", ".global")):
            continue
        if body.startswith(".byte"):
            vals = [int(t.strip()[1:], 16) for t in body[5:].split(",")]
            out += bytes(vals); pc += len(vals)
            continue
        am = re.fullmatch(r"([A-Za-z_]\w*)\s*=\s*\*\s*-\s*(\d+)", body)
        if am:
            if not second: syms[am.group(1)] = pc - int(am.group(2))
            continue
        parts = body.split(None, 1)
        mn = parts[0]
        opnd = parts[1].strip() if len(parts) > 1 else ""
        mode, expr = classify(mn, opnd)
        sz = size_of(mode)
        if not second:
            out += bytes(sz); pc += sz
            continue
        op = ENC.get((mn, mode))
        if op is None:
            raise ValueError("opcode non codificabile %s %s (%s)" % (mn, opnd, mode))
        if mode in ("imp", "acc"):
            b = [op]
        elif mode == "rel":
            rel = parse_val(expr, syms) - (pc + 2)
            if not -128 <= rel <= 127: raise ValueError("branch fuori portata a %04X" % pc)
            b = [op, rel & 0xFF]
        elif sz == 2:
            b = [op, parse_val(expr, syms) & 0xFF]
        else:
            v = parse_val(expr, syms)
            b = [op, v & 0xFF, (v >> 8) & 0xFF]
        out += bytes(b); pc += sz
    return bytes(out)


def main():
    rom_path, dirpath = sys.argv[1], sys.argv[2]
    d = open(rom_path, "rb").read()
    prg = d[16:16 + d[4] * 16384]
    n = len(prg) // UNIT
    syms = load(dirpath)
    bases = {}
    for u in range(n):
        first = open(os.path.join(dirpath, "unit%02d.asm" % u), encoding="utf-8").readline()
        bases[u] = int(re.search(r"finestra CPU \$([0-9A-F]{4})", first).group(1), 16)
    for u in range(n):                                    # passo 1: etichette
        assemble_unit(os.path.join(dirpath, "unit%02d.asm" % u), u, bases[u], syms, False)
    bad = 0
    for u in range(n):                                    # passo 2: byte
        try:
            b = assemble_unit(os.path.join(dirpath, "unit%02d.asm" % u), u, bases[u], syms, True)
        except Exception as e:
            print("unita' %d: %s" % (u, e)); bad += 1; continue
        ref = prg[u * UNIT:(u + 1) * UNIT]
        if b != ref:
            diff = [i for i in range(min(len(b), len(ref))) if b[i] != ref[i]]
            print("unita' %d: DIVERSA (%d byte diversi, lunghezza %d/%d, primo a %04X)" % (
                u, len(diff), len(b), len(ref), bases[u] + (diff[0] if diff else 0)))
            bad += 1
    print("RIASSEMBLAGGIO: %s (%d/%d unita' identiche)" % ("OK, ROM identica" if not bad else "DIFFERENZE", n - bad, n))
    return bad


if __name__ == "__main__":
    sys.exit(1 if main() else 0)
