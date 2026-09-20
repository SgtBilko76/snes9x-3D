#!/usr/bin/env python3
"""Generates a small public-domain SNES test ROM for the VR port.

The ROM draws two backgrounds that scroll at different speeds: a dense
colourful BG2 and a sparse BG1 of solid blocks over it.  That gives us a
moving picture to confirm the emulation and display path work, and real
parallax material for the stereo passes later on.
"""

import struct
import sys

# --- tiny 65816 assembler ---------------------------------------------------

class Assembler:
    def __init__(self, origin=0x8000):
        self.origin = origin
        self.code = bytearray()
        self.labels = {}
        self.fixups = []   # (offset, label, kind)

    @property
    def pc(self):
        return self.origin + len(self.code)

    def label(self, name):
        self.labels[name] = self.pc

    def emit(self, *values):
        self.code.extend(values)

    def abs_op(self, opcode, addr):
        self.emit(opcode, addr & 0xff, (addr >> 8) & 0xff)

    def abs_label(self, opcode, name):
        self.emit(opcode)
        self.fixups.append((len(self.code), name, "abs"))
        self.emit(0, 0)

    def branch(self, opcode, name):
        self.emit(opcode)
        self.fixups.append((len(self.code), name, "rel"))
        self.emit(0)

    def resolve(self):
        for offset, name, kind in self.fixups:
            target = self.labels[name]
            if kind == "abs":
                self.code[offset] = target & 0xff
                self.code[offset + 1] = (target >> 8) & 0xff
            else:
                delta = target - (self.origin + offset + 1)
                assert -128 <= delta <= 127, f"branch to {name} out of range ({delta})"
                self.code[offset] = delta & 0xff

    # Instructions actually used below.
    def sei(self):          self.emit(0x78)
    def clc(self):          self.emit(0x18)
    def xce(self):          self.emit(0xfb)
    def rep(self, v):       self.emit(0xc2, v)
    def sep(self, v):       self.emit(0xe2, v)
    def lda_imm8(self, v):  self.emit(0xa9, v)
    def lda_imm16(self, v): self.emit(0xa9, v & 0xff, (v >> 8) & 0xff)
    def lda_dp(self, v):    self.emit(0xa5, v)
    def lda_abs(self, a):   self.abs_op(0xad, a)
    def lda_absx(self, n):  self.abs_label(0xbd, n)
    def sta_abs(self, a):   self.abs_op(0x8d, a)
    def stz_abs(self, a):   self.abs_op(0x9c, a)
    def stz_dp(self, v):    self.emit(0x64, v)
    def ldx_imm16(self, v): self.emit(0xa2, v & 0xff, (v >> 8) & 0xff)
    def stx_abs(self, a):   self.abs_op(0x8e, a)
    def inx(self):          self.emit(0xe8)
    def cpx_imm16(self, v): self.emit(0xe0, v & 0xff, (v >> 8) & 0xff)
    def bne(self, n):       self.branch(0xd0, n)
    def beq(self, n):       self.branch(0xf0, n)
    def bra(self, n):       self.branch(0x80, n)
    def inc_dp(self, v):    self.emit(0xe6, v)
    def and_imm8(self, v):  self.emit(0x29, v)
    def lsr_a(self):        self.emit(0x4a)
    def tcs(self):          self.emit(0x1b)
    def phk(self):          self.emit(0x4b)
    def plb(self):          self.emit(0xab)
    def rti(self):          self.emit(0x40)


# --- graphics data ----------------------------------------------------------

