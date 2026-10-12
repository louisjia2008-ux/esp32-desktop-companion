# Clock, brightness and page gestures

The logical canvas remains **480 × 320 landscape**. The native panel and touch
driver remain 320 × 480; rotation is applied above the hardware drivers.

Home displays a Chinese date at the upper left and large refractive glass
digits. The small Claude status capsule sits near the bottom. Tap Home to
toggle the clock; `clock_vis` in NVS stores the choice. Swipe left or right
between Home and Mac. There are exactly two pages; Claude usage/quota is not
implemented. Tap Mac to return Home, or wait 15 seconds without interaction.

Horizontal swipes need at least 70 px and more than twice their vertical
distance. Taps travel less than 20 px and last less than 600 ms. A release is
required before a new gesture. Modal action cards keep their existing touch
and hold-to-confirm paths. A dimmed screen consumes the first touch to wake.

## Time and brightness

The `ambient` capability enables the `clock` and `display_settings` messages.
The Bridge sends time at hello and every 600 seconds; the UTC offset follows
the Mac. The device uses `settimeofday` and continues offline. After a cold
boot it waits for time synchronization before showing the date and time.

Default rules:

| Setting | Default |
| --- | --- |
| Night interval | 23:00–07:00, across midnight |
| Night idle / awake | 10% / 40% |
| Day awake / idle | 100% / 30% |
| Idle delay | 300 seconds after the latest interaction |
| Normal fade | 300 ms |
| Night/day boundary fade | 2 seconds |

Cards and touch restore the awake level. Equal night start/end disables the
night interval. Idle delay zero disables idle dimming. Without a known time,
the day rules apply. PWM uses GPIO1, 5 kHz, 10 bits, a gamma-2.2 curve and a
constant high level at 100%.

`GET/PUT /api/display-settings` operates on the exact validated settings
fields. It is loopback-only and rejects cross-origin writes. The Mac saves
`mac_bridge/.runtime/display-settings.json`; the device writes `disp_v1` only
when a setting changes. The rules continue while disconnected.

## Font and renderer

Inter Display SemiBold and Noto Sans SC are bundled in `tools/fonts/` with
their SIL Open Font License files. Regenerate with:

```sh
.venv/bin/pip install -r tools/requirements-dev.txt
.venv/bin/python tools/gen_glass_font.py
```

Clock glyphs contain three bytes per pixel, in column order: coverage, signed
lighting, and packed refraction offsets. Coverage-only date glyphs retain row
order. The renderer snapshots the current wallpaper, blurs a small internal-RAM
sample, then applies full-resolution coverage and refraction. Fixed-point
math, a small lighting table and a compact per-minute glyph plan keep
arithmetic out of the pixel loop. Full rendering was measured above the 10 ms
target, so background sampling/blur refreshes every other frame, as planned;
the full-resolution glyph coverage still composites on every current frame.
`CLOCK_PROFILE` separates sampling, blur and compositing; `METRICS clock_ms`
includes the date and complete clock render, separate from JPEG decode and LCD
flush. Actual device measurements belong in the acceptance record, not in the
font-generation assumptions.

```sh
.venv/bin/python scripts/run_touch_tests.py
.venv/bin/python scripts/run_event_tests.py
.venv/bin/python scripts/run_glass_tests.py
```

The last test renders 168 combinations using the actual generated glyph data,
with framebuffer guards and ASan/UBSan. It does not prove visible panel output.
