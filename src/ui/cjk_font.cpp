#include "cjk_font.h"

// Only this translation unit compiles the large table. It deliberately does
// not include Arduino_GFX_Library.h, which would also parse every other CJK
// font shipped with the library.
#define U8G2_USE_LARGE_FONTS
#include <U8g2lib.h>
#include <font/u8g2_font_unifont_t_chinese4.h>

namespace uifont {

const uint8_t* cjkFont() { return u8g2_font_unifont_t_chinese4; }

}  // namespace uifont
