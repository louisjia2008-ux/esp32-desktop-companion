#!/usr/bin/env python3
"""Render the lock-screen clock over a wallpaper frame on the Mac.

Mirrors the firmware maths in src/ui/glass_clock.cpp (RGB565 rounding
included) so the look can be tuned without flashing:

    .venv/bin/python tools/preview_glass_clock.py frame.jpg out.png --time 20:15
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_glass_font as gen  # noqa: E402

TIME_BASELINE = 204
TIME_TRACKING = -3
DATE_X, DATE_BASELINE = 22, 46


def to565(rgb):
    r, g, b = (max(0, min(255, int(v))) for v in rgb)
    r, g, b = r >> 3, g >> 2, b >> 3
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def mix(a, b, amount):
    return a + (b - a) * amount // 255


def blend(bg, colour, alpha):
    return to565(tuple(mix(bg[i], colour[i], alpha) for i in range(3)))


def box_blur(image, radius):
    """Separable box blur with clamped edges, as the firmware's scratch blur."""
    w, h = image.size
    src = image.load()
    tmp = [[(0, 0, 0)] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            acc = [0, 0, 0]
            for k in range(-radius, radius + 1):
                c = src[min(max(x + k, 0), w - 1), y]
                for i in range(3):
                    acc[i] += c[i]
            tmp[y][x] = tuple(v // (2 * radius + 1) for v in acc)
    for y in range(h):
        for x in range(w):
            acc = [0, 0, 0]
            for k in range(-radius, radius + 1):
                c = tmp[min(max(y + k, 0), h - 1)][x]
                for i in range(3):
                    acc[i] += c[i]
            src[x, y] = tuple(v // (2 * radius + 1) for v in acc)
    return image


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("frame", type=Path, help="wallpaper JPEG (native 320x480 or 480x320)")
    parser.add_argument("output", type=Path)
    parser.add_argument("--time", default="20:15")
    parser.add_argument("--date", default="10月1日 周三")
    args = parser.parse_args()

    image = Image.open(args.frame).convert("RGB")
    if image.size == (320, 480):  # native portrait frame -> landscape as shown
        image = image.transpose(Image.ROTATE_90)
    image = image.resize((480, 320))
    px = image.load()
    for y in range(320):
        for x in range(480):
            px[x, y] = to565(px[x, y])

    latin = gen.fetch(gen.INTER_ZIP[0], gen.INTER_ZIP[2], "InterDisplay-SemiBold.ttf", gen.INTER_ZIP[1])
    cjk = gen.fetch(gen.NOTO_SC[0], gen.NOTO_SC[1], "NotoSansSC.ttf")
    clock, clock_data, _ = gen.build(latin, "0123456789:", gen.CLOCK_DIGIT_HEIGHT, glass=True)
    date, date_data, _ = gen.build(cjk, gen.DATE_CHARACTERS, gen.DATE_HEIGHT, glass=False,
                                   cjk_reference="月", variation=b"Medium")
    by_cp = {g["codepoint"]: g for g in clock}
    digit_advance = max(by_cp[ord(c)]["advance"] for c in "0123456789")

    # Date: coverage text with a soft shadow, as drawGlassText().
    date_cp = {g["codepoint"]: g for g in date}
    for shadow in (True, False):
        pen = DATE_X
        for ch in args.date:
            g = date_cp.get(ord(ch))
            if g:
                for row in range(g["h"]):
                    for col in range(g["w"]):
                        a = date_data[g["offset"] + row * g["w"] + col]
                        x = pen + g["x"] + col + (1 if shadow else 0)
                        y = DATE_BASELINE + g["y"] + row + (2 if shadow else 0)
                        if a and 0 <= x < 480 and 0 <= y < 320:
                            px[x, y] = blend(px[x, y], (0, 0, 0) if shadow else (255, 255, 255),
                                             a * 120 // 255 if shadow else a)
                pen += 16 if "0" <= ch <= "9" else g["advance"]
            else:
                pen += gen.DATE_HEIGHT // 3

    # Time: glass composite, as GlassClock::drawGlassTime().
    advances = [digit_advance if c.isdigit() else by_cp[ord(c)]["advance"] for c in args.time]
    width = sum(advances) + TIME_TRACKING * (len(args.time) - 1)
    start = (480 - width) // 2
    background = image.copy().load()
    blurred = box_blur(image.copy(), 3).load()
    pen = start
    for ch, advance in zip(args.time, advances):
        g = by_cp[ord(ch)]
        for row in range(g["h"]):
            for col in range(g["w"]):
                o = g["offset"] + (col * g["h"] + row) * 3
                a, light, offsets = clock_data[o:o + 3]
                light = light - 256 if light > 127 else light
                dx, dy = offsets >> 4, offsets & 15
                dx, dy = (v - 16 if v >= 8 else v for v in (dx, dy))
                x, y = pen + g["x"] + col, TIME_BASELINE + g["y"] + row
                if not (0 <= x < 480 and 0 <= y < 320) or (a == 0 and light == 0):
                    continue
                if a == 0:
                    px[x, y] = blend(px[x, y], (0, 0, 0), -light * 2)
                    continue
                sx = min(max(x + dx, 0), 479)
                sy = min(max(y + dy, 0), 319)
                r, gg, b = blurred[sx, sy]
                luma = (r * 77 + gg * 150 + b * 29) >> 8
                glass = [mix(c, luma, 40) for c in (r, gg, b)]
                glass = [mix(c, 255, 60) for c in glass]
                if light > 0:
                    glass = [mix(c, 255, light * 235 // 127) for c in glass]
                elif light < 0:
                    glass = [mix(c, 0, -light * 120 // 127) for c in glass]
                px[x, y] = blend(background[x, y], glass, a)
        pen += advance + TIME_TRACKING
    image.resize((960, 640), Image.NEAREST).save(args.output)
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
