#include "touch_driver.h"

#include <Wire.h>

#include <cstring>

#include "board_config.h"

namespace {

// Read one touch point (6 bytes) plus the two-byte gesture/count header.
// The final three zero bytes are part of the AXS15231B I2C transaction used
// by Espressif-compatible drivers, not padding added by the compiler.
constexpr uint8_t kReadTouchCommand[11] = {
    0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};

}  // namespace

bool Axs15231bTouch::begin() {
  pinMode(board::kTouchInterrupt, INPUT);
  if (!Wire.begin(board::kTouchSda, board::kTouchScl,
                  board::kTouchI2cFrequency)) {
    return false;
  }
  Wire.setClock(board::kTouchI2cFrequency);
  return devicePresent();
}

bool Axs15231bTouch::devicePresent() {
  Wire.beginTransmission(board::kTouchAddress);
  return Wire.endTransmission() == 0;
}

TouchReadResult Axs15231bTouch::read(TouchPoint& point) {
  diagnostics_ = {};
  diagnostics_.interruptLow = digitalRead(board::kTouchInterrupt) == LOW;
  uint8_t response[kResponseLength] = {};

  Wire.beginTransmission(board::kTouchAddress);
  const size_t written = Wire.write(kReadTouchCommand, sizeof(kReadTouchCommand));
  if (written != sizeof(kReadTouchCommand) || Wire.endTransmission() != 0) {
    diagnostics_.error = TouchReadError::COMMAND;
    return TouchReadResult::ERROR;
  }

  const size_t received = Wire.requestFrom(
      static_cast<uint8_t>(board::kTouchAddress),
      static_cast<uint8_t>(kResponseLength));
  if (received != kResponseLength) {
    diagnostics_.error = TouchReadError::RESPONSE;
    while (Wire.available()) {
      Wire.read();
    }
    return TouchReadResult::ERROR;
  }

  for (size_t index = 0; index < kResponseLength; ++index) {
    if (!Wire.available()) {
      diagnostics_.error = TouchReadError::RESPONSE;
      return TouchReadResult::ERROR;
    }
    response[index] = static_cast<uint8_t>(Wire.read());
  }

  memcpy(diagnostics_.raw, response, sizeof(diagnostics_.raw));
  const uint8_t pointCount = response[1];
  diagnostics_.pointCount = pointCount;
  diagnostics_.event = response[2] >> 6;
  diagnostics_.rawX =
      (static_cast<uint16_t>(response[2] & 0x0F) << 8) | response[3];
  diagnostics_.rawY =
      (static_cast<uint16_t>(response[4] & 0x0F) << 8) | response[5];
  if (pointCount == 0) {
    return TouchReadResult::UP;
  }
  // Observed on hardware: once a finger lifts, this controller never returns
  // count=0 again and instead repeats filler such as count=0xFF event=3.
  // Classifying that as a read error left the UI waiting for a release
  // forever, so it is a valid "no contact" report. Bus failures above remain
  // errors and are never treated as release.
  if (pointCount > kMaximumReportedPoints) {
    diagnostics_.idleReport = true;
    return TouchReadResult::UP;
  }
  if (pointCount > 1) {
    diagnostics_.error = TouchReadError::POINT_COUNT;
    return TouchReadResult::ERROR;
  }

  // A lift event carries the last contact's coordinates; it is a release.
  if (diagnostics_.event == kLiftEvent) {
    diagnostics_.idleReport = true;
    return TouchReadResult::UP;
  }

  const uint16_t rawX =
      (static_cast<uint16_t>(response[2] & 0x0F) << 8) | response[3];
  const uint16_t rawY =
      (static_cast<uint16_t>(response[4] & 0x0F) << 8) | response[5];

  // The touch controller always reports native portrait coordinates. Reject
  // malformed packets here; display rotation is a separate upper-layer step.
  if (rawX >= board::kPanelWidth || rawY >= board::kPanelHeight) {
    diagnostics_.error = TouchReadError::COORDINATES;
    return TouchReadResult::ERROR;
  }

  point.x = rawX;
  point.y = rawY;
  return TouchReadResult::DOWN;
}
