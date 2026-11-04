#!/usr/bin/env python3
"""Draw Star Haven's icon (48x48) and HOME-menu banner (256x128), synthesize
the banner jingle, and build cia/icon.icn, cia/icon_ique.icn and cia/banner.bnr with bannertool.

Everything is original: a folded-paper star over a night sky, the title in
Fira Sans Heavy (SIL Open Font License), and a few bell tones. No game art.

With cia/banner.cgfx (Velvii's 3D banner, see NOTICE) the banner is that model
instead, with the paper star drawn into its emblem texture; the flat 256x128
picture is still drawn as the fallback.

usage: make_branding.py            (BANNERTOOL defaults to port/3ds/local/bin)
"""
import math
import os
import random
import struct
import subprocess
import wave

from PIL import Image, ImageDraw, ImageFilter, ImageFont

PORT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(PORT, "cia")
BANNERTOOL = os.environ.get("BANNERTOOL", os.path.join(PORT, "local", "bin", "bannertool"))
FONT = os.environ.get("BRAND_FONT", "/usr/share/fonts/TTF/FiraSans-Heavy.ttf")

NAME = "Star Haven"
LONG = "Star Haven: a Paper Mario port for New 3DS (bring your own ROM)"
PUBLISHER = "Star Haven team"
NAME_IQUE = "Star Haven iQue"
LONG_IQUE = "Star Haven: a Paper Mario port for 3DS, iQue (Chinese) version (bring your own ROM)"

SKY_TOP, SKY_BOTTOM = (18, 24, 72), (64, 40, 120)
STAR_LIGHT, STAR_DARK, STAR_EDGE = (255, 226, 110), (246, 168, 44), (122, 62, 16)
TITLE, TITLE_EDGE = (255, 246, 224), (34, 22, 74)


