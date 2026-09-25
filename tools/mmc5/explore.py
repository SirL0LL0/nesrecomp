#!/usr/bin/env python3
"""Esplorazione automatica per la copertura: input casuali dai savestate, in headless, con registrazione del codice.

  python explore.py RUNNER_DIR ROM --exe NOME.exe [--frames 3000] [--seeds 3] [--out analysis/explore.bin] [--states a.sav b.sav ...]

Per ogni (savestate, seme) genera uno script (LOAD_STATE, input casuali in TURBO, EXIT), lancia il runner con
NESRECOMP_COV_FILE, poi unisce (OR) tutte le coperture in --out e stampa quanti opcode nuovi rispetto a analysis/all.bin.
Poi: python tools/cov_merge.py analysis/all.bin analysis/all.bin analysis/explore.bin  e rigenera disasm/blocchi.
"""
import argparse, glob, os, random, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))


def make_script(state, seed, frames, path, rel_state):
    rnd = random.Random(seed)
    lines = ["LOAD_STATE " + rel_state, "TURBO ON", "WAIT 30"]
    used = 30
    buttons = ["A"] * 8 + ["B"] * 2 + ["START"] + ["UP", "DOWN", "LEFT", "RIGHT"] * 3 + ["SELECT"]
    while used < frames:
        b = rnd.choice(buttons)
        hold = rnd.randint(4, 40) if b in ("UP", "DOWN", "LEFT", "RIGHT") else rnd.randint(2, 6)
        wait = rnd.randint(3, 30)
        lines += ["HOLD " + b, "WAIT %d" % hold, "RELEASE " + b, "WAIT %d" % wait]
        used += hold + wait
    lines.append("EXIT 0")
    open(path, "w").write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runner_dir")
    ap.add_argument("rom")
    ap.add_argument("--frames", type=int, default=3000)
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--out", default="analysis/explore.bin")
    ap.add_argument("--states", nargs="*")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--exe", default="game.exe", help="nome dell'eseguibile nella cartella del runner")
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE aggiunta all'ambiente (ripetibile)")
    a = ap.parse_args()
    a.runner_dir = os.path.abspath(a.runner_dir)
    a.rom = os.path.abspath(a.rom)
    a.out = os.path.abspath(a.out)
    a.states = [os.path.abspath(s) for s in a.states] if a.states else None
    runner = os.path.join(a.runner_dir, a.exe)
    states = a.states or sorted(glob.glob(os.path.join(a.runner_dir, "savestates", "*.sav")))
    tmp = tempfile.mkdtemp(prefix="mmc5exp_")
    covs = []
    env0 = dict(os.environ, **dict(kv.split("=", 1) for kv in a.env))
    for st in states:
        rel = os.path.relpath(st, a.runner_dir).replace("\\", "/")
        for sd in range(a.seeds):
            tag = "%s_s%d" % (os.path.splitext(os.path.basename(st))[0], sd)
            script = os.path.join(tmp, tag + ".txt")
            cov = os.path.join(tmp, tag + ".bin")
            make_script(st, 1000 + sd, a.frames, script, rel)
            env = dict(env0, NESRECOMP_COV_FILE=cov, NESRECOMP_BLOCK_MISS_FILE=os.path.join(tmp, tag + ".miss"))
            try:
                r = subprocess.run([runner, a.rom, "--script", script], cwd=a.runner_dir, env=env,
                                   capture_output=True, text=True, timeout=a.timeout)
                tail = [l for l in r.stderr.splitlines() if "[blocks] translated" in l]
                print("%-14s exit=%s %s" % (tag, r.returncode, tail[-1][:110] if tail else ""), flush=True)
            except subprocess.TimeoutExpired:
                print("%-14s TIMEOUT" % tag, flush=True)
            if os.path.exists(cov):
                covs.append(cov)
    if not covs:
        sys.exit("nessuna copertura prodotta")
    subprocess.run([sys.executable, os.path.join(here, "cov_merge.py"), a.out] + covs, check=True, capture_output=True)
    new = open(a.out, "rb").read()
    base_p = os.path.join(here, "..", "analysis", "all.bin")
    base = open(base_p, "rb").read() if os.path.exists(base_p) else bytes(len(new))
    n_new = sum(1 for i, v in enumerate(new) if v & 1 and not base[i] & 1)
    print("copertura scritta in %s: %d opcode, di cui NUOVI rispetto a all.bin: %d" % (a.out, sum(1 for v in new if v & 1), n_new))
    print("miss (interprete su codice non tradotto): vedi i file .miss in", tmp)


if __name__ == "__main__":
    main()
