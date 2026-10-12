#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>

#include <array>

class Wallpaper {
 public:
  void begin(uint32_t seed);
  void render(Arduino_Canvas& gfx, uint32_t nowMs);
  bool selectVideo(const char* wallpaperId);
  void clearVideo() {
    activeWallpaperId_[0] = '\0';
    videoFrameReady_ = false;
    videoFrameUpdatedMs_ = 0;
  }
  bool decodeVideoFrame(const char* wallpaperId, const uint8_t* jpeg,
                        size_t jpegLength, uint32_t nowMs);
  bool videoFresh(uint32_t nowMs) const;
  const char* activeWallpaperId() const { return activeWallpaperId_; }

 private:
  struct Particle {
    int32_t xQ8 = 0;
    int32_t yQ8 = 0;
    int16_t velocityXQ8 = 0;
    int16_t velocityYQ8 = 0;
    uint8_t radius = 1;
    uint8_t hue = 0;
  };

  static constexpr size_t kParticleCount = 24;

  uint32_t nextRandom();
  void resetParticle(Particle& particle, bool randomY);
  void renderProcedural(Arduino_Canvas& gfx, uint32_t nowMs);
  static int drawJpegBlock(JPEGDRAW* draw);
  int copyJpegBlock(const JPEGDRAW& draw);

  std::array<Particle, kParticleCount> particles_{};
  uint32_t randomState_ = 1;
  uint32_t lastUpdateMs_ = 0;
  JPEGDEC decoder_;
  uint16_t* videoFrame_ = nullptr;
  uint16_t* decodeFrame_ = nullptr;
  char activeWallpaperId_[17] = {};
  uint32_t videoFrameUpdatedMs_ = 0;
  bool videoFrameReady_ = false;
  bool decodeFrameValid_ = false;
};