def sky(w, h, seed, scale):
    """night gradient with a sprinkle of stars; scale = drawing pixels per output pixel"""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(SKY_TOP[i] + (SKY_BOTTOM[i] - SKY_TOP[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    d = ImageDraw.Draw(img)
    rnd = random.Random(seed)
    for _ in range(max(6, int(w * h / (scale * scale) / 520))):
        x, y = rnd.uniform(0, w), rnd.uniform(0, h)
        r = rnd.choice((0.35, 0.45, 0.55, 0.8)) * scale
        a = rnd.randint(110, 240)
        d.ellipse((x - r, y - r, x + r, y + r), fill=(a, a, min(255, a + 20)))
    return img


def sparkle(d, cx, cy, r, color):
    """four-point twinkle"""
    k = r * 0.22
    d.polygon([(cx, cy - r), (cx + k, cy - k), (cx + r, cy), (cx + k, cy + k),
               (cx, cy + r), (cx - k, cy + k), (cx - r, cy), (cx - k, cy - k)], fill=color)


def paper_star(size, tilt=-12.0):
    """a five-pointed star folded along its arms: each arm a light and a dark
    half, a dark edge and a soft drop shadow (RGBA, size x size)"""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    c = size / 2
    R, r = size * 0.44, size * 0.44 * 0.45
    pts = []
    for i in range(10):
        a = math.radians(tilt - 90 + i * 36)
        rr = R if i % 2 == 0 else r
        pts.append((c + rr * math.cos(a), c + rr * math.sin(a)))
    shadow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    off = size * 0.035
    ImageDraw.Draw(shadow).polygon([(x + off, y + off * 1.4) for x, y in pts], fill=(8, 6, 30, 150))
    shadow = shadow.filter(ImageFilter.GaussianBlur(size * 0.02))
    img.alpha_composite(shadow)
    d = ImageDraw.Draw(img)
    for i in range(0, 10, 2):           # each arm: tip i, inner corners i-1 and i+1
        tip, left, right = pts[i], pts[i - 1], pts[(i + 1) % 10]
        d.polygon([(c, c), left, tip], fill=STAR_LIGHT)
        d.polygon([(c, c), tip, right], fill=STAR_DARK)
    w = max(2, int(size * 0.03))
    d.line(pts + [pts[0]], fill=STAR_EDGE, width=w, joint="curve")
    for i in range(0, 10, 2):           # the folds
        d.line([(c, c), pts[i]], fill=STAR_EDGE + (110,), width=max(1, w // 2))
    return img


def outlined_text(img, xy, text, font, fill, edge, stroke):
    d = ImageDraw.Draw(img)
    x, y = xy
    shadow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((x + stroke * 0.6, y + stroke * 0.9), text, font=font, fill=(0, 0, 0, 140),
                                stroke_width=stroke, stroke_fill=(0, 0, 0, 140))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(stroke * 0.6)))
    d.text((x, y), text, font=font, fill=fill, stroke_width=stroke, stroke_fill=edge)


def make_icon(path):
    S = 8                                          # draw at 8x, then scale down
    img = sky(48 * S, 48 * S, 7, S).convert("RGBA")
    d = ImageDraw.Draw(img)
    sparkle(d, 9 * S, 10 * S, 3.2 * S, (255, 255, 240))
    sparkle(d, 40 * S, 36 * S, 2.4 * S, (230, 230, 255))
    star = paper_star(44 * S)
    img.alpha_composite(star, (2 * S, 3 * S))
    img = img.resize((48, 48), Image.LANCZOS).convert("RGB")
    img.save(path)


def make_banner(path):
    S = 4
    W, H = 256 * S, 128 * S
    img = sky(W, H, 11, S).convert("RGBA")
    d = ImageDraw.Draw(img)
    # a soft trail behind the star, from the lower left
    trail = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    td = ImageDraw.Draw(trail)
    for i in range(40):
        t = i / 39
        x = (-20 + 110 * t) * S
        y = (122 - 70 * t) * S
        r = (3 + 9 * t) * S
        td.ellipse((x - r, y - r, x + r, y + r), fill=(255, 230, 150, int(18 + 40 * t)))
    img.alpha_composite(trail.filter(ImageFilter.GaussianBlur(3 * S)))
    for cx, cy, r in ((150, 20, 3.0), (236, 104, 2.6), (196, 16, 1.8), (112, 112, 2.0)):
        sparkle(d, cx * S, cy * S, r * S, (255, 255, 235))
    img.alpha_composite(paper_star(96 * S), (4 * S, 16 * S))
    font = ImageFont.truetype(FONT, 34 * S)
    for i, word in enumerate(NAME.split()):
        outlined_text(img, (104 * S, (22 + i * 40) * S), word, font, TITLE, TITLE_EDGE, 3 * S)
    img = img.resize((256, 128), Image.LANCZOS)
    img.save(path)


def make_jingle(path, rate=32000):
    """a rising four-note bell arpeggio and a high twinkle, about 1.6 s"""
    notes = [(0.00, 783.99), (0.11, 1046.5), (0.22, 1318.5), (0.33, 1567.98), (0.55, 2093.0)]
    n = int(rate * 1.65)
    buf = [0.0] * n
    for start, f in notes:
        s0 = int(start * rate)
        for i in range(s0, n):
            t = (i - s0) / rate
            env = math.exp(-t / 0.32) * min(1.0, t / 0.004)
            v = math.sin(2 * math.pi * f * t) + 0.35 * math.sin(2 * math.pi * 2 * f * t) \
                + 0.12 * math.sin(2 * math.pi * 3.01 * f * t)
            buf[i] += 0.22 * env * v
    peak = max(abs(v) for v in buf) or 1.0
    frames = b"".join(struct.pack("<hh", int(v / peak * 0.7 * 32767), int(v / peak * 0.7 * 32767)) for v in buf)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(frames)


def make_emblem(cgfx):
    """the paper star in the 3D banner's emblem texture (its quad is 1.844:1 and stretches
    the square texture across it, so the star is drawn that much narrower), and the model
    sized to the banner area"""
    import cgfx_texture
    star = paper_star(220)
    star = star.resize((round(star.width / 1.844), star.height), Image.LANCZOS)
    img = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    img.alpha_composite(star, ((256 - star.width) // 2, 4))
    d = cgfx_texture.replace(bytearray(open(cgfx, "rb").read()), "PaperMarioLogo", img)
    # Velvii's title texture is 256x244 (240 rows of data): a console draws only part of a
    # texture that isn't a power of two tall (#30, #32)
    d = cgfx_texture.set_height(d, "Star Haven", 128)
    # the model is about 2.2 x 1.2 units around y = 1.715 (with its animation) and the banner
    # area about 26 x 13, so scale it up 7x through the root bone and centre it on y = 1.2, clear
    # of the bottom edge, where HOME Menu themes draw over the banner.
    # Each part is a front and a back layer 0.03-0.06 apart: the depth stays unscaled so they
    # don't drift apart into two visible sheets.
    scale = 7.0
    d = cgfx_texture.place_root(d, scale, 0.0, 1.2 - 1.715 * scale, -1.0, depth_scale=1.0)
    open(cgfx, "wb").write(d)


def main():
    os.makedirs(OUT, exist_ok=True)
    icon, banner, jingle = (os.path.join(OUT, f) for f in ("icon.png", "banner.png", "banner.wav"))
    make_icon(icon)
    make_banner(banner)
    make_jingle(jingle)
    subprocess.run([BANNERTOOL, "makesmdh", "-s", NAME, "-l", LONG, "-p", PUBLISHER, "-i", icon,
                    "-o", os.path.join(OUT, "icon.icn")], check=True)
    # the iQue build installs as its own title beside the EU/US one (tools/package.sh)
    subprocess.run([BANNERTOOL, "makesmdh", "-s", NAME_IQUE, "-l", LONG_IQUE, "-p", PUBLISHER, "-i", icon,
                    "-o", os.path.join(OUT, "icon_ique.icn")], check=True)
    cgfx = os.path.join(OUT, "banner.cgfx")
    if os.path.exists(cgfx):
        make_emblem(cgfx)
        art = ["-ci", cgfx]
    else:
        art = ["-i", banner]
    subprocess.run([BANNERTOOL, "makebanner", *art, "-a", jingle,
                    "-o", os.path.join(OUT, "banner.bnr")], check=True)
    print("wrote", icon, banner, jingle, "icon.icn", "banner.bnr")


if __name__ == "__main__":
    main()
