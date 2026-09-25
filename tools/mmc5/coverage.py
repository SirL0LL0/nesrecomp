#!/usr/bin/env python3
"""Unisce le registrazioni di codice eseguito (CDL Mesen e jb_exec.bin) e riporta la copertura.

  python tools/coverage.py report a.cdl b.cdl jb_exec.bin ...
  python tools/coverage.py merge -o merged.bin a.cdl jb_exec.bin ...
  python tools/coverage.py entries -o entries.csv a.cdl jb_exec.bin ...

Formato interno: 1 byte per byte di PRG; bit0 = codice eseguito, bit1 = ingresso (target di salto/chiamata).
Da CDL Mesen: bit0 <- flag 0x01 (Code), bit1 <- flag 0x04|0x08 (JumpTarget/SubEntryPoint).
Un file e' un CDL se inizia con 'CDLv2' (i primi 9 byte sono header), altrimenti e' un exec.bin.
"""
import argparse, os, sys

PRG_SIZE = 512 * 1024


def load(path):
    d = open(path, "rb").read()
    if d[:5] == b"CDLv2":
        d = d[9:9 + PRG_SIZE]
        # Il flag Code di Mesen copre TUTTI i byte di un'istruzione (opcode + operandi): ricostruisco gli opcode
        # decodificando dall'inizio di ogni tratto (o da un bersaglio di salto); gli operandi diventano bit2.
        here = os.path.dirname(os.path.abspath(__file__))
        sys.path.insert(0, here)
        import disasm
        rp = os.environ.get("CDL_ROM")   # la ROM da cui e' stato registrato il CDL (serve per la lunghezza delle istruzioni)
        if not rp:
            sys.exit("CDL: imposta CDL_ROM=percorso della ROM usata con Mesen")
        rd = open(rp, "rb").read()
        prg = rd[16:16 + PRG_SIZE]
        out = bytearray(len(d))
        i = 0
        while i < len(d):
            b = d[i]
            if not b & 1:
                i += 1
                continue
            name, mode = disasm.OPS[prg[i]]
            ln = disasm.LEN[mode] if name != "???" else 1
            out[i] = 1 | (2 if b & 0x0C else 0)
            for k in range(1, ln):
                if i + k < len(d) and d[i + k] & 1 and not d[i + k] & 0x0C:
                    out[i + k] |= 4
                else:
                    break                    # tratto troncato: il byte dopo e' un nuovo inizio
            i += ln
        return bytes(out)
    if len(d) != PRG_SIZE:
        sys.exit("%s: dimensione %d inattesa (PRG=%d)" % (path, len(d), PRG_SIZE))
    return bytes(b & 7 for b in d)      # exec.bin / cov: bit0 opcode, bit1 entry, bit2 operando


def union(paths):
    acc = bytearray(PRG_SIZE)
    for p in paths:
        d = load(p)
        n = sum(1 for b in d if b & 1)
        new = sum(1 for i, b in enumerate(d) if (b & 1) and not (acc[i] & 1))
        print("%-40s code %7d   nuovi %7d" % (os.path.basename(p), n, new))
        for i, b in enumerate(d):
            acc[i] |= b
    return acc


def report(paths):
    acc = union(paths)
    code = sum(1 for b in acc if b & 1)
    print("\nUNIONE: %d byte di codice = %.2f%% del PRG" % (code, 100 * code / PRG_SIZE))
    print("\nbanco 8K: byte-codice  ingressi")
    used = 0
    for u in range(PRG_SIZE // 8192):
        seg = acc[u * 8192:(u + 1) * 8192]
        c, e = sum(1 for b in seg if b & 1), sum(1 for b in seg if b & 2)
        if c:
            used += 1
            print("  %2d: %6d  %5d" % (u, c, e))
    print("banchi con codice: %d / %d" % (used, PRG_SIZE // 8192))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["report", "merge", "entries"])
    ap.add_argument("files", nargs="+")
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    if a.cmd == "report":
        report(a.files)
        return
    acc = union(a.files)
    if not a.out:
        sys.exit("serve -o")
    if a.cmd == "merge":
        open(a.out, "wb").write(bytes(acc))
    else:
        with open(a.out, "w") as f:
            f.write("bank8k,cpu_window,addr_in_bank,rom_offset\n")
            for i, b in enumerate(acc):
                if b & 2:
                    u, o = divmod(i, 8192)
                    f.write("%d,,%04X,%05X\n" % (u, o, i))
    print("scritto", a.out)


if __name__ == "__main__":
    main()
