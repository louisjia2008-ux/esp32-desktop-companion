#!/usr/bin/env python3
"""Generate the anti-aliased "Liquid Glass" glyph tables for the firmware.

Writes src/ui/glass_font_data.cpp. Three fonts are produced:

* clock  - digits and ':' for the lock-screen time. Three bytes per pixel,
           stored column by column (the panel's native memory order):
           coverage, a signed light term (+ rim highlight, - inner shadow;
           outside the glyph a negative drop-shadow strength) and the
           refraction offset (dx in the high nibble, dy in the low nibble,
           both signed). Everything the device would need a distance field
           for is precomputed here, so the per-frame work is one background
           sample, a tint and a blend.
* medium - digits and '%' for the usage gauges, coverage only.
* date   - digits and the CJK characters of "10月1日 周三", coverage only.

Fonts and their SIL OFL 1.1 license texts are bundled in tools/fonts.
Missing fonts are downloaded to .cache; both sources are SHA-256 verified.

    .venv/bin/pip install -r tools/requirements-dev.txt
    .venv/bin/python tools/gen_glass_font.py
"""

from __future__ import annotations

import argparse
import hashlib
import io
import math
from pathlib import Path
import urllib.request
import zipfile

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / "tools" / "fonts" / ".cache"
OUTPUT = ROOT / "src" / "ui" / "glass_font_data.cpp"

INTER_ZIP = ("https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip",
             "extras/ttf/InterDisplay-SemiBold.ttf",
             "0310d7a325896129730c6c8cf9a6e0f81ee258bedf77b1ff059b2a7b75f74e02")
NOTO_SC = ("https://github.com/google/fonts/raw/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf",
           "a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da")

SUPERSAMPLE = 4
CLOCK_DIGIT_HEIGHT = 104      # height of '0' in pixels
CLOCK_PADDING = 8             # room for the drop shadow around each glyph
MEDIUM_DIGIT_HEIGHT = 40
DATE_HEIGHT = 24              # height of CJK characters
DATE_CHARACTERS = "0123456789月日周一二三四五六"
REFRACTION_PX = 4.0
LIGHT_DIRECTION = (-0.62, -0.78)  # from the top-left, unit vector
SHADOW_OFFSET = (1, 4)        # mostly downwards, like a soft light from above
SHADOW_RADIUS = 4.5           # barely exceeds the offset: no halo on the lit side
SHADOW_STRENGTH = 0.18


def fetch(url: str, sha256: str, name: str, member: str | None = None) -> Path:
    CACHE.mkdir(parents=True, exist_ok=True)
    bundled = ROOT / "tools" / "fonts" / name
    target = bundled if bundled.exists() else CACHE / name
    if not target.exists():
        print(f"downloading {url}")
        with urllib.request.urlopen(url, timeout=120) as response:
            payload = response.read()
        if member is not None:
            payload = zipfile.ZipFile(io.BytesIO(payload)).read(member)
        target.write_bytes(payload)
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    if digest != sha256:
        if target.parent == CACHE:
            target.unlink()
        raise SystemExit(f"{name}: unexpected SHA-256 {digest}; refusing to use it")
    return target


def render(font: ImageFont.FreeTypeFont, character: str) -> tuple[list[list[int]], int, int, int]:
    """Coverage at 1x (0..255), its top-left in the em box, and advance."""
    left, top, right, bottom = font.getbbox(character)
    width, height = right - left + 2 * SUPERSAMPLE, bottom - top + 2 * SUPERSAMPLE
    image = Image.new("L", (width, height), 0)
    ImageDraw.Draw(image).text((SUPERSAMPLE - left, SUPERSAMPLE - top), character, fill=255, font=font)
    small = image.resize((math.ceil(width / SUPERSAMPLE), math.ceil(height / SUPERSAMPLE)),
                         Image.LANCZOS)
    pixels = list(small.getdata())
    w, h = small.size
    grid = [pixels[y * w:(y + 1) * w] for y in range(h)]
    advance = round(font.getlength(character) / SUPERSAMPLE)
    return grid, (left - SUPERSAMPLE) // SUPERSAMPLE, (top - SUPERSAMPLE) // SUPERSAMPLE, advance


