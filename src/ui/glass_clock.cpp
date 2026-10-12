#include "glass_clock.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
// The rest of the firmware optimizes for size. This bounded pixel kernel
// needs inlining and loop optimization; -Os leaves calls inside every pixel.
#pragma GCC optimize("O3")
#endif

#include "board_config.h"
#include "text_layout.h"

namespace uifont {

namespace {

constexpr int16_t kTimeTracking = -3;
constexpr int16_t kTimeBaseline = 204;  // digits span roughly y 100..204
constexpr int16_t kDateX = 22;
constexpr int16_t kDateBaseline = 46;

// Landscape (x, y) -> native portrait framebuffer, as fillLandscapeRect.
inline uint16_t* pixelAt(uint16_t* frameBuffer, int16_t x, int16_t y) {
  return frameBuffer + static_cast<size_t>(x) * board::kPanelWidth +
         (board::kPanelWidth - 1 - y);
}

inline bool onScreen(int16_t x, int16_t y) {
  return x >= 0 && y >= 0 && x < board::kDisplayWidth && y < board::kDisplayHeight;
}

struct Rgb { int r, g, b; };

inline Rgb unpack(uint16_t c) {
  const int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}

inline uint16_t pack(const Rgb& c) {
  return static_cast<uint16_t>(((c.r & 0xF8) << 8) |
                               ((c.g & 0xFC) << 3) |
                               (c.b >> 3));
}

inline int mix(int from, int to, int amount255) {
  // Xtensa's integer division is expensive in a per-pixel loop. Map the
  // endpoints exactly to 0..256 and use fixed-point multiplication instead.
  const int amount256 = amount255 + (amount255 >> 7);
  return from + (((to - from) * amount256) >> 8);
}

inline uint16_t blend(uint16_t background, const Rgb& colour, int alpha255) {
  if (alpha255 == 255) return pack(colour);
  const Rgb bg = unpack(background);
  return pack({mix(bg.r, colour.r, alpha255), mix(bg.g, colour.g, alpha255),
               mix(bg.b, colour.b, alpha255)});
}

inline uint16_t blend565(uint16_t bg, uint16_t fg, int alpha) {
  if (alpha == 255) return fg;
  const int a = alpha + (alpha >> 7);
  const int r = (bg >> 11) + ((((fg >> 11) - (bg >> 11)) * a) >> 8);
  const int g = ((bg >> 5) & 63) + (((((fg >> 5) & 63) - ((bg >> 5) & 63)) * a) >> 8);
  const int b = (bg & 31) + ((((fg & 31) - (bg & 31)) * a) >> 8);
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

// Six-bit channel/light lookup keeps shading outside the pixel loop.
// It lives in internal RAM (4 KiB), with no PSRAM or flash reads.
uint8_t shadeTable[64][64];
void prepareShadeTable() {
  static bool ready = false;
  if (ready) return;
  for (int index = 0; index < 64; ++index) {
    const int l = index * 4 - 128;
    const int amount = l > 0 ? (l * 474) >> 8 : (-l * 242) >> 8;
    for (int c = 0; c < 64; ++c) {
      shadeTable[index][c] = static_cast<uint8_t>(mix(c, l > 0 ? 63 : 0, amount));
    }
  }
  ready = true;
}

int16_t digitAdvance(const GlassFont& font) {
  int16_t advance = 0;
  for (char digit = '0'; digit <= '9'; ++digit) {
    const GlassGlyph* glyph = findGlyph(font, static_cast<uint8_t>(digit));
    if (glyph != nullptr) advance = std::max<int16_t>(advance, glyph->advance);
  }
  return advance;
}

// Digits share one advance (tabular) so the time never jiggles as it ticks.
int16_t glyphAdvance(const GlassFont& font, const GlassGlyph* glyph, uint32_t codepoint) {
  if (glyph == nullptr) return font.height / 3;
  if (codepoint >= '0' && codepoint <= '9') return digitAdvance(font);
  return glyph->advance;
}

}  // namespace

int16_t glassTextWidth(const GlassFont& font, const char* text, int16_t tracking) {
  int16_t width = 0;
  bool first = true;
  for (size_t i = 0; text != nullptr && text[i] != '\0';) {
    uint32_t codepoint = 0;
    i += text::decode(text + i, codepoint);
    if (!first) width += tracking;
    width += glyphAdvance(font, findGlyph(font, codepoint), codepoint);
    first = false;
  }
  return width;
}

void drawGlassText(Arduino_Canvas& gfx, const GlassFont& font, const char* text,
                   int16_t x, int16_t baseline, uint16_t color,
                   uint8_t shadowAlpha, int16_t tracking) {
  uint16_t* frameBuffer = gfx.getFramebuffer();
  if (frameBuffer == nullptr || board::kDisplayRotation != 1 || font.bytesPerPixel != 1) return;
  const Rgb ink = unpack(color);
  const Rgb black{0, 0, 0};
  for (int pass = 0; pass < 2; ++pass) {  // shadow first, then the ink
    if (pass == 0 && shadowAlpha == 0) continue;
    int16_t penX = x;
    for (size_t i = 0; text[i] != '\0';) {
      uint32_t codepoint = 0;
      i += text::decode(text + i, codepoint);
      const GlassGlyph* glyph = findGlyph(font, codepoint);
      if (glyph != nullptr) {
        const uint8_t* bitmap = font.data + glyph->offset;
        const int16_t left = penX + glyph->x + (pass == 0 ? 1 : 0);
        const int16_t top = baseline + glyph->y + (pass == 0 ? 2 : 0);
        for (int16_t column = 0; column < glyph->width; ++column) {
          for (int16_t row = 0; row < glyph->height; ++row) {
            const uint8_t coverage = pgm_read_byte(bitmap + row * glyph->width + column);
            const int16_t px = left + column, py = top + row;
            if (coverage == 0 || !onScreen(px, py)) continue;
            uint16_t* pixel = pixelAt(frameBuffer, px, py);
            *pixel = pass == 0 ? blend565(*pixel, 0, (coverage * shadowAlpha + 128) >> 8)
                               : blend565(*pixel, color, coverage);
          }
        }
      }
      penX += glyphAdvance(font, glyph, codepoint) + tracking;
    }
  }
}

void GlassClock::formatDate(const struct tm& local, char* out, size_t capacity) {
  static const char* const kWeekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
  snprintf(out, capacity, "%d月%d日 周%s", local.tm_mon + 1, local.tm_mday,
           kWeekdays[(local.tm_wday % 7 + 7) % 7]);
}

void GlassClock::formatTime(const struct tm& local, char* out, size_t capacity) {
  snprintf(out, capacity, "%02d:%02d", local.tm_hour % 24, local.tm_min % 60);
}

bool GlassClock::ensureScratch(size_t pixels) {
  if (scratchPixels_ >= pixels) return true;
#if defined(ARDUINO_ARCH_ESP32)
  uint16_t* replacement = static_cast<uint16_t*>(heap_caps_malloc(
      pixels * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#else
  uint16_t* replacement = static_cast<uint16_t*>(malloc(pixels * sizeof(uint16_t)));
#endif
  if (replacement == nullptr) return false;
  free(scratch_);
  scratch_ = replacement;
  scratchPixels_ = pixels;
  return true;
}

namespace {

// One pass of a box blur along rows (step 1) or columns (step = width),
// with clamped edges and running channel sums: O(pixels) for any radius.
void boxBlurPass(const uint16_t* source, uint16_t* destination, int16_t lines,
                 int16_t length, size_t lineStride, size_t step) {
  constexpr int16_t radius = 1;
  for (int16_t line = 0; line < lines; ++line) {
    const uint16_t* in = source + line * lineStride;
    uint16_t* out = destination + line * lineStride;
    int32_t r = 0, g = 0, b = 0;
    auto add = [&](int16_t index, int sign) {
      const uint16_t c = in[std::min<int16_t>(std::max<int16_t>(index, 0), length - 1) * step];
      r += sign * (c >> 11); g += sign * ((c >> 5) & 63); b += sign * (c & 31);
    };
    for (int16_t k = -radius; k <= radius; ++k) add(k, 1);
    for (int16_t i = 0; i < length; ++i) {
      // Exact unsigned division by three for these small channel sums.
      out[i * step] = static_cast<uint16_t>((((r * 21846) >> 16) << 11) |
                                            (((g * 21846) >> 16) << 5) |
                                            ((b * 21846) >> 16));
      add(i + radius + 1, 1);
      add(i - radius, -1);
    }
  }
}

}  // namespace

void GlassClock::render(Arduino_Canvas& gfx, const struct tm& local) {
  uint16_t* frameBuffer = gfx.getFramebuffer();
  if (frameBuffer == nullptr || board::kDisplayRotation != 1) return;
  char date[48];
  char time[8];
  formatDate(local, date, sizeof(date));
  formatTime(local, time, sizeof(time));
  drawGlassText(gfx, kDateFont, date, kDateX, kDateBaseline, 0xFFFF, 120);
  drawGlassTime(frameBuffer, time);
}

bool GlassClock::prepareLayout(const char* text) {
  if (commands_ && strcmp(layoutText_, text) == 0) return true;
  const GlassFont& font = kClockFont;
  if (font.bytesPerPixel != 3) return false;
  const int startX = (board::kDisplayWidth - glassTextWidth(font, text, kTimeTracking)) / 2;
  int left = board::kDisplayWidth, top = board::kDisplayHeight, right = 0, bottom = 0;
  int penX = startX;
  size_t maxCommands = 0, maxColumns = 0;
  for (size_t i = 0; text[i]; ++i) {
    const GlassGlyph* glyph = findGlyph(font, text[i]);
    if (!glyph) return false;
    left = std::min(left, penX + glyph->x);
    top = std::min(top, kTimeBaseline + glyph->y);
    right = std::max(right, penX + glyph->x + glyph->width);
    bottom = std::max(bottom, kTimeBaseline + glyph->y + glyph->height);
    maxCommands += static_cast<size_t>(glyph->width) * glyph->height;
    maxColumns += glyph->width;
    penX += glyphAdvance(font, glyph, text[i]) + kTimeTracking;
  }
  left = std::max(left, 0); top = std::max(top, 0);
  right = std::min<int>(right, board::kDisplayWidth);
  bottom = std::min<int>(bottom, board::kDisplayHeight);
  if (right <= left || bottom <= top) return false;
  if (maxCommands > commandCapacity_) {
#if defined(ARDUINO_ARCH_ESP32)
    auto* replacement = static_cast<uint32_t*>(heap_caps_malloc(
        maxCommands * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    auto* replacement = static_cast<uint32_t*>(malloc(maxCommands * sizeof(uint32_t)));
#endif
    if (!replacement) return false;
    free(commands_); commands_ = replacement; commandCapacity_ = maxCommands;
  }
  if (maxColumns > columnCapacity_) {
#if defined(ARDUINO_ARCH_ESP32)
    auto* replacement = static_cast<ColumnPlan*>(heap_caps_malloc(
        maxColumns * sizeof(ColumnPlan), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#else
    auto* replacement = static_cast<ColumnPlan*>(malloc(maxColumns * sizeof(ColumnPlan)));
#endif
    if (!replacement) { layoutText_[0] = 0; return false; }
    free(columns_); columns_ = replacement; columnCapacity_ = maxColumns;
  }
  const int regionWidth = right - left, regionHeight = bottom - top;
  left_ = left; bottom_ = bottom;
  sampleWidth_ = (regionWidth + 3) >> 2;
  sampleHeight_ = (regionHeight + 3) >> 2;
  columnCount_ = 0;
  size_t commandCount = 0;
  penX = startX;
  // Glyph geometry and refraction addresses only change once per minute.
  // Compile a sparse plan once, so each wallpaper frame only shades pixels.
  for (size_t i = 0; text[i]; ++i) {
    const GlassGlyph* glyph = findGlyph(font, text[i]);
    const uint8_t* cells = font.data + glyph->offset;
    const int glyphTop = kTimeBaseline + glyph->y;
    const int firstRow = std::max(0, top - glyphTop);
    const int endRow = std::min<int>(glyph->height, bottom - glyphTop);
    for (int col = 0; col < glyph->width; ++col) {
      const int px = penX + glyph->x + col;
      if (px < left || px >= right || endRow <= firstRow) continue;
      const size_t first = commandCount;
      for (int row = firstRow; row < endRow; ++row) {
        const uint8_t* cell = cells + (col * glyph->height + row) * 3;
        const uint8_t coverage = pgm_read_byte(cell);
        const int8_t light = static_cast<int8_t>(pgm_read_byte(cell + 1));
        if (!coverage && !light) continue;
        uint32_t sample = 8191;  // thirteen-bit sentinel for an outer shadow
        const uint32_t alpha = std::min<uint32_t>(31, ((coverage ? coverage : -light * 2) + 4) >> 3);
        if (!alpha) continue;
        if (coverage) {
          const uint8_t offsets = pgm_read_byte(cell + 2);
          const int dx = (offsets >> 4) >= 8 ? (offsets >> 4) - 16 : offsets >> 4;
          const int dy = (offsets & 15) >= 8 ? (offsets & 15) - 16 : offsets & 15;
          const int x = std::min(std::max(px + dx - left, 0), regionWidth - 1);
          const int y = std::min(std::max(glyphTop + row + dy - top, 0), regionHeight - 1);
          sample = static_cast<uint32_t>((x >> 2) * sampleHeight_ + ((regionHeight - 1 - y) >> 2));
        }
        const uint32_t lighting = std::min<uint32_t>(63, (static_cast<int>(light) + 130) >> 2);
        commands_[commandCount++] = sample | (static_cast<uint32_t>(endRow - 1 - row) << 13) |
            (lighting << 21) | (alpha << 27);
      }
      if (commandCount != first) {
        columns_[columnCount_++] = {static_cast<uint32_t>(first),
          static_cast<uint16_t>(commandCount - first), static_cast<uint16_t>(px),
          static_cast<uint16_t>(glyphTop + endRow - 1), static_cast<uint16_t>(endRow - firstRow)};
      }
    }
    penX += glyphAdvance(font, glyph, text[i]) + kTimeTracking;
  }
#if defined(ARDUINO_ARCH_ESP32)
  // Compact the sparse four-byte commands into internal RAM when at least
  // 64 KiB remains for networking/OTA. External RAM is a bounded fallback.
  const size_t bytes = commandCount * sizeof(uint32_t);
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= bytes + 65536) {
    auto* compact = static_cast<uint32_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (compact) {
      memcpy(compact, commands_, bytes);
      free(commands_); commands_ = compact; commandCapacity_ = commandCount;
    }
  }
#endif
  strlcpy(layoutText_, text, sizeof(layoutText_));
  refreshBackground_ = true;
  return true;
}

__attribute__((noinline)) void GlassClock::composite(uint16_t* frameBuffer, const uint16_t* frosted) {
  uint16_t pixels[256];
  for (size_t col = 0; col < columnCount_; ++col) {
    const ColumnPlan& column = columns_[col];
    uint16_t* destination = pixelAt(frameBuffer, column.x, column.bottom);
    memcpy(pixels, destination, column.pixels * sizeof(uint16_t));
    const uint32_t* commands = commands_ + column.first;
    for (int i = 0; i < column.count; ++i) {
      const uint32_t command = commands[i];
      const uint32_t sample = command & 8191;
      const uint32_t row = (command >> 13) & 255;
      const uint32_t alpha = command >> 27;
      uint16_t foreground = 0;
      if (sample != 8191) {
        const uint16_t glass = frosted[sample];
        const uint8_t* shade = shadeTable[(command >> 21) & 63];
        const int r = glass >> 11, b = glass & 31;
        foreground = static_cast<uint16_t>((shade[(r << 1) | (r >> 4)] >> 1) << 11 |
            shade[(glass >> 5) & 63] << 5 | (shade[(b << 1) | (b >> 4)] >> 1));
      }
      pixels[row] = blend565(pixels[row], foreground, (alpha << 3) | (alpha >> 2));
    }
    memcpy(destination, pixels, column.pixels * sizeof(uint16_t));
  }
}

void GlassClock::drawGlassTime(uint16_t* frameBuffer, const char* text) {
  const uint32_t startedUs = micros();
  if (!prepareLayout(text)) return;
  const size_t plane = static_cast<size_t>(sampleWidth_) * sampleHeight_;
  if (!ensureScratch(2 * plane)) return;
  uint16_t* intermediate = scratch_;
  uint16_t* frosted = intermediate + plane;
  // Measured full rendering exceeds the 10 ms target, so use the planned
  // alternate-frame background refresh. Coverage still composites every
  // frame over the current wallpaper; a minute change refreshes immediately.
  if (refreshBackground_) {
    for (int x = 0; x < sampleWidth_; ++x) {
      for (int y = 0; y < sampleHeight_; ++y) {
        intermediate[x * sampleHeight_ + y] = *pixelAt(frameBuffer, left_ + (x << 2), bottom_ - 1 - (y << 2));
      }
    }
  }
  const uint32_t copiedUs = micros();
  if (refreshBackground_) {
  boxBlurPass(intermediate, frosted, sampleWidth_, sampleHeight_, sampleHeight_, 1);
  boxBlurPass(frosted, intermediate, sampleHeight_, sampleWidth_, 1, sampleHeight_);
  prepareShadeTable();
  for (size_t p = 0; p < plane; ++p) {
    Rgb glass = unpack(intermediate[p]);
    const int luma = (glass.r * 77 + glass.g * 150 + glass.b * 29) >> 8;
    glass = {mix(glass.r, luma, 40), mix(glass.g, luma, 40), mix(glass.b, luma, 40)};
    intermediate[p] = pack({mix(glass.r, 255, 60), mix(glass.g, 255, 60), mix(glass.b, 255, 60)});
  }
  }
  refreshBackground_ = !refreshBackground_;
  const uint32_t blurredUs = micros();
  composite(frameBuffer, intermediate);
  static uint32_t lastProfileMs = 0;
  if (millis() - lastProfileMs >= 10000) {
    Serial.printf("CLOCK_PROFILE copy_us=%lu blur_us=%lu glyph_us=%lu\n",
                  (unsigned long)(copiedUs-startedUs), (unsigned long)(blurredUs-copiedUs),
                  (unsigned long)(micros()-blurredUs));
    lastProfileMs = millis();
  }
}

}  // namespace uifont
