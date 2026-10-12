#pragma once

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

// Main-loop owner. Production lines are dropped whole when USB has no room.
// Arduino-ESP32 2.0.17 HWCDC::write can wait for its TX ring; checking the
// entire line first avoids that path. Sampling tasks never call this logger.
class RuntimeLog {
 public:
  template <typename... Args>
  void printf(const char* format, Args... args) {
#if defined(SENSOR_SERVICE_ENABLED)
    char line[512];
    const int length = snprintf(line, sizeof(line), format, args...);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(line)) { ++dropped_; return; }
    write(line, static_cast<size_t>(length));
#else
    Serial.printf(format, args...);
#endif
  }
  void println(const char* line = "") { printf("%s\n", line); }
  uint32_t droppedLines() const { return dropped_; }
 private:
  void write(const char* line, size_t length) {
    if (!length) return;
    if (Serial.availableForWrite() < static_cast<int>(length)) { ++dropped_; return; }
    if (Serial.write(reinterpret_cast<const uint8_t*>(line), length) != length) ++dropped_;
  }
  uint32_t dropped_ = 0;
};
