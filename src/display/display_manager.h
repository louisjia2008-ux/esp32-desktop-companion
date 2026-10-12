#pragma once

#include <Arduino_GFX_Library.h>

namespace display {

// Initializes the proven QSPI panel with a landscape Canvas. The implementation
// keeps native panel geometry separate from logical UI geometry.
bool begin();

Arduino_Canvas& canvas();

// Backlight brightness in percent (0 = off). Applied through a gamma 2.2
// curve so equal steps look equally different to the eye.
void setBacklightLevel(uint8_t percent);

}  // namespace display
