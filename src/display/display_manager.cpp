#include "display_manager.h"

#include <Arduino.h>

#include "board_config.h"

namespace {

constexpr uint32_t kColorTestDelayMs = 250;
constexpr uint8_t kBacklightChannel = 0;
constexpr uint32_t kBacklightFrequencyHz = 5000;
constexpr uint8_t kBacklightResolutionBits = 10;
uint8_t appliedBacklightPercent = 255;

// Keep the proven physical controller path at native rotation 0. The Canvas
// alone performs rotation 1, so flush() still sends the same native 320x480
// framebuffer layout that passed hardware bring-up.
Arduino_DataBus* const displayBus = new Arduino_ESP32QSPI(
    board::kLcdCs, board::kLcdClock, board::kLcdData0, board::kLcdData1,
    board::kLcdData2, board::kLcdData3);

Arduino_GFX* const physicalDisplay = new Arduino_AXS15231B(
    displayBus, board::kLcdReset, 0, false,
    board::kPanelWidth, board::kPanelHeight, 0, 0, 0, 0);

Arduino_Canvas* const frameBuffer = new Arduino_Canvas(
    board::kPanelWidth, board::kPanelHeight, physicalDisplay, 0, 0,
    board::kDisplayRotation);

void showSolidColor(uint16_t color) {
  frameBuffer->fillScreen(color);
  frameBuffer->flush();
  delay(kColorTestDelayMs);
}

}  // namespace

namespace display {

bool begin() {
  pinMode(board::kLcdTearingEffect, INPUT);
  pinMode(board::kLcdBacklight, OUTPUT);
  digitalWrite(board::kLcdBacklight, LOW);

  Serial.println("LCD init: START");
  if (!frameBuffer->begin()) {
    Serial.println("LCD init: FAIL");
    return false;
  }

  ledcSetup(kBacklightChannel, kBacklightFrequencyHz, kBacklightResolutionBits);
  ledcAttachPin(board::kLcdBacklight, kBacklightChannel);
  setBacklightLevel(100);
  Serial.printf("LCD init: OK (%dx%d, rotation=%u)\n", frameBuffer->width(),
                frameBuffer->height(), board::kDisplayRotation);

  Serial.println("LCD color test: BLACK RED GREEN BLUE BLACK");
  showSolidColor(BLACK);
  showSolidColor(RED);
  showSolidColor(GREEN);
  showSolidColor(BLUE);
  showSolidColor(BLACK);
  Serial.println("LCD color test: COMPLETE (visual confirmation required)");
  return true;
}

Arduino_Canvas& canvas() { return *frameBuffer; }

void setBacklightLevel(uint8_t percent) {
  percent = percent > 100 ? 100 : percent;
  if (percent == appliedBacklightPercent) return;
  appliedBacklightPercent = percent;
  constexpr uint32_t kMaximumDuty = (1U << kBacklightResolutionBits) - 1;
  uint32_t duty = 0;
  if (percent >= 100) {
    // 2^bits keeps the output permanently high: no switching at full
    // brightness, so the backlight driver cannot add noise near the antenna.
    duty = 1U << kBacklightResolutionBits;
  } else if (percent > 0) {
    const float linear = powf(percent / 100.0f, 2.2f);
    duty = static_cast<uint32_t>(lroundf(linear * kMaximumDuty));
    if (duty == 0) duty = 1;  // never fully off unless asked to be
  }
  ledcWrite(kBacklightChannel, duty);
}

}  // namespace display
