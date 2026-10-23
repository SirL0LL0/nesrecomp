#!/usr/bin/env python3
"""Serial, order-balanced process-CPU measurements of existing cycle hosts.

Correctness captures belong in separate runs. Both binaries get identical
arguments, including audio output: a headless host otherwise disables PCM.
The lock is advisory across tasks; pass the same --lock from every window.
"""
import argparse
import contextlib
import ctypes
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time


@contextlib.contextmanager
def measurement_lock(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('a+b') as handle:
        handle.seek(0)
        if os.name == 'nt':
            import msvcrt
            if not path.stat().st_size:
                handle.write(b'\0'); handle.flush(); handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            yield
        finally:
            handle.seek(0)
            if os.name == 'nt':
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle, fcntl.LOCK_UN)


def run_cpu(command, cwd, log, timeout):
    before = time.perf_counter()
    if os.name != 'nt':
        import resource
        prior = resource.getrusage(resource.RUSAGE_CHILDREN)
    startup = None
    if os.name == 'nt':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    with log.open('wb') as output:
        with subprocess.Popen(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT,
                              startupinfo=startup,
                              creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)) as proc:
            try:
                status = proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait()
                raise
            wall = time.perf_counter() - before
            if os.name == 'nt':
                times = [ctypes.c_uint64() for _ in range(4)]
                fn = ctypes.windll.kernel32.GetProcessTimes
                fn.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_uint64)] * 4
                fn.restype = ctypes.c_int
                if not fn(int(proc._handle), *(ctypes.byref(v) for v in times)):
                    raise ctypes.WinError()
                cpu = (times[2].value + times[3].value) / 10_000_000
            else:
                after = resource.getrusage(resource.RUSAGE_CHILDREN)
                cpu = after.ru_utime + after.ru_stime - prior.ru_utime - prior.ru_stime
    if status:
        raise RuntimeError(f'{command[0]} returned {status}; see {log}')
    return dict(cpu_seconds=cpu, wall_seconds=wall, command=command)


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline', 'candidate', 'rom', 'out', 'lock'):
        ap.add_argument('--' + name, type=Path, required=True)
    ap.add_argument('--frames', type=int, default=1800)
    ap.add_argument('--blocks', type=int, default=3)
    ap.add_argument('--timeout', type=float, default=180)
    ap.add_argument('--input', type=Path)
    ap.add_argument('--extra', nargs=argparse.REMAINDER, default=[])
    args = ap.parse_args()
    if args.frames <= 0 or args.blocks <= 0:
        ap.error('frames and blocks must be positive')
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    executables = dict(A=args.baseline.resolve(), B=args.candidate.resolve())
    manifest = dict(executables={k: dict(path=str(v), sha256=sha256(v)) for k, v in executables.items()},
                    rom_sha256=sha256(args.rom), frames=args.frames, blocks=args.blocks,
                    input_sha256=sha256(args.input) if args.input else None,
                    measurement='process user + kernel CPU, including startup and warmup', samples=[])
    # Warm both binaries, then A/A controls and ABBA blocks. No profiler,
    # --hash-out, or per-frame disk dump belongs in these measurement legs.
    orders = [('warmup', 'AB'), ('AA', 'AAAA')]
    orders += [('ABBA', 'ABBA' if i % 2 == 0 else 'BAAB') for i in range(args.blocks)]
    with measurement_lock(args.lock.resolve()):
        for kind, order in orders:
            for label in order:
                number = len(manifest['samples'])
                wav = out / 'audio.wav'
                command = [str(executables[label]), str(args.rom.resolve()), '--headless',
                           '--frames', str(args.frames), '--no-save', '--wav-out', str(wav)]
                if args.input:
                    command += ['--input', str(args.input.resolve())]
                command += args.extra
                row = run_cpu(command, out, out / f'{number:02d}-{label}.log', args.timeout)
                row.update(kind=kind, label=label, pcm_wav_sha256=sha256(wav))
                manifest['samples'].append(row)
                (out / 'samples.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
                print(f'{number:02d} {kind} {label}: cpu={row["cpu_seconds"]:.6f} wall={row["wall_seconds"]:.6f}', flush=True)
    samples = manifest['samples']
    aa = [s['cpu_seconds'] for s in samples if s['kind'] == 'AA']
    aa_deltas = [(aa[i] - aa[i+1]) / aa[i] * 100 for i in (0, 2)]
    measured = [s for s in samples if s['kind'] == 'ABBA']
    deltas = []
    for start in range(0, len(measured), 4):
        group = measured[start:start+4]
        a = statistics.mean(s['cpu_seconds'] for s in group if s['label'] == 'A')
        b = statistics.mean(s['cpu_seconds'] for s in group if s['label'] == 'B')
        deltas.append((a-b)/a*100)
    summary = dict(aa_reduction_percent=aa_deltas, abba_reduction_percent=deltas,
                   median_reduction_percent=statistics.median(deltas),
                   all_audio_equal=len({s['pcm_wav_sha256'] for s in samples}) == 1,
                   acceptance='Requires separate correctness gates and review against A/A noise; no automatic acceptance')
    (out / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
