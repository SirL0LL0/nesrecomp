#!/usr/bin/env python3
"""Quanto codice eseguito l'analisi statica NON trova ("miss" potenziali del recompiler)?

  python tools/miss_report.py baserom_jp.nes all.bin [--loo user5.bin=rest.bin]

  * S0  = codice trovato dall'analisi statica da soli vettori/tabelle (nessuna copertura);
  * E   = opcode realmente eseguiti (copertura unita');
  * miss statici = E - S0 (per unita', in byte e in ingressi di routine);
  * leave-one-out (--loo NUOVA=RESTO): S1 = statica + RESTO; miss di NUOVA = opcode eseguiti in NUOVA e non in S1:
    misura quanto la scoperta di nuove zone e' prevedibile senza giocarle.
"""
import os, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import disasm

UNIT = disasm.UNIT


def code_set(rom, seeds=()):
    code, _, _ = disasm.analyze(rom, list(seeds))
    s = set()
    for u, d in code.items():
        for off, ln in d.items():
            s.add(u * UNIT + off)
    return s


def executed(path):
    b = open(path, "rb").read()
    return {i for i, v in enumerate(b) if v & 1}, {i for i, v in enumerate(b) if v & 2}


def per_unit(missing):
    out = {}
    for i in missing:
        out[i // UNIT] = out.get(i // UNIT, 0) + 1
    return out


def main():
    rom_path, allbin = sys.argv[1], sys.argv[2]
    loo = None
    if "--loo" in sys.argv:
        new, rest = sys.argv[sys.argv.index("--loo") + 1].split("=")
        loo = (new, rest)
    E, entries = executed(allbin)
    rom0 = disasm.Rom(rom_path, [])
    S0 = code_set(rom0)
    miss = E - S0
    print("opcode eseguiti: %d | trovati dall'analisi statica pura: %d (%.1f%%) | miss statici: %d" % (
        len(E), len(E & S0), 100.0 * len(E & S0) / max(len(E), 1), len(miss)))
    ent_miss = {e for e in entries if e not in S0}
    print("ingressi di routine visti: %d | non trovati staticamente: %d" % (len(entries), len(ent_miss)))
    for u, n in sorted(per_unit(miss).items(), key=lambda kv: -kv[1])[:12]:
        print("   unita' %2d: %5d byte non trovati" % (u, n))
    if loo:
        new, rest = loo
        En, _ = executed(new)
        rom1 = disasm.Rom(rom_path, [rest])
        S1 = code_set(rom1)
        m1 = En - S1
        print("\nleave-one-out: %s contiene %d opcode; con l'analisi statica + il resto della copertura ne trovo %d, miss %d (%.1f%%)" % (
            os.path.basename(new), len(En), len(En & S1), len(m1), 100.0 * len(m1) / max(len(En), 1)))
        for u, n in sorted(per_unit(m1).items(), key=lambda kv: -kv[1])[:8]:
            print("   unita' %2d: %5d byte" % (u, n))


if __name__ == "__main__":
    main()
