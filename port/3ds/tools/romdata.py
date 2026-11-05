#!/usr/bin/env python3
"""Bring-your-own-ROM packaging: take the game data out of a linked ELF.

The game build compiles some data straight from the ROM into .data: the
images, palettes and fonts behind INCLUDE_IMG / INCLUDE_PAL / INCLUDE_RAW
(port/3ds/build_assets, see gen_include_assets.py) and the vertex lists and
display lists of assets/<ver>/**/*.{vtx,gfx}.inc.c. A build meant for other
people must not contain them, so this tool

  * finds every such object in the ELF and the ROM range it came from,
  * checks that the object can be rebuilt bit for bit from that range
    (raw bytes; Vtx = big-endian shorts; Gfx = big-endian words, except
    the address words the linker relocated),
  * writes a copy of the ELF with the objects zeroed, except those address
    words: they point into this program (not ROM data), and the .3dsx
    loader relocates them, and
  * writes romdata.bin, which pm3ds_rom.c applies at boot, once the
    player's ROM is loaded.

The table holds ROM offsets and sizes, and where the kept address words are
(with their link-time values, as a check); never ROM bytes. Any word that
differs from the ROM other than a relocated one fails the run.

usage: romdata.py <in.elf> <rom.z64> <out.elf> <out romdata.bin>
"""
import os
import re
import struct
import sys
import zlib

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(TOOLS, "..", "..", ".."))
# PM3DS_VERSION=pal, us or jp, as for make
VER = os.environ.get("PM3DS_VERSION", "pal")
ASSETS = os.path.join(ROOT, "assets", VER)
BUILD_ASSETS = os.path.join(ROOT, "port", "3ds", "build_assets" + ("" if VER == "pal" else "_" + VER))

sys.path.insert(0, TOOLS)
import gen_include_assets  # noqa: E402  (splat index)

MAGIC = b"PMRD"
VERSION = 1
KIND_RAW, KIND_VTX, KIND_GFX = 0, 1, 2
ANCHOR = "pm3ds_romdata_anchor"
STATE = "pm3ds_romdata_state"
# A combined program (tools/universal.py) holds one copy of the game per
# version; every symbol of the non-PAL copies carries the version's prefix.
# ROMDATA_PREFIX picks the copy this run strips ("" = the PAL copy).
PREFIX = os.environ.get("ROMDATA_PREFIX", "")
COPY_PREFIXES = ("us_", "iq_")
# the section holding the copy's data: .data in a one-version program, the
# copy's own .copy_<v>_data in the combined one (universal/universal.ld)
SECTION = os.environ.get("ROMDATA_SECTION", ".data")
# the game's data, which the game checks and keeps (pm3ds_rom_layout.h): PAL's
# 64 MB dumps repeat 0x2000000-0x3000000 after it (splat.yaml)
ROM_NAME, ROM_DATA_SIZE = {"pal": (b"Paper Mario (Europe)", 0x3000000),
                           "us": (b"Paper Mario (USA)", 0x2800000),
                           "jp": (b"Mario Story (Japan)", 0x2800000),
                           "ique": (b"Paper Mario (iQue)", 0x2800000)}[VER]

SHT_SYMTAB, SHT_REL = 2, 9
R_ARM_ABS32 = 2


