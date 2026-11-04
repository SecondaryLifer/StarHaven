#!/usr/bin/env python3
"""Extract the binaries behind INCLUDE_IMG / INCLUDE_PAL from the baserom.

Upstream .incbin's ver/<ver>/build/<path>.png.bin etc., which its image tool
builds from the extracted PNGs; those files are byte-identical to the ROM
ranges splat split them from. So: walk ver/pal/splat.yaml, give every image
/ palette subsegment its ROM offset and size (explicit offsets, or `auto`
chained from format x width x height), match the INCLUDE_* paths used in src/,
and write each range to port/3ds/build_assets/<path>.bin.

usage: gen_include_assets.py            (writes bins + prints a summary)
"""
import os
import re
import sys

import yaml

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
# PM3DS_VERSION=pal (build_assets/) or us (build_assets_us/), as for make
VER = os.environ.get("PM3DS_VERSION", "pal")
SPLAT = os.path.join(ROOT, "ver", VER, "splat.yaml")
ROM = os.path.join(ROOT, "ver", VER, "baserom.z64")
OUT = os.path.join(ROOT, "port", "3ds", "build_assets" + ("" if VER == "pal" else "_" + VER))
# INCLUDE_RAW charset segments (pm_charset in splat.yaml)
CHARSET = {
    "pal": {
        "charset/standard.dat": (0x110CB0, 0x115FB0),
        "charset/standard/palette.dat": (0x115FB0, 0x1164B0),
        "charset/title.dat": (0x1164B0, 0x117410),
        "charset/subtitle.dat": (0x117410, 0x117F98),
        "charset/subtitle/palette.dat": (0x117F98, 0x117FA8),
        "charset/title/palette.dat": (0x117FA8, 0x117FB8),
    },
    "us": {
        "charset/standard.dat": (0x10F1B0, 0x1144B0),
        "charset/standard/palette.dat": (0x1144B0, 0x1149B0),
        "charset/title.dat": (0x1149B0, 0x115910),
        "charset/subtitle.dat": (0x115910, 0x116498),
        "charset/subtitle/palette.dat": (0x116498, 0x1164A8),
        "charset/title/palette.dat": (0x1164A8, 0x1164B8),
    },
    # JP: kana, latin, kanji and menu fonts instead of the one standard font (ver/jp/splat.yaml)
    "jp": {
        "charset/kana.dat": (0x10F330, 0x114A40),
        "charset/latin.dat": (0x114A40, 0x115610),
        "charset/kanji.dat": (0x115610, 0x118B00),
        "charset/buttons.dat": (0x118B00, 0x118F60),
        "charset/menu_kana.dat": (0x118F60, 0x11C7A0),
        "charset/menu_latin.dat": (0x11C7A0, 0x11CF38),
        "charset/standard/palette.dat": (0x11CF38, 0x11D438),
        "charset/title.dat": (0x11D438, 0x11E398),
        "charset/subtitle.dat": (0x11E398, 0x11EF20),
        "charset/subtitle/palette.dat": (0x11EF20, 0x11EF30),
        "charset/title/palette.dat": (0x11EF30, 0x11EF40),
    },
    # iQue: one large 16x16 Chinese font (0x9E0 glyphs) (ver/ique/splat.yaml)
    "ique": {
        "charset/standard.dat": (0x10E120, 0x15D120),
        "charset/standard/palette.dat": (0x15D120, 0x15D620),
        "charset/title.dat": (0x15D620, 0x15E580),
        "charset/subtitle.dat": (0x15E580, 0x15F108),
        "charset/subtitle/palette.dat": (0x15F120, 0x15F130),
        "charset/title/palette.dat": (0x15F130, 0x15F140),
    },
}

BPP = {"rgba32": 32, "rgba16": 16, "ia16": 16, "i8": 8, "ia8": 8, "ci8": 8,
       "i4": 4, "ia4": 4, "ci4": 4}
INC_RE = re.compile(r'INCLUDE_(IMG|PAL)\("([^"]+)"')


def norm(p):
    return os.path.normpath(p).replace("\\", "/")


