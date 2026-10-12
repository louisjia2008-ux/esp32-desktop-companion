#pragma once

#include <Arduino.h>

struct TouchPoint {
  uint16_t x = 0;
  uint16_t y = 0;
};

enum class TouchReadResult : uint8_t {
  UP,
  DOWN,
  ERROR,
};

enum class TouchReadError : uint8_t {
  NONE,
  COMMAND,
  RESPONSE,
  POINT_COUNT,
  COORDINATES,
};

struct TouchReadDiagnostics {
  TouchReadError error = TouchReadError::NONE;
  uint8_t pointCount = 0;
  uint8_t event = 0;
  uint16_t rawX = 0;
  uint16_t rawY = 0;
  // The packet carried no current contact: an explicit lift event or the
  // controller's idle filler (for example count=0xFF) seen after a release.
  bool idleReport = false;
  uint8_t raw[8] = {};
  bool interruptLow = false;
};

class Axs15231bTouch {
 public:
  bool begin();
  bool devicePresent();
  TouchReadResult read(TouchPoint& point);
  TouchReadDiagnostics diagnostics() const { return diagnostics_; }

 private:
  static constexpr size_t kResponseLength = 8;
  // The AXS15231B reports at most five contacts. Larger counts are not touch
  // data; after a finger lifts the controller repeats such filler packets
  // instead of returning count=0.
  static constexpr uint8_t kMaximumReportedPoints = 5;
  static constexpr uint8_t kLiftEvent = 1;
  TouchReadDiagnostics diagnostics_{};
};
