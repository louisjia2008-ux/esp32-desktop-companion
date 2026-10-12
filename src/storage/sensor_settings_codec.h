#pragma once

#include <stddef.h>
#include <stdint.h>
#include "sensors/sensor_settings.h"

namespace sensors {
// Independent, explicit byte encoding; never writes DisplaySettings/disp_v1.
constexpr size_t kSensorSettingsBytes = 4;
inline void encodeSensorSettings(const SensorSettings& settings, uint8_t* bytes) {
  bytes[0] = 0x53;
  bytes[1] = 1;
  bytes[2] = (settings.presenceWakeEnabled ? 1 : 0) |
             (settings.co2ReminderEnabled ? 2 : 0);
  bytes[3] = bytes[2] ^ 0xA5;
}
inline bool decodeSensorSettings(const uint8_t* bytes, size_t length,
                                 SensorSettings& settings) {
  if (length != kSensorSettingsBytes || bytes[0] != 0x53 || bytes[1] != 1 ||
      (bytes[2] & ~3U) || bytes[3] != (bytes[2] ^ 0xA5)) return false;
  settings.presenceWakeEnabled = (bytes[2] & 1) != 0;
  settings.co2ReminderEnabled = (bytes[2] & 2) != 0;
  return true;
}
}  // namespace sensors
