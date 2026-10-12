#pragma once

// Arduino_GFX enables its own, self-contained u8g2 font renderer only when
// <U8g2lib.h> can be included. This shim supplies the two macros that
// renderer and the font tables need, instead of pulling in the whole U8g2
// library. Large font tables stay disabled (no U8G2_USE_LARGE_FONTS here);
// the one CJK table the UI uses is compiled once in src/ui/cjk_font.cpp.
#ifndef U8G2_WITH_UNICODE
#define U8G2_WITH_UNICODE
#endif
#ifndef U8G2_FONT_SECTION
#define U8G2_FONT_SECTION(name)
#endif