def make_palette():
    """Eight 16-colour palettes sweeping through hue and brightness."""
    data = bytearray()
    for palette in range(8):
        for index in range(16):
            if index == 0:
                data += struct.pack("<H", 0)   # transparent / backdrop
                continue

            # A different hue per palette, ramped in brightness per index.
            level = index * 2
            red   = (level * ((palette + 1) & 3) // 3) & 0x1f
            green = (level * ((palette + 2) & 3) // 3) & 0x1f
            blue  = (level * ((palette + 3) & 3) // 3) & 0x1f
            data += struct.pack("<H", (blue << 10) | (green << 5) | red)
    return bytes(data)


def make_tiles():
    """Sixteen 4bpp tiles; tile N is a solid block of colour index N."""
    data = bytearray()
    for tile in range(16):
        for plane_pair in (0, 2):
            for row in range(8):
                low = 0xff if tile & (1 << plane_pair) else 0x00
                high = 0xff if tile & (1 << (plane_pair + 1)) else 0x00
                data.append(low)
                data.append(high)
    return bytes(data)


def make_map(sparse):
    """A 32x32 tilemap.  Sparse maps leave most tiles transparent so the layer
    below shows through, which is what makes the parallax visible."""
    data = bytearray()
    for y in range(32):
        for x in range(32):
            if sparse:
                solid = (x % 5 == 0) and (y % 3 == 0)
                tile = (1 + (x + y) % 15) if solid else 0
                palette = (x // 5) % 8
            else:
                tile = 1 + (x * 3 + y * 5) % 15
                palette = (y // 4) % 8
            data += struct.pack("<H", (palette << 10) | tile)
    return bytes(data)


# --- ROM assembly -----------------------------------------------------------

def build():
    palette = make_palette()
    tiles = make_tiles()
    map_bg1 = make_map(sparse=True)
    map_bg2 = make_map(sparse=False)

    asm = Assembler(0x8000)

    asm.label("reset")
    asm.sei()
    asm.clc()
    asm.xce()                       # native mode
    asm.rep(0x38)                   # 16-bit A/X, binary mode
    asm.lda_imm16(0x1fff)
    asm.tcs()
    asm.phk()
    asm.plb()                       # data bank = program bank
    asm.sep(0x20)                   # 8-bit accumulator

    asm.lda_imm8(0x8f)
    asm.sta_abs(0x2100)             # forced blank while we set up

    # Palette into CGRAM.
    asm.stz_abs(0x2121)
    asm.ldx_imm16(0)
    asm.label("pal_loop")
    asm.lda_absx("palette")
    asm.sta_abs(0x2122)
    asm.inx()
    asm.cpx_imm16(len(palette))
    asm.bne("pal_loop")

    # Tiles into VRAM word 0.
    asm.lda_imm8(0x80)
    asm.sta_abs(0x2115)             # increment after the high byte
    asm.ldx_imm16(0x0000)
    asm.stx_abs(0x2116)
    asm.ldx_imm16(0)
    asm.label("tile_loop")
    asm.lda_absx("tiles")
    asm.sta_abs(0x2118)
    asm.inx()
    asm.lda_absx("tiles")
    asm.sta_abs(0x2119)
    asm.inx()
    asm.cpx_imm16(len(tiles))
    asm.bne("tile_loop")

    # BG1 tilemap into VRAM word $4000.
    asm.ldx_imm16(0x4000)
    asm.stx_abs(0x2116)
    asm.ldx_imm16(0)
    asm.label("map1_loop")
    asm.lda_absx("map_bg1")
    asm.sta_abs(0x2118)
    asm.inx()
    asm.lda_absx("map_bg1")
    asm.sta_abs(0x2119)
    asm.inx()
    asm.cpx_imm16(len(map_bg1))
    asm.bne("map1_loop")

    # BG2 tilemap into VRAM word $4800.
    asm.ldx_imm16(0x4800)
    asm.stx_abs(0x2116)
    asm.ldx_imm16(0)
    asm.label("map2_loop")
    asm.lda_absx("map_bg2")
    asm.sta_abs(0x2118)
    asm.inx()
    asm.lda_absx("map_bg2")
    asm.sta_abs(0x2119)
    asm.inx()
    asm.cpx_imm16(len(map_bg2))
    asm.bne("map2_loop")

    asm.lda_imm8(0x01)
    asm.sta_abs(0x2105)             # BG mode 1, 8x8 tiles
    asm.lda_imm8(0x40)
    asm.sta_abs(0x2107)             # BG1 map at word $4000
    asm.lda_imm8(0x48)
    asm.sta_abs(0x2108)             # BG2 map at word $4800
    asm.lda_imm8(0x00)
    asm.sta_abs(0x210b)             # both BGs use the tiles at word 0
    asm.lda_imm8(0x03)
    asm.sta_abs(0x212c)             # BG1 + BG2 on the main screen
    asm.lda_imm8(0x0f)
    asm.sta_abs(0x2100)             # display on, full brightness

    asm.stz_dp(0x00)

    asm.label("main")
    asm.label("wait_vbl")
    asm.lda_abs(0x4212)
    asm.and_imm8(0x80)
    asm.beq("wait_vbl")

    asm.inc_dp(0x00)
    asm.lda_dp(0x00)
    asm.sta_abs(0x210d)             # BG1 scrolls at full speed
    asm.stz_abs(0x210d)
    asm.lda_dp(0x00)
    asm.lsr_a()
    asm.sta_abs(0x2110)             # BG2 scrolls at half speed
    asm.stz_abs(0x2110)

    asm.label("wait_active")
    asm.lda_abs(0x4212)
    asm.and_imm8(0x80)
    asm.bne("wait_active")
    asm.bra("main")

    asm.label("irq")
    asm.rti()

    # Data follows the code.
    asm.label("palette");  asm.code.extend(palette)
    asm.label("tiles");    asm.code.extend(tiles)
    asm.label("map_bg1");  asm.code.extend(map_bg1)
    asm.label("map_bg2");  asm.code.extend(map_bg2)

    asm.resolve()

    rom = bytearray(b"\x00" * 0x8000)
    rom[0:len(asm.code)] = asm.code

    # LoROM header at $7FC0.
    title = b"SNES9X VR TEST ROM   "[:21].ljust(21, b" ")
    rom[0x7fc0:0x7fd5] = title
    rom[0x7fd5] = 0x20              # LoROM, slow
    rom[0x7fd6] = 0x00              # ROM only
    rom[0x7fd7] = 0x07              # 128 KB; snes9x calls anything smaller corrupt
    rom[0x7fd8] = 0x00              # no SRAM
    rom[0x7fd9] = 0x01              # NTSC
    rom[0x7fda] = 0x33
    rom[0x7fdb] = 0x00

    irq = asm.labels["irq"]
    reset = asm.labels["reset"]
    for vector in range(0x7fe4, 0x7ff0, 2):     # native vectors
        rom[vector:vector + 2] = struct.pack("<H", irq)
    for vector in range(0x7ff4, 0x8000, 2):     # emulation vectors
        rom[vector:vector + 2] = struct.pack("<H", irq)
    rom[0x7ffc:0x7ffe] = struct.pack("<H", reset)

    rom[0x7fdc:0x7fde] = struct.pack("<H", 0x0000)
    rom[0x7fde:0x7fe0] = struct.pack("<H", 0xffff)
    # Pad out to the size the header advertises so the core stops flagging it.
    rom.extend(b"\x00" * (0x20000 - len(rom)))

    checksum = sum(rom) & 0xffff
    rom[0x7fdc:0x7fde] = struct.pack("<H", checksum ^ 0xffff)
    rom[0x7fde:0x7fe0] = struct.pack("<H", checksum)

    return bytes(rom)


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "test.sfc"
    data = build()
    with open(path, "wb") as handle:
        handle.write(data)
    print(f"wrote {path} ({len(data)} bytes)")
