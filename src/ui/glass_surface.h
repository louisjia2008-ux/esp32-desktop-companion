#pragma once

#include <Arduino_GFX_Library.h>
#include <algorithm>
#include "board_config.h"

namespace surface {
inline void glass(Arduino_Canvas&,int,int,int,int,uint16_t,uint8_t,int);
inline uint16_t mix(uint16_t background,uint16_t tint,unsigned alpha) {
  const unsigned inverse=255-alpha;
  const unsigned r=((background>>11)*inverse+(tint>>11)*alpha+127)/255;
  const unsigned g=(((background>>5)&63)*inverse+((tint>>5)&63)*alpha+127)/255;
  const unsigned b=((background&31)*inverse+(tint&31)*alpha+127)/255;
  return uint16_t((r<<11)|(g<<5)|b);
}
inline uint32_t pack565(uint16_t color) {
  return (uint32_t(color)|(uint32_t(color)<<16))&0x07E0F81FU;
}
// inverse is 0..32; weightedTint is pack565(tint)*(32-inverse).
// Each lane fits even after rounding: R/B <=1008, G <=2032. The
// nonnegative weighted sum neither overflows uint32_t nor carries into
// another lane. Preparing weightedTint leaves one multiply per pixel.
inline uint16_t mixPacked565(uint16_t background,uint32_t weightedTint,unsigned inverse) {
  const uint32_t packed=((pack565(background)*inverse+weightedTint+0x02008010U)>>5)&0x07E0F81FU;
  return uint16_t(packed|(packed>>16));
}

// The home dock uses the clock's approach: sample the current wallpaper,
// soften the sample, then shade a rounded glass surface with a faint rim.
// A small shared grid avoids allocating another framebuffer. Most of each
// wallpaper pixel remains visible; text switches ink on bright backgrounds.
uint16_t liquid(Arduino_Canvas& gfx,int x,int y,int width,int height);
// Tint the current wallpaper in place. Rounded corners leave the wallpaper
// untouched, and no extra full-screen buffer is needed on the ESP32.
inline void glass(Arduino_Canvas& gfx, int x, int y, int width, int height,
                  uint16_t tint = 0x0863, uint8_t alpha = 176, int radius = 12) {
  auto* frame = gfx.getFramebuffer();
  if (!frame || board::kDisplayRotation != 1) {
    gfx.fillRect(x, y, width, height, tint);
    return;
  }
  radius = std::max(0, std::min(radius, std::min(width, height) / 2));
  const int right = std::min<int>(x + width, board::kDisplayWidth);
  const int bottom = std::min<int>(y + height, board::kDisplayHeight);
  const unsigned inverse = 255 - alpha;
  for (int px = std::max(x, 0); px < right; ++px) {
    const int cornerX = px < x + radius ? x + radius - 1 - px :
                        px >= x + width - radius ? px - (x + width - radius) : 0;
    for (int py = std::max(y, 0); py < bottom; ++py) {
      const int cornerY = py < y + radius ? y + radius - 1 - py :
                          py >= y + height - radius ? py - (y + height - radius) : 0;
      if (cornerX && cornerY && cornerX * cornerX + cornerY * cornerY >= radius * radius) continue;
      uint16_t& pixel = frame[px * board::kPanelWidth + board::kPanelWidth - 1 - py];
      const unsigned r = (((pixel >> 11) * inverse + (tint >> 11) * alpha + 127) / 255);
      const unsigned g = ((((pixel >> 5) & 63) * inverse + ((tint >> 5) & 63) * alpha + 127) / 255);
      const unsigned b = (((pixel & 31) * inverse + (tint & 31) * alpha + 127) / 255);
      pixel = uint16_t((r << 11) | (g << 5) | b);
    }
  }
}
}  // namespace surface
