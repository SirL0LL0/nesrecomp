#!/usr/bin/env python3
"""Genera piccole ROM di prova MMC5 (mapper 5) per collaudare il mapper senza un gioco.

  python make_test_rom.py splitr  OUT.nes   # come split, ma a destra: 192 pixel BLU a destra dal tile 8
  python make_test_rom.py split   OUT.nes   # split screen: 8 tile a sinistra dalla ExRAM con un'altra pagina CHR
  python make_test_rom.py ntN     OUT.nes   # nametable N (0-3) visibile; $5105: 0=CIRAM0 (rosso), 1=ExRAM (blu), 2=fill tile 2 (verde), 3=CIRAM1 (tile 2 = verde)

Split: attesa a schermo: 64 pixel a sinistra BLU (indice 3 dalla pagina CHR 1 del split), il resto ROSSO (indice 1).
"""
import sys

PRG_UNITS = 4        # 32KB
CHR_KB = 16


def asm():
    c = bytearray()

    def b(*x): c.extend(x)

    def sta(addr): b(0x8D, addr & 0xFF, addr >> 8)
    def lda(v): b(0xA9, v)
    b(0x78, 0xD8, 0xA2, 0xFF, 0x9A)                 # SEI CLD LDX #$FF TXS
    lda(0); sta(0x2000); sta(0x2001)
    for _ in range(2):
        b(0x2C, 0x02, 0x20, 0x10, 0xFB)              # BIT $2002 ; BPL -5
    # palette $3F00: 4 sub-palettes, entries 0F 16 2A 12
    lda(0x3F); sta(0x2006); lda(0); sta(0x2006)
    for _ in range(4):
        for col in (0x0F, 0x16, 0x2A, 0x12):
            lda(col); sta(0x2007)
    return c


def build_split(ctrl=0x88):
    c = asm()

    def b(*x): c.extend(x)
    def sta(addr): b(0x8D, addr & 0xFF, addr >> 8)
    def lda(v): b(0xA9, v)
    # nametable 0: 1024 bytes of tile 1
    lda(0x20); sta(0x2006); lda(0); sta(0x2006)
    lda(1); b(0xA0, 4)
    b(0xA2, 0)
    b(0x8D, 0x07, 0x20, 0xE8, 0xD0, 0xFA, 0x88, 0xD0, 0xF5)
    # ExRAM in CPU mode ($5104=2), 1024 bytes of tile 2
    lda(2); sta(0x5104)
    lda(2); b(0xA2, 0)
    b(0x9D, 0x00, 0x5C, 0x9D, 0x00, 0x5D, 0x9D, 0x00, 0x5E, 0x9D, 0x00, 0x5F, 0xE8, 0xD0, 0xF1)
    lda(0); sta(0x5104)                              # Ex0
    lda(ctrl); sta(0x5200)                           # split control ($88 = on, left side, 8 tiles; $C8 = right side)
    lda(0); sta(0x5201)
    lda(1); sta(0x5202)                              # split CHR: 4KB page 1
    lda(1); sta(0x5101)                              # CHR 4KB mode
    lda(0); sta(0x5123); sta(0x5127)                 # BG/sprites: page 0
    lda(0x80); sta(0x2000); lda(0); sta(0x2005); sta(0x2005)       # NMI on (the runner paces frames with it)
    lda(0x0A); sta(0x2001)
    return c


def build_nt(base=1):
    c = asm()

    def b(*x): c.extend(x)
    def sta(addr): b(0x8D, addr & 0xFF, addr >> 8)
    def lda(v): b(0xA9, v)
    lda(0x78); sta(0x5105)                           # map the nametables first: the PPU writes below depend on it
    # CIRAM page 0 ($2000): tile 1 everywhere; CIRAM page 1 ($2C00 via map): tile 2 everywhere
    for hi, tile in ((0x20, 1), (0x2C, 2)):
        lda(hi); sta(0x2006); lda(0); sta(0x2006)
        lda(tile); b(0xA0, 4); b(0xA2, 0)
        b(0x8D, 0x07, 0x20, 0xE8, 0xD0, 0xFA, 0x88, 0xD0, 0xF5)
    # ExRAM (mode 2) = tile 3 in the first 960 bytes; attributes 0
    lda(2); sta(0x5104)
    lda(3); b(0xA2, 0)
    b(0x9D, 0x00, 0x5C, 0x9D, 0x00, 0x5D, 0x9D, 0x00, 0x5E, 0x9D, 0x00, 0x5F, 0xE8, 0xD0, 0xF1)
    lda(0); sta(0x5104)
    # $5105: slot0 = CIRAM0 (%00), slot1 = ExRAM (%10), slot2 = fill (%11), slot3 = CIRAM1 (%01)  -> %01 11 10 00
    lda(0x78); sta(0x5105)
    lda(2); sta(0x5106)                              # fill tile 2 (green)
    lda(0); sta(0x5107)
    lda(1); sta(0x5101)
    lda(0); sta(0x5123); sta(0x5127)
    lda(0x80 | base); sta(0x2000)                    # NMI on, base nametable = `base`
    lda(0); sta(0x2005); sta(0x2005)
    lda(0x0A); sta(0x2001)
    return c


def finish(code):
    assert len(code) < 0x1F00
    prg = bytearray(b"\xFF" * (PRG_UNITS * 0x2000))
    base = (PRG_UNITS - 1) * 0x2000
    prg[base:base + len(code)] = code
    loop = base + len(code)
    prg[loop:loop + 3] = bytes([0x4C, (0xE000 + len(code)) & 0xFF, (0xE000 + len(code)) >> 8])   # JMP self
    nmi = loop + 3
    prg[nmi] = 0x40                                   # RTI
    vec = base + 0x1FFA
    prg[vec:vec + 6] = bytes([(0xE000 + len(code) + 3) & 0xFF, (0xE000 + len(code) + 3) >> 8, 0x00, 0xE0,
                              (0xE000 + len(code) + 3) & 0xFF, (0xE000 + len(code) + 3) >> 8])
    chr_ = bytearray(CHR_KB * 1024)
    def tile(page, n, p0, p1):
        o = page * 0x1000 + n * 16
        chr_[o:o + 8] = bytes([p0] * 8)
        chr_[o + 8:o + 16] = bytes([p1] * 8)
    for pg in (0, 1):
        tile(pg, 0, 0, 0)
    tile(0, 1, 0xFF, 0x00)      # idx 1
    tile(0, 2, 0x00, 0xFF)      # idx 2
    tile(0, 3, 0xFF, 0xFF)      # idx 3
    tile(1, 1, 0xFF, 0xFF)
    tile(1, 2, 0xFF, 0xFF)      # split page: tile 2 = idx 3
    hdr = bytearray(b"NES\x1a") + bytes([PRG_UNITS // 2, CHR_KB // 8, 0x50, 0x00]) + bytes(8)
    return bytes(hdr + prg + chr_)


if __name__ == "__main__":
    kind, out = sys.argv[1], sys.argv[2]
    rom = finish(build_split(0xC8 if kind == "splitr" else 0x88)) if kind.startswith("split") else finish(build_nt(int(kind[2:]) if len(kind) > 2 else 1))
    open(out, "wb").write(rom)
    print("scritta", out)
