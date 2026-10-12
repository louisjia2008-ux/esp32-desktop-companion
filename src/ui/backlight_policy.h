#pragma once

#include <cstddef>
#include <cstdint>

// Screen brightness rules. Pure functions so the host tests cover the
// midnight wrap, idle dimming and waking without hardware.
struct DisplaySettings {
  uint16_t nightStartMin = 23 * 60;  // minutes after local midnight
  uint16_t nightEndMin = 7 * 60;     // equal start/end disables night mode
  uint8_t dayLevel = 100;            // percent
  uint8_t nightLevel = 10;
  uint8_t nightWakeLevel = 40;       // night, after a touch or with a card
  uint16_t idleDimS = 300;           // 0 never dims for idleness
  uint8_t idleLevel = 30;

  bool operator==(const DisplaySettings& other) const;
  bool operator!=(const DisplaySettings& other) const { return !(*this == other); }
};

bool validDisplaySettings(const DisplaySettings& settings);
bool isNight(const DisplaySettings& settings, uint16_t minuteOfDay);

struct BacklightInputs {
  bool timeValid = false;
  uint16_t minuteOfDay = 0;
  uint32_t sinceInteractionMs = 0;
  bool attention = false;  // a card or notification is on screen
  // A fresh presence policy can select awake/idle without changing the
  // persisted DisplaySettings layout. Attention always takes precedence.
  bool activityOverride = false;
  bool overrideActive = false;
};

uint8_t targetBacklight(const DisplaySettings& settings, const BacklightInputs& inputs);

// A normal brightness change takes 300 ms. Pass 2000 ms when crossing a
// night/day boundary; repeated calls with the same target retain that fade.
class BacklightFader {
 public:
  uint8_t step(uint8_t target, uint32_t nowMs, uint32_t durationMs = 300);
  uint8_t level() const { return static_cast<uint8_t>((levelQ8_ + 128) >> 8); }

 private:
  uint32_t levelQ8_ = 100U << 8;
  uint32_t fromQ8_ = 100U << 8;
  uint32_t targetQ8_ = 100U << 8;
  uint32_t transitionMs_ = 0;
  uint32_t durationMs_ = 300;
  bool started_ = false;
};
