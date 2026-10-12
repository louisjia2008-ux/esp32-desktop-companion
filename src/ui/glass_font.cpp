#include "glass_font.h"

namespace uifont {

const GlassGlyph* findGlyph(const GlassFont& font, uint32_t codepoint) {
  for (uint8_t i = 0; i < font.count; ++i) {
    if (font.glyphs[i].codepoint == codepoint) return &font.glyphs[i];
  }
  return nullptr;
}

}  // namespace uifont
