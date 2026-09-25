#!/usr/bin/env python3
"""Collaudo differenziale: le funzioni decompilate devono dare lo STESSO stato finale dell'interprete puro.

  python decomp_check.py RUNNER_DIR ROM --exe NOME.exe [--env KEY=VAL] [--frames 2000] [--seeds 2] [--states a.sav ...]

Per ogni (savestate, seme): stessi input casuali, una volta con NESRECOMP_DECOMP=0 (solo interprete + blocchi) e
una con le funzioni decompilate; confronta l'hash del savestate finale (RAM, PPU, mapper, CPU).
"""
import argparse, glob, hashlib, os, random, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from explore import make_script


def run(runner_dir, rom, script, env_extra, timeout, exe="game.exe", base_env=None):
    env = dict(os.environ, **(base_env or {}), **env_extra)
    r = subprocess.run([os.path.join(runner_dir, exe), rom, "--script", script], cwd=runner_dir,
                       env=env, capture_output=True, text=True, timeout=timeout)
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runner_dir")
    ap.add_argument("rom")
    ap.add_argument("--frames", type=int, default=2000)
    ap.add_argument("--seeds", type=int, default=2)
    ap.add_argument("--states", nargs="*")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--exe", default="game.exe", help="eseguibile nella cartella del runner")
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE aggiunta all'ambiente")
    a = ap.parse_args()
    base_env = dict(kv.split("=", 1) for kv in a.env)
    rd, rom = os.path.abspath(a.runner_dir), os.path.abspath(a.rom)
    states = [os.path.abspath(s) for s in a.states] if a.states else sorted(glob.glob(os.path.join(rd, "savestates", "*.sav")))
    tmp = tempfile.mkdtemp(prefix="mmc5chk_")
    bad = 0
    for st in states:
        rel = os.path.relpath(st, rd).replace("\\", "/")
        for sd in range(a.seeds):
            tag = "%s_s%d" % (os.path.splitext(os.path.basename(st))[0], sd)
            hashes = {}
            for mode, env in (("interp", {"NESRECOMP_DECOMP": "0"}), ("decomp", {})):
                script = os.path.join(tmp, "%s_%s.txt" % (tag, mode))
                out = os.path.join(tmp, "%s_%s.sav" % (tag, mode)).replace("\\", "/")
                make_script(st, 500 + sd, a.frames, script, rel)
                s = open(script).read().replace("EXIT 0", "SAVE_STATE %s\nEXIT 0" % out)
                open(script, "w").write(s)
                try:
                    r = run(rd, rom, script, env, a.timeout, a.exe, base_env)
                except subprocess.TimeoutExpired:
                    hashes[mode] = "TIMEOUT"
                    continue
                hashes[mode] = hashlib.md5(open(out, "rb").read()).hexdigest() if os.path.exists(out) else "NOSAVE"
                if mode == "decomp":
                    dec = [l for l in r.stderr.splitlines() if "[interp] WATCHDOG" in l or "Fault" in l or "fault" in l]
                    if dec:
                        print("   avvisi:", dec[:2])
            ok = hashes["interp"] == hashes["decomp"] and hashes["interp"] not in ("NOSAVE", "TIMEOUT")
            bad += 0 if ok else 1
            print("%-12s %s  interp=%s decomp=%s" % (tag, "IDENTICO" if ok else "DIVERSO", hashes["interp"][:8], hashes["decomp"][:8]), flush=True)
    print("RISULTATO: %s" % ("tutti identici" if not bad else "%d diversi" % bad))
    return bad


if __name__ == "__main__":
    sys.exit(1 if main() else 0)
