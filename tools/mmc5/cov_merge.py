#!/usr/bin/env python3
"""Unisce (OR) file di copertura: cov_merge.py OUT.bin IN1.bin IN2.bin ...  (con .win/.ram/.wramw se presenti)."""
import os, sys

out, ins = sys.argv[1], sys.argv[2:]
for suffix in ("", ".win", ".ram", ".wramw"):
    acc = None
    for p in ins:
        q = p + suffix
        if not os.path.exists(q):
            continue
        b = open(q, "rb").read()
        if acc is None:
            acc = bytearray(b)
        else:
            if len(b) > len(acc):
                acc.extend(bytes(len(b) - len(acc)))
            for i, v in enumerate(b):
                acc[i] |= v
    if acc is not None:
        open(out + suffix, "wb").write(acc)
        print("scritto", out + suffix, len(acc))
