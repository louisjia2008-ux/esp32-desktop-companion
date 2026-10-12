#include "backlight_policy.h"

#include <algorithm>

bool DisplaySettings::operator==(const DisplaySettings& other) const {
  return nightStartMin == other.nightStartMin && nightEndMin == other.nightEndMin &&
         dayLevel == other.dayLevel && nightLevel == other.nightLevel &&
         nightWakeLevel == other.nightWakeLevel && idleDimS == other.idleDimS &&
         idleLevel == other.idleLevel;
}

bool validDisplaySettings(const DisplaySettings& s) {
  return s.nightStartMin < 1440 && s.nightEndMin < 1440 &&
         s.dayLevel >= 5 && s.dayLevel <= 100 &&
         s.nightLevel >= 1 && s.nightLevel <= 100 &&
         s.nightWakeLevel >= s.nightLevel && s.nightWakeLevel <= 100 &&
         s.idleLevel >= 1 && s.idleLevel <= s.dayLevel &&
         s.idleDimS <= 3600;
}

bool isNight(const DisplaySettings& s, uint16_t minuteOfDay) {
  if (s.nightStartMin == s.nightEndMin) return false;
  if (s.nightStartMin < s.nightEndMin) {
    return minuteOfDay >= s.nightStartMin && minuteOfDay < s.nightEndMin;
  }
  return minuteOfDay >= s.nightStartMin || minuteOfDay < s.nightEndMin;  // wraps midnight
}

uint8_t targetBacklight(const DisplaySettings& s, const BacklightInputs& in) {
  const bool touchActive = s.idleDimS == 0 ||
      in.sinceInteractionMs < static_cast<uint32_t>(s.idleDimS) * 1000U;
  const bool active = in.attention ||
      (in.activityOverride ? in.overrideActive : touchActive);
  // Without a synced clock night cannot be known: stay on the day rules.
  if (in.timeValid && isNight(s, in.minuteOfDay)) {
    return active ? s.nightWakeLevel : s.nightLevel;
  }
  return active ? s.dayLevel : s.idleLevel;
}

uint8_t BacklightFader::step(uint8_t target, uint32_t nowMs, uint32_t durationMs) {
  const uint32_t targetQ8 = static_cast<uint32_t>(std::min<uint8_t>(target, 100)) << 8;
  if (!started_) {
    started_ = true;
    transitionMs_ = nowMs;
    levelQ8_ = fromQ8_ = targetQ8_ = targetQ8;
    return level();
  }
  const uint32_t elapsed = std::min(nowMs - transitionMs_, durationMs_);
  levelQ8_ = static_cast<uint32_t>(static_cast<int32_t>(fromQ8_) +
      (static_cast<int32_t>(targetQ8_) - static_cast<int32_t>(fromQ8_)) *
      static_cast<int32_t>(elapsed) / static_cast<int32_t>(durationMs_));
  if (targetQ8 != targetQ8_) {
    fromQ8_ = levelQ8_;
    targetQ8_ = targetQ8;
    transitionMs_ = nowMs;
    durationMs_ = std::min<uint32_t>(std::max<uint32_t>(durationMs, 1), 2000);
  }
  return level();
}
