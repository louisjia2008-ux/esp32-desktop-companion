#include "sensor_policy.h"

namespace sensors {

PresencePolicyState PresencePolicy::update(uint32_t nowMs, bool enabled,
                                           bool radarFresh, bool presenceKnown,
                                           bool presence, uint32_t sinceTouchMs) {
  PresencePolicyState result;
  if (!enabled || !radarFresh || !presenceKnown) {
    absenceStarted_ = false;
    absenceMature_ = false;
    return result;
  }

  result.overrideIdle = true;
  if (presence) {
    absenceStarted_ = false;
    absenceMature_ = false;
    result.awake = true;
    return result;
  }

  if (!absenceStarted_) {
    absenceStarted_ = true;
    absenceMature_ = false;
    absenceStartedAtMs_ = nowMs;
  }
  const uint32_t absentMs = nowMs - absenceStartedAtMs_;
  if (absentMs >= policy::kAbsentDimMs) absenceMature_ = true;
  result.continuousAbsentMs = absenceMature_ ? policy::kAbsentDimMs : absentMs;
  result.awake = !absenceMature_ || sinceTouchMs < policy::kAbsentDimMs;
  return result;
}

void Co2ReminderPolicy::interruptQualification() {
  highRunStarted_ = false;
  highForMs_ = 0;
  qualified_ = false;
}

Co2PolicyState Co2ReminderPolicy::update(const Co2PolicyInputs& in) {
  const bool fresh = in.hasSample && in.fresh &&
                     in.nowMs - in.sampleAtMs <= policy::kCo2FreshMs;
  if (!fresh) {
    interruptQualification();
  } else if (!seenSample_ || in.sampleAtMs != lastSampleAtMs_) {
    // Detect a missed stale interval even when no update() call occurred in
    // that interval and the next observation already contains a fresh sample.
    if (seenSample_ && in.sampleAtMs - lastSampleAtMs_ > policy::kCo2FreshMs) {
      interruptQualification();
    }
    seenSample_ = true;
    lastSampleAtMs_ = in.sampleAtMs;

    if (in.co2Ppm < policy::kRecoveredCo2Ppm) {
      highEpisode_ = false;
      interruptQualification();
    } else if (!qualified_) {
      if (in.co2Ppm >= policy::kHighCo2Ppm) {
        if (!highRunStarted_) {
          highRunStarted_ = true;
          highRunStartedAtMs_ = in.sampleAtMs;
        }
        highForMs_ = in.sampleAtMs - highRunStartedAtMs_;
        if (highForMs_ >= policy::kHighCo2DurationMs) {
          highForMs_ = policy::kHighCo2DurationMs;
          qualified_ = true;
          highEpisode_ = true;
        }
      } else {
        // Before qualification, 1300..1499 ppm breaks the >=1500 ppm run.
        // After qualification, hysteresis retains it until <1300 or stale.
        interruptQualification();
      }
    }
  }

  Co2PolicyState result;
  result.highEpisode = highEpisode_;
  result.qualifyingHigh = qualified_;
  result.highForMs = highForMs_;
  // Once elapsed, retain that fact even if an absent user defers the next
  // reminder for another full millis() period. A successful display rearms it.
  if (reminderShown_ && !cooldownReady_ &&
      in.nowMs - lastReminderAtMs_ >= policy::kReminderCooldownMs) {
    cooldownReady_ = true;
  }
  result.shouldRemind = fresh && highEpisode_ && qualified_ &&
      in.presenceFresh && in.presenceKnown && in.presence &&
      in.reminderEnabled && in.canShowReminder && cooldownReady_;
  return result;
}

void Co2ReminderPolicy::markReminderShown(uint32_t nowMs) {
  reminderShown_ = true;
  cooldownReady_ = false;
  lastReminderAtMs_ = nowMs;
}

}  // namespace sensors