def distance_field(inside: list[list[bool]]) -> list[list[float]]:
    """Signed Euclidean distance (positive inside) via 8SSEDT."""
    h, w = len(inside), len(inside[0])
    far = 1e6

    def sweep(target: bool) -> list[list[float]]:
        # Distance from each pixel to the nearest pixel where inside == target.
        vx = [[0 if inside[y][x] == target else far for x in range(w)] for y in range(h)]
        vy = [row[:] for row in vx]

        def relax(x: int, y: int, dx: int, dy: int) -> None:
            nx, ny = x + dx, y + dy
            if 0 <= nx < w and 0 <= ny < h:
                cx, cy = vx[ny][nx] + abs(dx), vy[ny][nx] + abs(dy)
                if cx * cx + cy * cy < vx[y][x] ** 2 + vy[y][x] ** 2:
                    vx[y][x], vy[y][x] = cx, cy

        for y in range(h):
            for x in range(w):
                for dx, dy in ((-1, 0), (0, -1), (-1, -1), (1, -1)):
                    relax(x, y, dx, dy)
            for x in range(w - 1, -1, -1):
                relax(x, y, 1, 0)
        for y in range(h - 1, -1, -1):
            for x in range(w - 1, -1, -1):
                for dx, dy in ((1, 0), (0, 1), (-1, 1), (1, 1)):
                    relax(x, y, dx, dy)
            for x in range(w):
                relax(x, y, -1, 0)
        return [[math.hypot(vx[y][x], vy[y][x]) for x in range(w)] for y in range(h)]

    to_outside = sweep(False)
    to_inside = sweep(True)
    return [[(to_outside[y][x] - 0.5) if inside[y][x] else -(to_inside[y][x] - 0.5)
             for x in range(w)] for y in range(h)]


def pad(grid: list[list[int]], amount: int) -> list[list[int]]:
    w = len(grid[0]) + 2 * amount
    blank = [[0] * w for _ in range(amount)]
    return blank + [[0] * amount + row + [0] * amount for row in grid] + [row[:] for row in blank]


def glass_pixels(coverage: list[list[int]]) -> bytes:
    """Three bytes per pixel, column-major (see module docstring)."""
    h, w = len(coverage), len(coverage[0])
    sdf = distance_field([[value >= 128 for value in row] for row in coverage])

    def at(x: int, y: int) -> float:
        return sdf[min(max(y, 0), h - 1)][min(max(x, 0), w - 1)]

    cells: dict[tuple[int, int], bytes] = {}
    lx, ly = LIGHT_DIRECTION
    for y in range(h):
        for x in range(w):
            a = coverage[y][x]
            d = sdf[y][x]
            # Gradient points inward (towards increasing distance).
            gx = at(x + 1, y) - at(x - 1, y)
            gy = at(x, y + 1) - at(x, y - 1)
            length = math.hypot(gx, gy) or 1.0
            gx, gy = gx / length, gy / length
            light = 0.0
            dx = dy = 0
            if a > 0:
                edge = max(0.0, min(1.0, 1.0 - max(d, 0.0) / 9.0))
                # Lens: near the rim the background is sampled from further
                # out, which bends the image like thick glass.
                dx = round(-gx * REFRACTION_PX * edge * edge)
                dy = round(-gy * REFRACTION_PX * edge * edge)
                normal = (-gx, -gy)
                facing = normal[0] * lx + normal[1] * ly
                # A soft specular band a few pixels wide on the lit rim and a
                # faint inner shadow opposite it; the body stays clear.
                rim = max(0.0, min(1.0, 1.0 - max(d, 0.0) / 6.0)) ** 2
                light = max(facing, 0.0) * rim * 0.85 - max(-facing, 0.0) * rim * 0.2
            if a < 255:
                sx, sy = x - SHADOW_OFFSET[0], y - SHADOW_OFFSET[1]
                shadow_d = at(sx, sy) if 0 <= sx < w and 0 <= sy < h else -SHADOW_RADIUS
                shadow = max(0.0, min(1.0, (shadow_d + SHADOW_RADIUS) / SHADOW_RADIUS)) ** 2
                if a == 0:
                    light = -shadow * SHADOW_STRENGTH
            dx, dy = max(-7, min(7, dx)), max(-7, min(7, dy))
            cells[(x, y)] = bytes((a, round(max(-1.0, min(1.0, light)) * 127) & 0xFF,
                                   ((dx & 0x0F) << 4) | (dy & 0x0F)))
    return b"".join(cells[(x, y)] for x in range(w) for y in range(h))


