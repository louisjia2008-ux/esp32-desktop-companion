#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include <ctime>

#include "glass_font.h"

namespace uifont {

// Width of UTF-8 text in a glass font; characters without a glyph count as
// a third of the font height (used for spaces).
int16_t glassTextWidth(const GlassFont& font, const char* text, int16_t tracking = 0);

// Coverage-only text (medium/date fonts) blended over the framebuffer with
// a soft dark shadow one pixel down-right. `baseline` is the text baseline.
void drawGlassText(Arduino_Canvas& gfx, const GlassFont& font, const char* text,
                   int16_t x, int16_t baseline, uint16_t color,
                   uint8_t shadowAlpha = 110, int16_t tracking = 0);

// iOS lock-screen style clock: the date top-left and a large "Liquid Glass"
// time in the centre, refracting whatever wallpaper frame is underneath.
class GlassClock {
 public:
  ~GlassClock() { free(scratch_); free(commands_); free(columns_); }
  // Formats "10月1日 周三" and "20:15" into the buffers (exposed for tests).
  static void formatDate(const struct tm& local, char* out, size_t capacity);
  static void formatTime(const struct tm& local, char* out, size_t capacity);

  void render(Arduino_Canvas& gfx, const struct tm& local);

 private:
  void drawGlassTime(uint16_t* frameBuffer, const char* text);
  bool ensureScratch(size_t pixels);
  bool prepareLayout(const char* text);
  void composite(uint16_t* frameBuffer, const uint16_t* frosted);

  struct ColumnPlan {
    uint32_t first;
    uint16_t count;
    uint16_t x;
    uint16_t bottom;
    uint16_t pixels;
  };
  uint32_t* commands_ = nullptr;
  ColumnPlan* columns_ = nullptr;
  size_t commandCapacity_ = 0;
  size_t columnCapacity_ = 0;
  size_t columnCount_ = 0;
  char layoutText_[8]{};
  int left_ = 0, bottom_ = 0, sampleWidth_ = 0, sampleHeight_ = 0;
  bool refreshBackground_ = true;

  uint16_t* scratch_ = nullptr;
  size_t scratchPixels_ = 0;
};

}  // namespace uifont
