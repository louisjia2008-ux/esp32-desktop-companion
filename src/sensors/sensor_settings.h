#pragma once

namespace sensors {

// Stored by the application in a separate, versioned NVS record. Keep this
// independent from DisplaySettings, whose existing binary layout is retained.
struct SensorSettings {
  bool presenceWakeEnabled = true;
  bool co2ReminderEnabled = true;

  bool operator==(const SensorSettings& other) const {
    return presenceWakeEnabled == other.presenceWakeEnabled &&
           co2ReminderEnabled == other.co2ReminderEnabled;
  }
  bool operator!=(const SensorSettings& other) const { return !(*this == other); }
};

}  // namespace sensors
