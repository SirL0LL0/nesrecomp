#!/usr/bin/env python3
"""Confronta due registri di istruzioni (NESRECOMP_BOUNDARY_LOG) e mostra la prima divergenza.

  python tools/seqdiff.py a.log b.log [--ctx 6]

Record da 12 byte: pc(2) A X Y S lo-ritorno hi-ritorno unita-nelle-4-finestre. Se lo stato (A,X,Y,S) diverge prima del pc, l'istruzione PRECEDENTE e' quella sbagliata.
"""
import sys


def main():
    a = open(sys.argv[1], "rb").read()
    b = open(sys.argv[2], "rb").read()
    ctx = int(sys.argv[sys.argv.index("--ctx") + 1]) if "--ctx" in sys.argv else 6
    if "--notop" in sys.argv:            # ignora i 2 byte in cima allo stack (P impilato dagli interrupt cambia con la potatura dei flag)
        strip = lambda d: b"".join(d[k:k + 6] + d[k + 8:k + 12] for k in range(0, len(d) - 11, 12))
        a, b = strip(a), strip(b)
        R = 10
    else:
        R = 12
    n = min(len(a), len(b)) // R
    i = 0
    # confronto veloce a blocchi
    step = R * 4096
    off = 0
    while off < min(len(a), len(b)) and a[off:off + step] == b[off:off + step]:
        off += step
    i = off // R
    while i < n and a[i * R:i * R + R] == b[i * R:i * R + R]:
        i += 1
    print("record: A=%d B=%d, identici per i primi %d" % (len(a) // R, len(b) // R, i))
    if i >= n:
        print("nessuna divergenza nel tratto comune")
        return 0
    for k in range(max(0, i - ctx), min(n, i + 3)):
        ra, rb = a[k * R:k * R + R], b[k * R:k * R + R]
        if R == 10:
            ra, rb = ra[:6] + b"\x00\x00" + ra[6:], rb[:6] + b"\x00\x00" + rb[6:]
        fmt = lambda r: "pc=%04X A=%02X X=%02X Y=%02X S=%02X top=%02X%02X w=%d,%d,%d,%d" % (r[0] | r[1] << 8, r[2], r[3], r[4], r[5], r[7], r[6], *[(x if x < 128 else -1) for x in r[8:12]])
        print("%s #%d  A: %s   B: %s" % (">>" if k == i else "  ", k, fmt(ra), fmt(rb)))
    return 1


if __name__ == "__main__":
    sys.exit(main())
