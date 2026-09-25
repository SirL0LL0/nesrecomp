"""Nomi automatici per il decompilato: variabili RAM (registri ombra, puntatori, tabelle) e funzioni note.

Priorita': analysis/symbols.tsv (a mano) > euristiche qui sotto > nomi numerici (zp_BC, ram_0300, ...).
"""
import collections

KNOWN_FUNCS = {}      # i nomi noti stanno in analysis/symbols.tsv del gioco
KNOWN_RAM = {}


def auto_names(syms, fn_ok, rom):
    """Riempie syms.auto e syms.func con nomi dedotti dal codice."""
    for k, n in KNOWN_FUNCS.items():
        if k not in syms.func:
            syms.func[k] = n
    for a, n in KNOWN_RAM.items():
        if a not in syms.ram:
            syms.auto[a] = n
    hw = syms.hw
    cand = collections.defaultdict(collections.Counter)
    ptrs = set()
    pcount = collections.Counter()
    bases = collections.defaultdict(int)
    for e, body in fn_ok.items():
        order = sorted(body.values(), key=lambda i: i.off)
        for idx, i in enumerate(order):
            # registro ombra: STA zp seguito da STA <registro hardware> (o viceversa)
            if i.name == "STA" and i.mode == "zpg" and idx + 1 < len(order):
                j = order[idx + 1]
                if j.name == "STA" and j.mode == "abs":
                    a = j.ops[0] | (j.ops[1] << 8)
                    if a in hw and j.off == i.off + i.ln:
                        cand[i.ops[0]][hw[a].lower() + "_shadow"] += 1
            if i.mode in ("inx", "iny"):
                pcount[i.ops[0]] += 1
            if i.mode in ("abx", "aby") and (i.ops[0] | (i.ops[1] << 8)) >= 0x8000:
                bases[i.ops[0] | (i.ops[1] << 8)] += 1
    for a, cnt in cand.items():
        if a in syms.ram or a in syms.auto:
            continue
        name, _ = cnt.most_common(1)[0]
        if not any(v == name for v in list(syms.auto.values()) + list(syms.ram.values())):
            syms.auto[a] = name
    ptrs = {a for a, n in pcount.items() if n >= 3}
    for a in ptrs:
        if a not in syms.ram and a not in syms.auto:
            syms.auto[a] = "ptr_%02X" % a
            syms.auto[a + 1] = "ptr_%02X_hi" % a if (a + 1) not in syms.ram and (a + 1) not in syms.auto else syms.auto.get(a + 1)
    for a in bases:
        if a not in syms.ram and a not in syms.auto:
            syms.auto[a] = "tbl_%04X" % a
