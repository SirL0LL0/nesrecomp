#!/usr/bin/env python3
"""End-to-end MMC5 checks with generated test ROMs (no game needed).

  python mmc5_rom_tests.py RUNNER.exe [--env KEY=VAL ...]

Builds ROMs with tools/mmc5/make_test_rom.py, runs each headless for 30 frames, takes a screenshot and checks pixel colours:
  split   left split: x<64 blue (ExRAM tiles through split CHR page), rest red (CIRAM)
  splitr  right split: x<64 red, x>=64 blue
  nt0..3  $5105 nametable sources: CIRAM0 red, ExRAM blue, fill green, CIRAM1 green
"""
import os, struct, subprocess, sys, tempfile, zlib


def read_png(path):
    """Minimal PNG reader (8-bit RGB/RGBA, non-interlaced): returns (w, h, rows of bytes, bytes per pixel)."""
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat = 8, b""
    while pos < len(d):
        n, typ = struct.unpack(">I4s", d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and ctype in (2, 6)
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        cur = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: cur[i] = (cur[i] + a) & 255
            elif f == 2: cur[i] = (cur[i] + b) & 255
            elif f == 3: cur[i] = (cur[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 255
        rows.append(cur)
        prev = cur
    return w, h, rows, bpp

HERE = os.path.dirname(os.path.abspath(__file__))
MK = os.path.join(HERE, "..", "..", "tools", "mmc5", "make_test_rom.py")
RED, BLUE, GREEN = (152, 34, 32), (48, 50, 236), (76, 208, 32)
CASES = {
    "split": [(10, 100, BLUE), (60, 100, BLUE), (70, 100, RED), (200, 100, RED)],
    "splitr": [(10, 100, RED), (60, 100, RED), (70, 100, BLUE), (200, 100, BLUE)],
    "nt0": [(128, 120, RED)], "nt1": [(128, 120, BLUE)], "nt2": [(128, 120, GREEN)], "nt3": [(128, 120, GREEN)],
}


def main():
    exe = os.path.abspath(sys.argv[1])
    env = dict(os.environ)
    for a in sys.argv[2:]:
        if a.startswith("KEY=") or "=" in a:
            k, v = a.split("=", 1)
            env[k] = v
    tmp = tempfile.mkdtemp(prefix="mmc5rom_")
    bad = 0
    for name, checks in CASES.items():
        rom = os.path.join(tmp, name + ".nes")
        png = os.path.join(tmp, name + ".png").replace("\\", "/")
        subprocess.run([sys.executable, MK, name, rom], check=True, capture_output=True)
        script = os.path.join(tmp, name + ".txt")
        open(script, "w").write("WAIT 30\nSCREENSHOT %s\nEXIT 0\n" % png)
        subprocess.run([exe, rom, "--script", script], cwd=os.path.dirname(exe), env=env, capture_output=True, timeout=120)
        try:
            w, h, rows, bpp = read_png(png.replace("/", os.sep))
        except Exception as e:
            print("%-7s NESSUNO SCREENSHOT (%s)" % (name, e)); bad += 1; continue
        px = lambda x, y: tuple(rows[y][x * bpp:x * bpp + 3])
        ok = all(px(x, y) == c for x, y, c in checks)
        print("%-7s %s" % (name, "ok" if ok else "DIVERSO: " + str([px(x, y) for x, y, _ in checks])))
        bad += 0 if ok else 1
    print("RISULTATO:", "tutti ok" if not bad else "%d falliti" % bad)
    return bad


if __name__ == "__main__":
    sys.exit(1 if main() else 0)
