#!/usr/bin/env python3
"""Classifica ogni byte della PRG: codice / dati inline / tabelle / padding / ignoto.

  python tools/classify.py baserom_jp.nes analysis/all.bin [OUT.txt]

  codice   : istruzioni decodificate da tools/disasm.py
  inline   : byte dopo una JSR a routine con argomenti inline (o stringa) nel codice decodificato
  tabella  : parole consecutive (>=4) che puntano a finestre ROM (puntatori/dispatch) o coppie lo/hi
  padding  : run >= 8 di $FF o $00
  ignoto   : tutto il resto (dati non riconosciuti o codice non raggiunto)
Scrive un riepilogo per unita' e, per le unita' "di codice", l'elenco dei tratti ignoti piu' grandi.
"""
import os, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import disasm

UNIT = disasm.UNIT


def main():
    rom_path, cov = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else None
    rom = disasm.Rom(rom_path, [cov])
    extra = disasm.load_seeds(rom)
    code, targets, ext = disasm.analyze(rom, extra)
    lines = []
    tot = {"codice": 0, "inline": 0, "tabella": 0, "padding": 0, "ignoto": 0}
    lines.append("unita  base   codice inline tabella padding ignoto  tipo")
    unknown_runs = []
    for u in range(rom.nunits):
        d = rom.prg[u * UNIT:(u + 1) * UNIT]
        cls = bytearray(UNIT)      # 0 ignoto, 1 codice, 2 inline, 3 tabella, 4 padding
        for off, ln in code[u].items():
            for x in range(off, off + ln):
                cls[x] = 1
        # inline dopo JSR a routine con argomenti
        for off, ln in code[u].items():
            if d[off] == 0x20 and off + ln <= UNIT:
                a = d[off + 1] | (d[off + 2] << 8)
                sk = disasm._inline_skip(rom, u, d, off, "JSR", a)
                lr = getattr(rom, "learned", {}).get(rom.unit_of(a, u))
                if sk is not None or lr:
                    end = sk if sk is not None else off + 3
                    for x in range(off + 3, min(end, UNIT)):
                        if cls[x] == 0:
                            cls[x] = 2
                    if lr and lr[0] == "str" and sk is None:
                        p = off + 3
                        while p < UNIT and d[p] != 0:
                            p += 1
                        for x in range(off + 3, min(p + 1, UNIT)):
                            if cls[x] == 0:
                                cls[x] = 2
        # padding
        i = 0
        while i < UNIT:
            if cls[i] == 0 and d[i] in (0x00, 0xFF):
                j = i
                while j < UNIT and cls[j] == 0 and d[j] == d[i]:
                    j += 1
                if j - i >= 8:
                    for x in range(i, j):
                        cls[x] = 4
                i = max(j, i + 1)
            else:
                i += 1
        # tabelle di parole
        i = 0
        while i < UNIT - 8:
            j = i
            n = 0
            while j + 1 < UNIT and cls[j] == 0 and cls[j + 1] == 0:
                w = d[j] | (d[j + 1] << 8)
                if 0x8000 <= w + 1 < 0x10000:
                    n += 1
                    j += 2
                else:
                    break
            if n >= 4:
                for x in range(i, j):
                    cls[x] = 3
                i = j
            else:
                i += 2
        cnt = [0] * 5
        for c in cls:
            cnt[c] += 1
        for k, name in ((1, "codice"), (2, "inline"), (3, "tabella"), (4, "padding"), (0, "ignoto")):
            tot[name] += cnt[k]
        kind = "CODICE" if cnt[1] >= 1200 or any(rom.op[u * UNIT:(u + 1) * UNIT]) else "dati"
        lines.append("%4d  $%04X %6d %6d %7d %7d %6d  %s" % (u, rom.base[u], cnt[1], cnt[2], cnt[3], cnt[4], cnt[0], kind))
        if kind == "CODICE":
            i = 0
            while i < UNIT:
                if cls[i] == 0:
                    j = i
                    while j < UNIT and cls[j] == 0:
                        j += 1
                    if j - i >= 16:
                        unknown_runs.append((j - i, u, rom.base[u] + i))
                    i = j
                else:
                    i += 1
    lines.append("")
    lines.append("TOTALE: " + "  ".join("%s %d (%.1f%%)" % (k, v, 100.0 * v / (rom.nunits * UNIT)) for k, v in tot.items()))
    lines.append("")
    lines.append("tratti ignoti >= 16 byte nelle unita' di codice (i piu' grandi):")
    for n, u, a in sorted(unknown_runs, reverse=True)[:60]:
        lines.append("  unita' %2d  $%04X  %5d byte" % (u, a, n))
    text = "\n".join(lines) + "\n"
    print(text)
    if out:
        open(out, "w", encoding="utf-8").write(text)


if __name__ == "__main__":
    main()