def index_splat():
    with open(SPLAT) as fh:
        cfg = yaml.safe_load(fh)
    images = {}     # key -> (start, size, type)

    def walk_list(subs, base_dir, first_start=None):
        # resolve starts: explicit ints, or chain from the previous known end
        entries = []
        for sub in subs:
            if isinstance(sub, dict) and "subsegments" not in sub:
                # leaf in dict form: {start, type, name, size | width/height}
                start = sub.get("start")
                entries.append((start if isinstance(start, int) else None, sub.get("type"),
                                str(sub["name"]) if "name" in sub else None,
                                sub.get("width"), sub.get("height"), None, sub.get("size")))
            elif isinstance(sub, dict):
                start = sub.get("start")
                entries.append((start if isinstance(start, int) else None, sub.get("type"), None, None, None, sub, None))
            elif isinstance(sub, list) and sub:
                start = sub[0] if isinstance(sub[0], int) else None
                typ = sub[1] if len(sub) > 1 else None
                name = str(sub[2]) if len(sub) > 2 else None
                w = sub[3] if len(sub) > 3 and isinstance(sub[3], int) else None
                h = sub[4] if len(sub) > 4 and isinstance(sub[4], int) else None
                entries.append((start, typ, name, w, h, None, None))
        cur = first_start
        prev_img_type = None
        for i, (start, typ, name, w, h, d, explicit_size) in enumerate(entries):
            if start is not None:
                cur = start
            if d is not None:
                ds = d.get("start") if isinstance(d.get("start"), int) else cur
                walk_list(d.get("subsegments", []) or [], d.get("dir", base_dir), ds)
                cur = None
                continue
            # size: from dims, or up to the next explicit start
            size = None
            if explicit_size:
                size = explicit_size
            elif typ in BPP and w and h:
                size = w * h * BPP[typ] // 8
            elif typ == "palette":
                size = 512 if prev_img_type == "ci8" else 32
                # several palettes back to back up to an explicit start; only
                # when that start is the very next entry (one further on is
                # preceded by auto entries, and swallowing them shifted e.g.
                # every battle move orb after ui/move_basic.pal by 0x6E0)
                nxt = entries[i + 1][0] if i + 1 < len(entries) else None
                if cur is not None and nxt is not None and nxt - cur > size and nxt - cur <= 512 * 4:
                    size = nxt - cur
            if typ in BPP:
                prev_img_type = typ
            if cur is not None and name and (typ in BPP or typ == "palette"):
                key = norm(os.path.join(base_dir, name))
                kind = "pal" if typ == "palette" else "img"
                images.setdefault((kind, key), (cur, size, typ, prev_img_type))
            if cur is not None and name and typ in ("vtx", "gfx"):
                # assets/<ver>/<key>.<typ>.inc.c (romdata.py strips them)
                images.setdefault((typ, norm(os.path.join(base_dir, name))), (cur, None, typ, None))
            if cur is not None and size is not None:
                cur += size
            elif typ not in (None,):
                cur = None   # unknown size breaks the auto chain

    for seg in cfg["segments"]:
        if isinstance(seg, dict):
            st = seg.get("start") if isinstance(seg.get("start"), int) else None
            walk_list(seg.get("subsegments", []) or [], seg.get("dir", ""), st)
    return images


def main():
    images = index_splat()
    rom = open(ROM, "rb").read()
    refs = set()
    for dp, _, files in os.walk(os.path.join(ROOT, "src")):
        for f in files:
            if not f.endswith(".c"):
                continue
            for kind, path in INC_RE.findall(open(os.path.join(dp, f), errors="replace").read()):
                refs.add((kind, path))
    charset = CHARSET[VER]
    manifest = []   # (kind, path as in src, rom start, size): romdata.py strips these
    for path, (a, b) in charset.items():
        dst = os.path.join(OUT, "raw", path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as fh:
            fh.write(rom[a:b])
        manifest.append(("RAW", path, a, b - a))
    for path in ("charset/kana.dat", "charset/kanji.dat", "charset/latin.dat", "charset/menu_kana.dat",
                 "charset/menu_latin.dat", "charset/buttons.dat"):
        # the JP fonts the sources name in every version: a placeholder where this
        # ROM has none, never over JP's real ones (fix from Darth-Koopa's fork)
        if path in charset:
            continue
        dst = os.path.join(OUT, "raw", path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as fh:
            fh.write(bytes(16))
    ok = miss = 0
    missing = []
    for kind, path in sorted(refs):
        if kind == "IMG":
            key = ("img", norm(re.sub(r"\.png$", "", path)))
            m = re.match(r"^(.*)\.png$", path)
        else:
            stem = re.sub(r"\.pal$", "", path)
            pm = re.match(r"^(.*)\.(\d+)$", stem)
            idx = int(pm.group(2)) - 1 if pm else 0
            key = ("pal", norm(pm.group(1) if pm else stem))
        ent = images.get(key)
        if not ent or ent[1] is None:
            miss += 1
            missing.append(path)
            # placeholder: transparent zeros (8 KB covers any UI image) so the
            # symbol exists and reads stay in bounds
            dst = os.path.join(OUT, path + ".bin")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as fh:
                fh.write(bytes(8192))
            continue
        start, size, typ, assoc = ent
        if kind == "PAL":
            palsize = 512 if assoc == "ci8" else 32
            start += idx * palsize
            size = palsize if idx or size > palsize and pm else size
        data = rom[start:start + size]
        dst = os.path.join(OUT, path + ".bin")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as fh:
            fh.write(data)
        manifest.append((kind, path, start, len(data)))
        ok += 1
    print(f"INCLUDE assets: {ok} written, {miss} unresolved")
    with open(os.path.join(OUT, "UNRESOLVED.txt"), "w") as fh:
        fh.write("\n".join(missing) + "\n")
    with open(os.path.join(OUT, "manifest.tsv"), "w") as fh:
        for kind, path, start, size in manifest:
            fh.write(f"{kind}\t{path}\t0x{start:X}\t{size}\n")
    for p in missing[:10]:
        print("  unresolved:", p)


if __name__ == "__main__":
    main()
