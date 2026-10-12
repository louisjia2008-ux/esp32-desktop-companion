// Host-test stand-in for the generated glyph tables (src/ui/glass_font_data.cpp):
// empty fonts keep the clock code compiling quickly without the 3 MB table.
#include "ui/glass_font.h"

namespace uifont {

const GlassFont kClockFont = {nullptr, 0, nullptr, 3, 104};
const GlassFont kMediumFont = {nullptr, 0, nullptr, 1, 40};
const GlassFont kDateFont = {nullptr, 0, nullptr, 1, 24};

}  // namespace uifont
