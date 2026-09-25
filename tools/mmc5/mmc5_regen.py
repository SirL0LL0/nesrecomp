#!/usr/bin/env python3
"""Backend MMC5 di nesrecomp: dalla ROM ai sorgenti C (blocchi tradotti + funzioni decompilate).

  python <nesrecomp>/tools/mmc5/mmc5_regen.py --rom game.nes [--cov analysis/all.bin ...] [--out generated/mmc5]
                                             [--no-readable] [--no-disasm]

Da lanciare nella cartella del gioco (dove stanno analysis/, generated/). Prodotti in --out:
  blocks/        blocchi base tradotti in C   (mmc5_blocks_*.c)   -> l'interprete li usa al posto della decodifica
  decomp/        funzioni C decompilate       (mmc5_dec_*.c)      -> chiamate dall'interprete / tra loro
  decomp_r/      sorgente leggibile (non si compila)
  ../../disasm/  assembly ca65-like, verificato riassemblabile (asm_verify)
La copertura (--cov, piu' file) e' quella scritta dal runner con NESRECOMP_COV_FILE (vedi README.md).
Senza copertura funziona ma trova meno codice: il modello MMC5 non permette di sapere staticamente quali banchi
siano mappati.
"""
import argparse, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))


def run(title, cmd):
    t = time.time()
    print("== %s" % title, flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True)
    for l in (r.stdout + r.stderr).strip().splitlines()[-5:]:
        print("   " + l)
    if r.returncode:
        print("   ERRORE (codice %d)" % r.returncode)
        sys.exit(r.returncode)
    print("   ok (%.0fs)" % (time.time() - t), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--cov", action="append", default=[])
    ap.add_argument("--out", default="generated/mmc5")
    ap.add_argument("--no-readable", action="store_true")
    ap.add_argument("--no-disasm", action="store_true")
    a = ap.parse_args()
    py = sys.executable
    cov = a.cov or ([os.path.join("analysis", "all.bin")] if os.path.exists(os.path.join("analysis", "all.bin")) else [])
    if not cov:
        print("ATTENZIONE: nessuna copertura (analysis/all.bin): il codice trovato sara' molto meno")
    if not a.no_disasm:
        run("disassemblaggio", [py, os.path.join(HERE, "disasm.py"), a.rom, "disasm"] + cov)
        run("verifica riassemblaggio", [py, os.path.join(HERE, "asm_verify.py"), a.rom, "disasm"])
    run("blocchi tradotti", [py, os.path.join(HERE, "blocks.py"), a.rom, os.path.join(a.out, "blocks")] + cov)
    run("decompilazione (eseguibile)", [py, os.path.join(HERE, "decompile.py"), a.rom, os.path.join(a.out, "decomp")] + cov)
    if not a.no_readable:
        run("decompilazione (leggibile)", [py, os.path.join(HERE, "decompile.py"), a.rom, os.path.join(a.out, "decomp_r")] + cov
            + ["--readable", "--closed"])
    print("\nFatto. In CMake: nesrecomp_mmc5_tier(<target> %s)" % a.out)


if __name__ == "__main__":
    main()