class Elf:
    def __init__(self, data):
        self.data = data
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            sys.exit("not a 32-bit little-endian ELF")
        shoff, = struct.unpack_from("<I", data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
        self.sections = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
                "<IIIIIIIIII", data, shoff + i * shentsize)
            self.sections.append(dict(name=name, type=typ, flags=flags, addr=addr, off=off, size=size,
                                      link=link, info=info, entsize=entsize))
        strtab = self.sections[shstrndx]
        for s in self.sections:
            s["name"] = self._str(strtab, s["name"])
        self.by_name = {s["name"]: s for s in self.sections}

    def _str(self, sec, off):
        b = self.data[sec["off"] + off: self.data.index(b"\0", sec["off"] + off)]
        return b.decode()

    def symbols(self):
        out = {}
        for s in self.sections:
            if s["type"] != SHT_SYMTAB:
                continue
            strs = self.sections[s["link"]]
            for i in range(s["size"] // 16):
                name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", self.data, s["off"] + i * 16)
                if name == 0 or shndx == 0:
                    continue
                n = self._str(strs, name)
                # prefer the global definition if a local one shares the name
                if n not in out or (info >> 4) == 1:
                    out[n] = (value, size, shndx)
        return out

    def abs32_relocs(self, target):
        """vaddrs of every R_ARM_ABS32 word in the section named `target`"""
        idx = self.sections.index(self.by_name[target])
        out = set()
        for s in self.sections:
            if s["type"] == SHT_REL and s["info"] == idx:
                for i in range(s["size"] // 8):
                    off, info = struct.unpack_from("<II", self.data, s["off"] + i * 8)
                    if info & 0xFF == R_ARM_ABS32:
                        out.add(off)
        return out

    def file_off(self, addr, size, prefer=None):
        # the copies' sections share their addresses: look in the copy's first
        if prefer in self.by_name:
            s = self.by_name[prefer]
            if s["addr"] <= addr and addr + size <= s["addr"] + s["size"]:
                return s["name"], s["off"] + addr - s["addr"]
        for s in self.sections:
            if s["addr"] and s["type"] != 8 and s["addr"] <= addr and addr + size <= s["addr"] + s["size"]:
                return s["name"], s["off"] + addr - s["addr"]
        return None, None


def swap16(b):
    out = bytearray(len(b))
    out[0::2], out[1::2] = b[1::2], b[0::2]
    return bytes(out)


def swap32(b):
    out = bytearray(len(b))
    out[0::4], out[1::4], out[2::4], out[3::4] = b[3::4], b[2::4], b[1::4], b[0::4]
    return bytes(out)


def vtx_native(b):
    """N64 Vtx (16 bytes: 6 big-endian shorts + 4 colour bytes) -> this port's layout"""
    out = bytearray()
    for i in range(0, len(b), 16):
        out += swap16(b[i:i + 12]) + b[i + 12:i + 16]
    return bytes(out)


def include_blobs(syms):
    """INCLUDE_IMG/PAL/RAW objects: (symbol, rom start, size)"""
    ranges = {}
    manifest = os.path.join(BUILD_ASSETS, "manifest.tsv")
    if not os.path.exists(manifest):
        sys.exit("%s missing: run tools/gen_include_assets.py (it writes the asset bins too)" % manifest)
    with open(manifest) as fh:
        for line in fh:
            kind, path, start, size = line.rstrip("\n").split("\t")
            ranges[(kind, path)] = (int(start, 16), int(size))
    # the symbol is a plain name, or N(name) in files shared by several maps:
    # one copy per map, named <namespace>_name
    inc = re.compile(r'INCLUDE_(IMG|PAL|RAW)\(\s*"([^"]+)"\s*,\s*(N\(\s*)?(\w+)\s*\)?\s*\)')
    out = []
    for dp, _, files in os.walk(os.path.join(ROOT, "src")):
        for f in files:
            if not f.endswith(".c"):
                continue
            for kind, path, ns, sym in inc.findall(open(os.path.join(dp, f), errors="replace").read()):
                if (kind, path) not in ranges:
                    continue
                names = [n for n in syms if n.endswith("_" + sym)] if ns else [sym] if sym in syms else []
                out += [(n,) + ranges[(kind, path)] for n in names]
    return out


def incc_blobs(syms, index):
    """vtx/gfx .inc.c arrays: (symbol, kind, rom start, size), in file order"""
    decl = re.compile(r"^\s*(Vtx|Gfx)\s+(\w+)\s*\[[^\]]*\]\s*=", re.M)
    out, unplaced = [], []
    for dp, _, files in os.walk(ASSETS):
        for f in files:
            m = re.match(r"^(.*)\.(vtx|gfx)\.inc\.c$", f)
            if not m:
                continue
            rel = os.path.relpath(os.path.join(dp, m.group(1)), ASSETS).replace("\\", "/")
            ent = index.get((m.group(2), rel))
            arrays = [(t, n) for t, n in decl.findall(open(os.path.join(dp, f), errors="replace").read())
                      if n in syms]
            if not arrays:
                continue
            if not ent:
                unplaced.append(rel)
                continue
            start = ent[0]
            for t, n in arrays:
                size = syms[n][1]
                out.append((n, KIND_VTX if t == "Vtx" else KIND_GFX, start, size))
                start += size
    return out, unplaced


def main():
    in_elf, rom_path, out_elf, out_table = sys.argv[1:5]
    elf_data = bytearray(open(in_elf, "rb").read())
    elf = Elf(bytes(elf_data))
    rom = open(rom_path, "rb").read()
    if rom[:4] != b"\x80\x37\x12\x40":
        sys.exit("the ROM must be in .z64 (big-endian) byte order")
    allsyms = elf.symbols()
    if PREFIX:
        syms = {n[len(PREFIX):]: v for n, v in allsyms.items() if n.startswith(PREFIX)}
    else:
        syms = {n: v for n, v in allsyms.items() if not n.startswith(COPY_PREFIXES)}
    if ANCHOR not in syms:
        if not os.environ.get("ROMDATA_DRY"):
            sys.exit("%s not in the ELF: is pm3ds_rom.c linked?" % ANCHOR)
        syms[ANCHOR] = (0, 0, 0)   # dry run against an older ELF
    relocs = elf.abs32_relocs(SECTION)
    index = gen_include_assets.index_splat()

    blobs = []   # (dst, rom, size, kind, symbol)
    for sym, start, size in include_blobs(syms):
        blobs.append((syms[sym][0], start, size, KIND_RAW, sym))
    incc, unplaced = incc_blobs(syms, index)
    for sym, kind, start, size in incc:
        blobs.append((syms[sym][0], start, size, kind, sym))
    if unplaced:
        sys.exit("no ROM offset for %d compiled .inc.c files, e.g. %s" % (len(unplaced), unplaced[:5]))

    blobs.sort()
    ptrs, problems, zeroed = [], [], 0
    seen = {}
    for dst, start, size, kind, sym in blobs:
        if size == 0:
            continue
        if dst in seen:
            if seen[dst] != (start, size, kind):
                problems.append("%s: two different sources for 0x%X" % (sym, dst))
            continue
        seen[dst] = (start, size, kind)
        sec, foff = elf.file_off(dst, size, SECTION)
        if sec != SECTION:
            problems.append("%s: in %s, not %s (would be read-only at run time)" % (sym, sec, SECTION))
            continue
        have = bytes(elf_data[foff:foff + size])
        src = rom[start:start + size]
        want = vtx_native(src) if kind == KIND_VTX else swap32(src) if kind == KIND_GFX else src
        if kind == KIND_GFX and size % 8:
            problems.append("%s: display list size %d is not a multiple of 8" % (sym, size))
            continue
        keep = []
        for w in range(0, size, 4):
            if dst + w in relocs:
                if kind != KIND_GFX:
                    problems.append("%s+0x%X: relocated word in a %s object" % (sym, w, ("raw", "vtx")[kind]))
                    break
                keep.append(w)
                ptrs.append((dst + w, struct.unpack_from("<I", have, w)[0]))
                continue
            if have[w:w + 4] != want[w:w + 4]:
                problems.append("%s+0x%X (%s, rom 0x%X): 0x%s vs ROM 0x%s" % (
                    sym, w, ("raw", "vtx", "gfx")[kind], start, have[w:w + 4].hex(), want[w:w + 4].hex()))
                break
        elf_data[foff:foff + size] = bytes(size)
        for w in keep:
            elf_data[foff + w:foff + w + 4] = have[w:w + 4]
        zeroed += size - 4 * len(keep)

    # mark the copy as stripped (pm3ds_rom.c: pm3ds_romdata_state "FULL" -> "STRP")
    state = syms.get(STATE)
    sec, foff = elf.file_off(state[0], 4, SECTION) if state else (None, None)
    if sec != SECTION or bytes(elf_data[foff:foff + 4]) != struct.pack("<I", 0x46554C4C):
        problems.append("%s missing or not \"FULL\" in %s" % (STATE, SECTION))
    else:
        elf_data[foff:foff + 4] = struct.pack("<I", 0x53545250)

    if problems:
        print("\n".join(problems[:40]))
        sys.exit("%d objects can't be rebuilt from the ROM; nothing written" % len(problems))

    entries = [(dst,) + seen[dst] for dst in sorted(seen) if seen[dst][1]]
    if any(start + size > ROM_DATA_SIZE for start, size, _ in seen.values()):
        sys.exit("an object lies past the first %d MB of the ROM" % (ROM_DATA_SIZE >> 20))
    crc = zlib.crc32(rom[:ROM_DATA_SIZE]) & 0xFFFFFFFF
    # CRC32 of every object as built, in table order: the game checks its
    # restored copy against it on every boot
    data_crc = 0
    for dst, _, size, _ in entries:
        _, foff = elf.file_off(dst, size, SECTION)
        data_crc = zlib.crc32(elf.data[foff:foff + size], data_crc)
    head = struct.pack("<4sIIIIIII32s", MAGIC, VERSION, ROM_DATA_SIZE, crc, syms[ANCHOR][0],
                       len(entries), len(ptrs), data_crc & 0xFFFFFFFF, ROM_NAME)
    body = b"".join(struct.pack("<IIII", d, r, s, k) for d, r, s, k in entries)
    body += b"".join(struct.pack("<II", d, v) for d, v in sorted(ptrs))
    os.makedirs(os.path.dirname(os.path.abspath(out_table)) or ".", exist_ok=True)
    with open(out_table, "wb") as fh:
        fh.write(head + body)
    with open(out_elf, "wb") as fh:
        fh.write(elf_data)
    kinds = [sum(1 for e in entries if e[3] == k) for k in (KIND_RAW, KIND_VTX, KIND_GFX)]
    print("romdata: %d objects (%d images/palettes/fonts, %d vertex lists, %d display lists), "
          "%.2f MB zeroed, %d relocated words; ROM CRC32 %08X, data CRC32 %08X" % (
              len(entries), kinds[0], kinds[1], kinds[2], zeroed / 1048576, len(ptrs), crc,
              data_crc & 0xFFFFFFFF))


if __name__ == "__main__":
    main()
