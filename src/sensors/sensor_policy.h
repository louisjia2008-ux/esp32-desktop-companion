#pragma once

#include <cstdint>

namespace sensors {

namespace policy {
constexpr uint32_t kAbsentDimMs = 5U * 60U * 1000U;
constexpr uint32_t kCo2FreshMs = 15U * 1000U;
constexpr uint32_t kHighCo2DurationMs = 5U * 60U * 1000U;
constexpr uint32_t kReminderCooldownMs = 30U * 60U * 1000U;
constexpr uint16_t kHighCo2Ppm = 1500;
constexpr uint16_t kRecoveredCo2Ppm = 1300;
}  // namespace policy

struct PresencePolicyState {
  // False means the existing touch/time brightness rules retain control.
  bool overrideIdle = false;
  bool awake = false;
  // Saturates at the five-minute dim threshold, rather than wrapping after
  // prolonged absence. A fresh person or unknown/stale radar resets it.
  uint32_t continuousAbsentMs = 0;
};

class PresencePolicy {
 public:
  PresencePolicyState update(uint32_t nowMs, bool enabled, bool radarFresh,
                             bool presenceKnown, bool presence,
                             uint32_t sinceTouchMs);

 private:
  bool absenceStarted_ = false;
  bool absenceMature_ = false;
  uint32_t absenceStartedAtMs_ = 0;
};

struct Co2PolicyInputs {
  uint32_t nowMs = 0;
  bool hasSample = false;
  bool fresh = false;
  uint32_t sampleAtMs = 0;
  uint16_t co2Ppm = 0;
  bool presenceFresh = false;
  bool presenceKnown = false;
  bool presence = false;
  bool reminderEnabled = true;
  // The application clears this for another popup, OTA, pairing/setup, etc.
  bool canShowReminder = false;
};

struct Co2PolicyState {
  // Once established, an episode lasts until a fresh reading is < 1300 ppm.
  // Stale data can retain this history, but cannot qualify an alert.
  bool highEpisode = false;
  bool qualifyingHigh = false;
  uint32_t highForMs = 0;
  bool shouldRemind = false;
};

class Co2ReminderPolicy {
 public:
  Co2PolicyState update(const Co2PolicyInputs& inputs);
  // Call only after the local event system actually admits the reminder.
  void markReminderShown(uint32_t nowMs);

 private:
  void interruptQualification();
  bool seenSample_ = false;
  uint32_t lastSampleAtMs_ = 0;
  bool highRunStarted_ = false;
  uint32_t highRunStartedAtMs_ = 0;
  uint32_t highForMs_ = 0;
  bool qualified_ = false;
  bool highEpisode_ = false;
  bool reminderShown_ = false;
  bool cooldownReady_ = true;
  uint32_t lastReminderAtMs_ = 0;
};

}  // namespace sensors