def build(font_path: Path, characters: str, height: int, glass: bool, cjk_reference: str = "0",
          variation: bytes | None = None) -> tuple[list[dict], bytes, int]:
    probe = ImageFont.truetype(str(font_path), 100 * SUPERSAMPLE)
    if variation:
        probe.set_variation_by_name(variation)
    _, top, _, bottom = probe.getbbox(cjk_reference)
    size = round(100 * height / ((bottom - top) / SUPERSAMPLE)) * SUPERSAMPLE
    font = ImageFont.truetype(str(font_path), size)
    if variation:
        font.set_variation_by_name(variation)
    ascent = font.getmetrics()[0] // SUPERSAMPLE
    glyphs, data = [], bytearray()
    for character in characters:
        grid, x_offset, y_offset, advance = render(font, character)
        if glass:
            grid = pad(grid, CLOCK_PADDING)
            x_offset -= CLOCK_PADDING
            y_offset -= CLOCK_PADDING
            pixels = glass_pixels(grid)
        else:
            pixels = bytes(value for row in grid for value in row)
        glyphs.append({"codepoint": ord(character), "x": x_offset, "y": y_offset - ascent,
                       "w": len(grid[0]), "h": len(grid), "advance": advance, "offset": len(data)})
        data += pixels
    return glyphs, bytes(data), ascent


def emit(name: str, glyphs: list[dict], data: bytes, bytes_per_pixel: int, line_height: int) -> str:
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 24]) for i in range(0, len(data), 24)]
    glyph_rows = ",\n".join(
        f"    {{0x{g['codepoint']:04x}, {g['x']}, {g['y']}, {g['w']}, {g['h']}, {g['advance']}, {g['offset']}}}"
        for g in glyphs)
    return (f"const uint8_t {name}Data[{len(data)}] PROGMEM = {{\n    " + ",\n    ".join(rows) +
            f"\n}};\n\nconst GlassGlyph {name}Glyphs[{len(glyphs)}] = {{\n{glyph_rows}\n}};\n\n"
            f"const GlassFont {name} = {{{name}Glyphs, {len(glyphs)}, {name}Data, {bytes_per_pixel}, "
            f"{line_height}}};\n\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--font", type=Path, help="Latin digit font (default: Inter Display SemiBold)")
    parser.add_argument("--cjk-font", type=Path, help="CJK font for the date (default: Noto Sans SC)")
    args = parser.parse_args()
    latin = args.font or fetch(INTER_ZIP[0], INTER_ZIP[2], "InterDisplay-SemiBold.ttf", INTER_ZIP[1])
    cjk = args.cjk_font or fetch(NOTO_SC[0], NOTO_SC[1], "NotoSansSC.ttf")

    clock, clock_data, _ = build(latin, "0123456789:", CLOCK_DIGIT_HEIGHT, glass=True)
    medium, medium_data, _ = build(latin, "0123456789%", MEDIUM_DIGIT_HEIGHT, glass=False)
    date, date_data, _ = build(cjk, DATE_CHARACTERS, DATE_HEIGHT, glass=False,
                               cjk_reference="月", variation=b"Medium")
    source = ("// Generated by tools/gen_glass_font.py - do not edit.\n"
              "// Inter Display SemiBold and Noto Sans SC Medium, SIL Open Font License 1.1\n"
              "// (tools/fonts/LICENSE-*.txt).\n\n"
              "#include \"glass_font.h\"\n\nnamespace uifont {\n\n" +
              emit("kClockFont", clock, clock_data, 3, CLOCK_DIGIT_HEIGHT) +
              emit("kMediumFont", medium, medium_data, 1, MEDIUM_DIGIT_HEIGHT) +
              emit("kDateFont", date, date_data, 1, DATE_HEIGHT) +
              "}  // namespace uifont\n")
    OUTPUT.write_text(source, encoding="utf-8")
    total = len(clock_data) + len(medium_data) + len(date_data)
    print(f"wrote {OUTPUT.relative_to(ROOT)}: {total} bytes of glyph data")
    for name, glyphs in (("clock", clock), ("medium", medium), ("date", date)):
        print(f"  {name}: " + " ".join(f"{chr(g['codepoint'])}{g['w']}x{g['h']}/{g['advance']}" for g in glyphs))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
