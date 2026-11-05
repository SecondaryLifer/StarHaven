#!/usr/bin/env python3
"""Write the game copies' data for the combined program's romfs.

tools/universal.py links every copy's .data and .bss at one address
(__pm3ds_copy_area, universal/universal.ld) in sections the 3DS doesn't load.
This dumps each copy's .copy_<v>_data from the (romdata-stripped) ELF into
<romfs>/copy_<v>.bin, which universal/starter.c loads before calling the copy:

    "SHCP", u32 the area's link address, u32 data size, u32 zlib size, zlib data

(all little-endian). The .bss part isn't stored: the area starts zeroed.

usage: copy_images.py <elf> <romfs dir>
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ["PM3DS_VERSION"] = "pal"   # romdata.py reads it on import; only its ELF reader is used
from romdata import Elf  # noqa: E402

COPIES = ("pal", "us", "iq")


def main():
    elf_path, romfs = sys.argv[1:3]
    elf = Elf(open(elf_path, "rb").read())
    syms = elf.symbols()
    area = syms["__pm3ds_copy_area"][0]
    area_size = syms["__pm3ds_copy_area_end"][0] - area
    for v in COPIES:
        sec = elf.by_name[".copy_%s_data" % v]
        if sec["addr"] != area:
            sys.exit(".copy_%s_data is at 0x%X, not the copy area 0x%X" % (v, sec["addr"], area))
        bss = elf.by_name[".copy_%s_bss" % v]
        if bss["addr"] + bss["size"] > area + area_size:
            sys.exit("copy %s doesn't fit its area" % v)
        data = elf.data[sec["off"]:sec["off"] + sec["size"]]
        z = zlib.compress(data, 9)
        with open(os.path.join(romfs, "copy_%s.bin" % v), "wb") as fh:
            fh.write(b"SHCP" + struct.pack("<III", area, len(data), len(z)) + z)
        print("copy_%s.bin: %.2f MB data -> %.2f MB" % (v, len(data) / 1048576, len(z) / 1048576))


if __name__ == "__main__":
    main()
