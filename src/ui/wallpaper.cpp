#include "wallpaper.h"

#include <algorithm>
#include <cstring>
#include <math.h>

#include <esp_heap_caps.h>

#include "board_config.h"

namespace {

constexpr uint32_t kVideoFrameStaleMs = 1500;
constexpr size_t kNativePixelCount =
    static_cast<size_t>(board::kPanelWidth) * board::kPanelHeight;

uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
  return static_cast<uint16_t>(((red & 0xF8U) << 8U) |
                               ((green & 0xFCU) << 3U) | (blue >> 3U));
}

}  // namespace

void Wallpaper::begin(uint32_t seed) {
  randomState_ = seed == 0 ? 1 : seed;
  for (Particle& particle : particles_) {
    resetParticle(particle, true);
  }
  videoFrame_ = static_cast<uint16_t*>(heap_caps_malloc(
      kNativePixelCount * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  decodeFrame_ = static_cast<uint16_t*>(heap_caps_malloc(
      kNativePixelCount * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (videoFrame_ == nullptr || decodeFrame_ == nullptr) {
    if (videoFrame_ != nullptr) {
      heap_caps_free(videoFrame_);
      videoFrame_ = nullptr;
    }
    if (decodeFrame_ != nullptr) {
      heap_caps_free(decodeFrame_);
      decodeFrame_ = nullptr;
    }
    Serial.println("Video wallpaper framebuffer allocation failed");
  } else {
    Serial.printf("Video wallpaper framebuffers: %u bytes PSRAM\n",
                  static_cast<unsigned>(kNativePixelCount *
                                        sizeof(uint16_t) * 2));
  }
}

uint32_t Wallpaper::nextRandom() {
  randomState_ = randomState_ * 1664525UL + 1013904223UL;
  return randomState_;
}

void Wallpaper::resetParticle(Particle& particle, bool randomY) {
  particle.xQ8 = static_cast<int32_t>(nextRandom() % board::kDisplayWidth) << 8;
  particle.yQ8 = static_cast<int32_t>(
                         randomY ? nextRandom() % board::kDisplayHeight
                                 : board::kDisplayHeight + (nextRandom() % 30))
                     << 8;
  particle.velocityXQ8 =
      static_cast<int16_t>(static_cast<int32_t>(nextRandom() % 513) - 256);
  particle.velocityYQ8 =
      static_cast<int16_t>(-static_cast<int32_t>(640 + nextRandom() % 1920));
  particle.radius = static_cast<uint8_t>(1 + nextRandom() % 3);
  particle.hue = static_cast<uint8_t>(nextRandom() % 3);
}

bool Wallpaper::selectVideo(const char* wallpaperId) {
  if (wallpaperId == nullptr || strlen(wallpaperId) != 16 ||
      videoFrame_ == nullptr || decodeFrame_ == nullptr) {
    return false;
  }
  if (strcmp(activeWallpaperId_, wallpaperId) != 0) {
    strlcpy(activeWallpaperId_, wallpaperId, sizeof(activeWallpaperId_));
    videoFrameReady_ = false;
    videoFrameUpdatedMs_ = 0;
  }
  return true;
}

bool Wallpaper::decodeVideoFrame(const char* wallpaperId,
                                 const uint8_t* jpeg, size_t jpegLength,
                                 uint32_t nowMs) {
  if (wallpaperId == nullptr || jpeg == nullptr || jpegLength <= 4 ||
      jpegLength > 14 * 1024 || videoFrame_ == nullptr ||
      decodeFrame_ == nullptr ||
      strcmp(activeWallpaperId_, wallpaperId) != 0) {
    return false;
  }

  decodeFrameValid_ = true;
  if (!decoder_.openRAM(const_cast<uint8_t*>(jpeg),
                        static_cast<int>(jpegLength), drawJpegBlock)) {
    return false;
  }
  decoder_.setUserPointer(this);
  decoder_.setPixelType(RGB565_LITTLE_ENDIAN);
  if (decoder_.getWidth() != board::kPanelWidth ||
      decoder_.getHeight() != board::kPanelHeight) {
    decoder_.close();
    return false;
  }
  const bool decoded = decoder_.decode(0, 0, 0) != 0;
  decoder_.close();
  if (!decoded || !decodeFrameValid_) {
    return false;
  }

  std::swap(videoFrame_, decodeFrame_);
  videoFrameReady_ = true;
  videoFrameUpdatedMs_ = nowMs;
  return true;
}

bool Wallpaper::videoFresh(uint32_t nowMs) const {
  return videoFrameReady_ && videoFrame_ != nullptr &&
         nowMs - videoFrameUpdatedMs_ <= kVideoFrameStaleMs;
}

int Wallpaper::drawJpegBlock(JPEGDRAW* draw) {
  if (draw == nullptr || draw->pUser == nullptr) {
    return 0;
  }
  return static_cast<Wallpaper*>(draw->pUser)->copyJpegBlock(*draw);
}

int Wallpaper::copyJpegBlock(const JPEGDRAW& draw) {
  const int width = draw.iWidthUsed > 0 ? draw.iWidthUsed : draw.iWidth;
  if (decodeFrame_ == nullptr || draw.pPixels == nullptr || draw.x < 0 ||
      draw.y < 0 || width <= 0 || draw.iHeight <= 0 ||
      draw.x + width > board::kPanelWidth ||
      draw.y + draw.iHeight > board::kPanelHeight ||
      draw.iWidth < width) {
    decodeFrameValid_ = false;
    return 0;
  }
  for (int row = 0; row < draw.iHeight; ++row) {
    memcpy(decodeFrame_ +
               static_cast<size_t>(draw.y + row) * board::kPanelWidth + draw.x,
           draw.pPixels + static_cast<size_t>(row) * draw.iWidth,
           static_cast<size_t>(width) * sizeof(uint16_t));
  }
  return 1;
}

void Wallpaper::render(Arduino_Canvas& gfx, uint32_t nowMs) {
  if (videoFresh(nowMs)) {
    memcpy(gfx.getFramebuffer(), videoFrame_,
           kNativePixelCount * sizeof(uint16_t));
    return;
  }
  renderProcedural(gfx, nowMs);
}

void Wallpaper::renderProcedural(Arduino_Canvas& gfx, uint32_t nowMs) {
  uint32_t deltaMs = lastUpdateMs_ == 0 ? 40 : nowMs - lastUpdateMs_;
  if (deltaMs > 80) {
    deltaMs = 80;
  }
  lastUpdateMs_ = nowMs;

  // Fill native framebuffer rows directly. With Canvas rotation 1, native
  // rows become landscape x bands. This keeps the ambient gradient while
  // avoiding a 153,600-pixel software coordinate transform every frame.
  static constexpr uint16_t kBackgroundBands[] = {
      0x0003, 0x0024, 0x0045, 0x0066, 0x0086, 0x00A6};
  constexpr size_t kBandCount =
      sizeof(kBackgroundBands) / sizeof(kBackgroundBands[0]);
  constexpr int16_t kBandWidth = board::kDisplayWidth / kBandCount;
  uint16_t* frameBuffer = gfx.getFramebuffer();
  for (size_t index = 0;
       index < kBandCount; ++index) {
    const int16_t startX = static_cast<int16_t>(index * kBandWidth);
    const int16_t width =
        index + 1 == kBandCount ? board::kDisplayWidth - startX : kBandWidth;
    std::fill_n(frameBuffer + static_cast<size_t>(startX) * board::kPanelWidth,
                static_cast<size_t>(width) * board::kPanelWidth,
                kBackgroundBands[index]);
  }

  // Four slow sine ribbons create motion without any downloaded assets.
  static constexpr uint16_t kRibbonColors[] = {0x1370, 0x21D1, 0x3193,
                                                0x2990};
  for (uint8_t ribbon = 0; ribbon < 4; ++ribbon) {
    const int16_t baseY = static_cast<int16_t>(
        (static_cast<int32_t>(ribbon + 1) * board::kDisplayHeight) / 5);
    int16_t previousX = -8;
    const float phase = static_cast<float>(nowMs) *
                            (0.00032F + ribbon * 0.000055F) +
                        ribbon * 1.37F;
    int16_t previousY = static_cast<int16_t>(
        baseY + sinf(phase - 0.3F) * (13 + ribbon * 2));
    for (int16_t x = 0; x <= board::kDisplayWidth + 8; x += 8) {
      const float wave = sinf(x * (0.020F + ribbon * 0.0015F) + phase);
      const int16_t y =
          static_cast<int16_t>(baseY + wave * (13 + ribbon * 2));
      gfx.drawLine(previousX, previousY, x, y, kRibbonColors[ribbon]);
      previousX = x;
      previousY = y;
    }
  }

  static constexpr uint16_t kParticleColors[] = {
      0x4D7F, 0x7CFF, 0xB5FF};
  for (Particle& particle : particles_) {
    particle.xQ8 +=
        static_cast<int32_t>(particle.velocityXQ8) * deltaMs / 1000;
    particle.yQ8 +=
        static_cast<int32_t>(particle.velocityYQ8) * deltaMs / 1000;

    const int16_t x = static_cast<int16_t>(particle.xQ8 >> 8);
    const int16_t y = static_cast<int16_t>(particle.yQ8 >> 8);
    if (y < -8 || x < -12 || x > board::kDisplayWidth + 12) {
      resetParticle(particle, false);
      continue;
    }

    const uint16_t color = kParticleColors[particle.hue];
    gfx.fillCircle(x, y, particle.radius, color);
    if (particle.radius >= 2) {
      gfx.drawPixel(x + particle.radius + 1, y, rgb565(42, 73, 120));
    }
  }
}
