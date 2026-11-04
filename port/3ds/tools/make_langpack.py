#!/usr/bin/env python3
"""Build a Star Haven language pack (GitHub #84): a translation's message text,
loaded from the SD card instead of the ROM's (n64_shim/langpack.c).

    make_langpack.py SRC OUT_DIR

SRC is a folder with pack.ini and the translated .msg files, in the format of
the decompilation's assets/us/msg (one file per section, as 00_Intro.msg ..
2E_Credits.msg). A pack may leave files out: the game shows the ROM's text for
any message the pack lacks. Instead of .msg files, SRC may also be the .bin
that tools/build/msg/combine.py writes (the ROM's message table).

pack.ini:

    name = Català          shown in Settings > Game text (UTF-8, up to 31 bytes)
    plural = es            optional: the plural ending some messages add ("2 coins")
    singular = a           optional: the ending for one (default: none)

Copy OUT_DIR to the SD card as /3ds/StarHaven/lang/<id>/ (<id> a short name
without spaces, e.g. ca). It holds one file, messages.bin:

    0   "SHLP"
    4   u16 version (1), u16 base (0: US message ids)
    8   u32 message count, u32 text size
    16  char name[32] (UTF-8, NUL-terminated)
    48  u8 plural[8], u8 singular[8] (game font codes ending in 0xFD; [0] == 0: not set)
    64  count x { u32 id (section << 16 | index), u32 offset, u32 length }, ids ascending
    ..  the text (each message ends with [End], 0xFD)
All numbers little-endian. A message longer than 1024 bytes is refused (the
game's message buffers are that size); the credits' lines are cut at 256.

Needs Python 3 and msgpack (the decompilation's requirements).
"""
import argparse
import configparser
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

PORT = Path(__file__).resolve().parent.parent
REPO = PORT.parent.parent
PARSE = REPO / "tools" / "build" / "msg" / "parse_compile.py"
MSG_END = 0xFD
MAX_LEN = 0x400
CREDITS_SECTION, CREDITS_LEN = 0x2E, 256
VERSION = 1


def compile_msg(path, tmp):
    import msgpack
    out = Path(tmp) / (path.stem + ".msgpack")
    r = subprocess.run([sys.executable, str(PARSE), "us", str(path), str(out)], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{path.name}: the message compiler failed:\n{r.stdout}{r.stderr}")
    if r.stdout.strip():
        print(r.stdout.strip())
    with open(out, "rb") as f:
        return list(msgpack.unpack(f))


def from_msg_files(files):
    """(section, index) -> bytes, indices given as combine.py gives them"""
    msgs, sections = {}, {}
    with tempfile.TemporaryDirectory() as tmp:
        for path in files:
            for m in compile_msg(path, tmp):
                sec = m.get("section")
                if sec is None:
                    sys.exit(f"{path.name}: message {m.get('name')} has no section (#message:XX:...)")
                taken = sections.setdefault(sec, set())
                idx = m.get("index")
                if idx is None:
                    idx = len(taken)
                if (sec, idx) in msgs:
                    print(f"warning: {path.name}: message {sec:02X}:{idx:03X} given twice, the last one counts")
                taken.add(idx)
                msgs[(sec, idx)] = bytes(m["bytes"])
    return msgs


def from_table(path):
    """the ROM's table format (combine.py): section offsets, then per section its message offsets"""
    d = path.read_bytes()
    be = lambda o: struct.unpack_from(">I", d, o)[0]
    msgs, secs = {}, []
    while be(len(secs) * 4):
        secs.append(be(len(secs) * 4))
    for sec, so in enumerate(secs):
        offs = []
        while True:
            v = be(so + len(offs) * 4)
            offs.append(v)
            if v == so:
                break
        for idx in range(len(offs) - 1):
            msgs[(sec, idx)] = d[offs[idx]:offs[idx + 1]]
    return msgs


def ending(text, tmp):
    """a plural or singular ending in game font codes"""
    if not text:
        return b"\0" * 8
    src = Path(tmp) / "ending.msg"
    src.write_text("#message:00:000 {\n" + text + "[End]\n}\n", encoding="utf-8")
    b = bytes(compile_msg(src, tmp)[0]["bytes"]).rstrip(b"\0")
    if len(b) > 8:
        sys.exit(f"the ending {text!r} is too long (7 letters at most)")
    return b.ljust(8, bytes([MSG_END]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", type=Path, help="folder with pack.ini and .msg files (or combine.py's .bin)")
    ap.add_argument("out", type=Path, help="folder to write messages.bin to")
    a = ap.parse_args()

    ini = configparser.ConfigParser()
    ini_path = a.src / "pack.ini"
    if not ini_path.exists():
        sys.exit(f"{ini_path} is missing (name = ...)")
    ini.read_string("[pack]\n" + ini_path.read_text(encoding="utf-8-sig"))
    p = ini["pack"]
    name = p.get("name", "").strip()
    if not name or len(name.encode()) > 31:
        sys.exit("pack.ini: name must be 1..31 bytes")

    bins = sorted(a.src.glob("*.bin"))
    files = sorted(a.src.glob("*.msg"))
    if files:
        msgs = from_msg_files(files)
    elif bins:
        msgs = from_table(bins[0])
    else:
        sys.exit(f"no .msg files (or a .bin) in {a.src}")

    entries, data, bad = [], bytearray(), 0
    for (sec, idx), b in sorted(msgs.items()):
        if MSG_END not in b:
            print(f"error: {sec:02X}:{idx:03X} has no [End]")
            bad += 1
            continue
        b = b[:b.index(MSG_END) + 1]
        limit = CREDITS_LEN if sec == CREDITS_SECTION else MAX_LEN
        if len(b) > limit:
            print(f"error: {sec:02X}:{idx:03X} is {len(b)} bytes, more than the game's {limit}")
            bad += 1
            continue
        entries.append((sec << 16 | idx, len(data), len(b)))
        data += b
    if bad:
        sys.exit(f"{bad} messages refused")

    with tempfile.TemporaryDirectory() as tmp:
        plural = ending(p.get("plural", "").strip(), tmp)
        singular = ending(p.get("singular", "").strip(), tmp)
    flags = (1 if plural[0] else 0) | (2 if singular[0] else 0)
    if p.get("singular", "").strip() and not p.get("plural", "").strip():
        sys.exit("pack.ini: a singular ending needs a plural one too")

    out = bytearray(b"SHLP")
    out += struct.pack("<HHII", VERSION, 0, len(entries), len(data))
    out += name.encode().ljust(32, b"\0")
    out += plural if flags & 1 else b"\0" * 8
    out += singular if flags & 2 else b"\0" * 8
    for e in entries:
        out += struct.pack("<III", *e)
    out += data
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / "messages.bin").write_bytes(out)
    secs = sorted({sec for sec, _ in msgs})
    print(f"{a.out / 'messages.bin'}: {name}, {len(entries)} messages in {len(secs)} sections, {len(out)} bytes")


if __name__ == "__main__":
    main()
