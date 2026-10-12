#include "sensors/sensor_policy.h"
#include "sensors/radar_range.h"
#include "sensors/sensor_service.h"
#include "sensors/sensor_settings.h"
#include "storage/sensor_settings_codec.h"
#include "ui/backlight_policy.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
  std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #condition); \
} while (false)

using sensors::Co2PolicyInputs;
using sensors::Co2PolicyState;
using sensors::Co2ReminderPolicy;
using sensors::PresencePolicy;
using sensors::PresencePolicyState;
namespace limits = sensors::policy;

Co2PolicyInputs sample(uint32_t at, uint16_t ppm = 1500) {
  Co2PolicyInputs in;
  in.nowMs = in.sampleAtMs = at;
  in.hasSample = in.fresh = true;
  in.co2Ppm = ppm;
  in.presenceFresh = in.presenceKnown = in.presence = true;
  in.canShowReminder = true;
  return in;
}

// These samples are OFFLINE policy fixtures, never physical sensor readings.
Co2PolicyState highRun(Co2ReminderPolicy& policy, uint32_t start,
                       uint32_t duration = limits::kHighCo2DurationMs,
                       bool present = true, bool enabled = true) {
  Co2PolicyState result;
  for (uint32_t elapsed = 0; elapsed <= duration; elapsed += 5000) {
    auto in = sample(start + elapsed);
    in.presence = present;
    in.reminderEnabled = enabled;
    result = policy.update(in);
  }
  return result;
}

uint8_t brightness(const DisplaySettings& settings, const PresencePolicyState& state,
                   uint32_t sinceTouch, bool night = false, bool attention = false) {
  BacklightInputs in;
  in.timeValid = true;
  in.minuteOfDay = night ? 23 * 60 : 12 * 60;
  in.sinceInteractionMs = sinceTouch;
  in.attention = attention;
  in.activityOverride = state.overrideIdle;
  in.overrideActive = state.awake;
  return targetBacklight(settings, in);
}

sensors::RadarSample radarSample(uint8_t state, uint16_t movingCm = 0,
                                 uint16_t stillCm = 0,
                                 uint8_t movingEnergy = 50,
                                 uint8_t stillEnergy = 50) {
  sensors::RadarSample sample;
  sample.targetState = state;
  sample.presenceKnown = state <= 3;
  sample.presence = sample.presenceKnown && state != 0;
  sample.moving = sample.presenceKnown && (state & 1) != 0;
  sample.stationary = sample.presenceKnown && (state & 2) != 0;
  sample.movingDistanceCm = movingCm;
  sample.stillDistanceCm = stillCm;
  sample.movingEnergy = movingEnergy;
  sample.stillEnergy = stillEnergy;
  return sample;
}

void radarRangeInclusiveBoundary() {
  CHECK(sensors::kWakeRangeCm == 100);
  for (uint16_t distance : {99, 100, 101}) {
    auto range = sensors::radarRange(radarSample(1, distance));
    CHECK(range.movingValid && !range.stillValid && range.hasDistance);
    CHECK(range.nearestDistanceCm == distance);
    CHECK(range.nearPresence == (distance <= 100));
    range = sensors::radarRange(radarSample(2, 0, distance));
    CHECK(!range.movingValid && range.stillValid && range.hasDistance);
    CHECK(range.nearestDistanceCm == distance);
    CHECK(range.nearPresence == (distance <= 100));
  }
}

void radarRangeBothUsesNearestQualifiedSummary() {
  auto range = sensors::radarRange(radarSample(3, 180, 70));
  CHECK(range.movingValid && range.stillValid && range.hasDistance);
  CHECK(range.nearestDistanceCm == 70 && range.nearPresence);
  range = sensors::radarRange(radarSample(3, 80, 220));
  CHECK(range.nearestDistanceCm == 80 && range.nearPresence);
  range = sensors::radarRange(radarSample(3, 101, 300));
  CHECK(range.nearestDistanceCm == 101 && !range.nearPresence);
  range = sensors::radarRange(radarSample(3, 100, 100));
  CHECK(range.nearestDistanceCm == 100 && range.nearPresence);
  // A near but zero-energy field cannot mask the far qualified summary.
  range = sensors::radarRange(radarSample(3, 40, 230, 0, 50));
  CHECK(!range.movingValid && range.stillValid && range.hasDistance);
  CHECK(range.nearestDistanceCm == 230 && !range.nearPresence);
  range = sensors::radarRange(radarSample(3, 240, 30, 50, 0));
  CHECK(range.movingValid && !range.stillValid && range.hasDistance);
  CHECK(range.nearestDistanceCm == 240 && !range.nearPresence);
}

void radarRangeRejectsAbsenceUnknownAndResidualFields() {
  for (uint8_t state : {0, 4, 5, 6}) {
    const auto range = sensors::radarRange(radarSample(state, 40, 50));
    CHECK(!range.movingValid && !range.stillValid && !range.hasDistance);
    CHECK(range.nearestDistanceCm == 0 && !range.nearPresence);
  }
  auto sample = radarSample(3, 40, 50);
  sample.presenceKnown = false;
  CHECK(!sensors::radarRange(sample).hasDistance);
  sample.presenceKnown = true;
  sample.presence = false;
  CHECK(!sensors::radarRange(sample).nearPresence);

  // Target-state flags, not residual fields for the inactive type, select the
  // summaries. The parser derives these flags from the validated state byte.
  auto range = sensors::radarRange(radarSample(1, 200, 20));
  CHECK(range.movingValid && !range.stillValid);
  CHECK(range.nearestDistanceCm == 200 && !range.nearPresence);
  range = sensors::radarRange(radarSample(2, 20, 210));
  CHECK(!range.movingValid && range.stillValid);
  CHECK(range.nearestDistanceCm == 210 && !range.nearPresence);
}

void radarRangeRejectsMissingDistanceAndEnergy() {
  for (const auto& sample : {
      radarSample(1, 0, 0), radarSample(2, 0, 0),
      radarSample(1, 50, 0, 0), radarSample(2, 0, 50, 50, 0),
      radarSample(3, 50, 60, 0, 0), radarSample(3, 0, 0),
      radarSample(3, 0, 60, 50, 0), radarSample(3, 50, 0, 0, 50)}) {
    const auto range = sensors::radarRange(sample);
    CHECK(!range.movingValid && !range.stillValid && !range.hasDistance);
    CHECK(range.nearestDistanceCm == 0 && !range.nearPresence);
  }
  auto range = sensors::radarRange(radarSample(3, 0, 90));
  CHECK(!range.movingValid && range.stillValid && range.nearPresence);
  range = sensors::radarRange(radarSample(3, 90, 0));
  CHECK(range.movingValid && !range.stillValid && range.nearPresence);
  range = sensors::radarRange(radarSample(1, UINT16_MAX, 0, 1));
  CHECK(range.hasDistance && range.nearestDistanceCm == UINT16_MAX);
  CHECK(!range.nearPresence);
}

void radarRangeCallerRequiresFreshnessAndRetainsFallback() {
  sensors::RadarSnapshot snapshot;
  snapshot.health = sensors::Health::Healthy;
  snapshot.hasSample = true;
  snapshot.sample = radarSample(1, 80);
  snapshot.sample.receivedAtMs = 10;
  const auto range = sensors::radarRange(snapshot.sample);
  CHECK(range.nearPresence);  // the pure range helper has no clock or bus
  PresencePolicy policy;
  auto state = policy.update(2010, true, sensors::radarFresh(snapshot, 2010),
      snapshot.sample.presenceKnown, range.nearPresence, 900000);
  CHECK(state.overrideIdle && state.awake);
  state = policy.update(2011, true, sensors::radarFresh(snapshot, 2011),
      snapshot.sample.presenceKnown, range.nearPresence, 900000);
  CHECK(!state.overrideIdle && !state.awake);
  CHECK(brightness(DisplaySettings(), state, 900000) == 30);
  CHECK(brightness(DisplaySettings(), state, 0) == 100);

  snapshot.health = sensors::Health::Recovering;
  CHECK(!sensors::radarFresh(snapshot, 20));
  snapshot.health = sensors::Health::Healthy;
  snapshot.hasSample = false;
  CHECK(!sensors::radarFresh(snapshot, 20));
}

void farRoomPresenceDimsWithoutSuppressingCo2Reminder() {
  const auto sample = radarSample(2, 0, 180);
  const auto range = sensors::radarRange(sample);
  CHECK(sample.presence && sample.presenceKnown && !range.nearPresence);
  PresencePolicy policy;
  auto state = policy.update(0, true, true, sample.presenceKnown,
                             range.nearPresence, 900000);
  CHECK(state.awake);
  state = policy.update(300000, true, true, sample.presenceKnown,
                        range.nearPresence, 900000);
  CHECK(state.overrideIdle && !state.awake);
  CHECK(brightness(DisplaySettings(), state, 900000) == 30);

  Co2ReminderPolicy reminder;
  CHECK(highRun(reminder, 0, 300000, sample.presence).shouldRemind);
}

void nearRangeAbsenceTouchNightAndReturn() {
  PresencePolicy policy;
  const auto far = sensors::radarRange(radarSample(3, 180, 200));
  const auto near = sensors::radarRange(radarSample(2, 0, 100));
  policy.update(1, true, true, true, far.nearPresence, 900000);
  auto state = policy.update(300001, true, true, true, far.nearPresence, 900000);
  CHECK(!state.awake && brightness(DisplaySettings(), state, 900000, true) == 10);
  state = policy.update(300002, true, true, true, far.nearPresence, 0);
  CHECK(state.awake && brightness(DisplaySettings(), state, 0, true) == 40);
  state = policy.update(600002, true, true, true, far.nearPresence, 300000);
  CHECK(!state.awake);
  state = policy.update(600003, true, true, true, near.nearPresence, 300001);
  CHECK(state.awake && state.continuousAbsentMs == 0);
  CHECK(brightness(DisplaySettings(), state, 300001, true) == 40);
}

void noQualifiedDistanceStillStartsNearAbsence() {
  // A fresh room-presence frame without a qualified distance cannot keep the
  // screen awake forever. Unknown/stale radar still takes the older fallback.
  const auto sample = radarSample(2, 0, 0);
  const auto range = sensors::radarRange(sample);
  CHECK(sample.presence && !range.hasDistance && !range.nearPresence);
  PresencePolicy policy;
  policy.update(0, true, true, sample.presenceKnown, range.nearPresence, 900000);
  auto state = policy.update(300000, true, true, sample.presenceKnown,
                            range.nearPresence, 900000);
  CHECK(state.overrideIdle && !state.awake);
  state = policy.update(300001, true, false, sample.presenceKnown,
                        range.nearPresence, 900000);
  CHECK(!state.overrideIdle);
}

void settingsAndPresenceDefaults() {
  sensors::SensorSettings settings;
  CHECK(settings.presenceWakeEnabled && settings.co2ReminderEnabled);
  const auto same = settings;
  CHECK(same == settings);
  settings.co2ReminderEnabled = false;
  CHECK(settings != same);

  PresencePolicy policy;
  auto state = policy.update(0, true, false, false, false, 0);
  CHECK(!state.overrideIdle);
  CHECK(state.continuousAbsentMs == 0);
  state = policy.update(0, true, true, true, true, 900000);
  CHECK(state.overrideIdle && state.awake);
  CHECK(state.continuousAbsentMs == 0);
  CHECK(brightness(DisplaySettings(), state, 900000) == 100);
}

void settingsCodecRoundtripAndRejectedRecords() {
  for (unsigned flags = 0; flags < 4; ++flags) {
    sensors::SensorSettings desired;
    desired.presenceWakeEnabled = (flags & 1) != 0;
    desired.co2ReminderEnabled = (flags & 2) != 0;
    uint8_t bytes[sensors::kSensorSettingsBytes];
    sensors::encodeSensorSettings(desired, bytes);
    CHECK(bytes[0] == 0x53 && bytes[1] == 1);
    CHECK(bytes[2] == flags && bytes[3] == (flags ^ 0xA5));
    sensors::SensorSettings restored;
    CHECK(sensors::decodeSensorSettings(bytes, sizeof(bytes), restored));
    CHECK(restored == desired);
  }

  const uint8_t malformed[][4] = {
    {0x52, 1, 3, 3 ^ 0xA5},  // wrong magic
    {0x53, 0, 3, 3 ^ 0xA5},  // old or unknown schema
    {0x53, 2, 3, 3 ^ 0xA5},
    {0x53, 1, 4, 4 ^ 0xA5},  // unknown flag, even with a matching checksum
    {0x53, 1, 3, 0},         // damaged checksum
  };
  for (const auto& bytes : malformed) {
    sensors::SensorSettings restored;
    const auto defaults = restored;
    CHECK(!sensors::decodeSensorSettings(bytes, sizeof(bytes), restored));
    CHECK(restored == defaults && restored.presenceWakeEnabled && restored.co2ReminderEnabled);
    restored.presenceWakeEnabled = false;
    const auto existing = restored;
    CHECK(!sensors::decodeSensorSettings(bytes, sizeof(bytes), restored));
    CHECK(restored == existing);  // a rejected record cannot partially alter settings
  }
  const uint8_t valid[] = {0x53, 1, 0, 0xA5, 0};
  for (size_t length : {0U, 1U, 2U, 3U, 5U}) {
    sensors::SensorSettings restored;
    CHECK(!sensors::decodeSensorSettings(valid, length, restored));
    CHECK(restored.presenceWakeEnabled && restored.co2ReminderEnabled);
  }
}

void continuousAbsenceAndTouchWake() {
  PresencePolicy policy;
  auto state = policy.update(0, true, true, true, false, 900000);
  CHECK(state.awake && state.continuousAbsentMs == 0);  // millis()==0 is valid
  state = policy.update(299999, true, true, true, false, 900000);
  CHECK(state.awake && state.continuousAbsentMs == 299999);
  state = policy.update(300000, true, true, true, false, 900000);
  CHECK(!state.awake && state.continuousAbsentMs == 300000);
  CHECK(brightness(DisplaySettings(), state, 900000) == 30);

  state = policy.update(300001, true, true, true, false, 0);
  CHECK(state.awake && state.continuousAbsentMs == 300000);
  state = policy.update(600000, true, true, true, false, 299999);
  CHECK(state.awake);
  state = policy.update(600001, true, true, true, false, 300000);
  CHECK(!state.awake);
  state = policy.update(600002, true, true, true, true, 300001);
  CHECK(state.awake && state.continuousAbsentMs == 0);
  state = policy.update(600003, true, true, true, false, 300002);
  CHECK(state.awake && state.continuousAbsentMs == 0);
}

void presenceNightAttentionAndFixedTimeout() {
  PresencePolicy policy;
  DisplaySettings display;
  display.idleDimS = 1;  // fresh presence uses the specified five minutes
  auto state = policy.update(20, true, true, true, false, 900000);
  state = policy.update(2020, true, true, true, false, 900000);
  CHECK(brightness(display, state, 900000) == 100);
  CHECK(brightness(display, state, 900000, true) == 40);
  state = policy.update(300020, true, true, true, false, 900000);
  CHECK(brightness(display, state, 900000) == 30);
  CHECK(brightness(display, state, 900000, true) == 10);
  CHECK(brightness(display, state, 900000, true, true) == 40);
  CHECK(brightness(display, state, 900000, false, true) == 100);
  display.idleDimS = 0;
  CHECK(brightness(display, state, 900000) == 30);
  state = policy.update(300021, true, true, true, true, 900000);
  CHECK(brightness(display, state, 900000, true) == 40);
}

void radarOfflineUnknownAndDisabledFallback() {
  PresencePolicy policy;
  DisplaySettings display;
  display.idleDimS = 60;
  policy.update(0, true, true, true, false, 900000);
  policy.update(299999, true, true, true, false, 900000);
  auto state = policy.update(300000, true, false, true, false, 900000);
  CHECK(!state.overrideIdle && state.continuousAbsentMs == 0);
  CHECK(brightness(display, state, 900000) == 30);
  CHECK(brightness(display, state, 1000) == 100);
  CHECK(brightness(display, state, 900000, true) == 10);
  CHECK(brightness(display, state, 1000, true) == 40);

  state = policy.update(300001, true, true, true, false, 900000);
  CHECK(state.awake && state.continuousAbsentMs == 0);  // unknown interval breaks absence
  state = policy.update(600001, true, true, false, false, 900000);
  CHECK(!state.overrideIdle);
  state = policy.update(600002, false, true, true, true, 900000);
  CHECK(!state.overrideIdle);
  CHECK(brightness(display, state, 900000) == 30);
  display.idleDimS = 0;
  CHECK(brightness(display, state, 900000) == 100);  // original no-idle rule survives
}

void presenceTimerWrapAndLongAbsence() {
  PresencePolicy policy;
  const uint32_t start = std::numeric_limits<uint32_t>::max() - 10000U;
  policy.update(start, true, true, true, false, 900000);
  auto state = policy.update(start + 299999U, true, true, true, false, 900000);
  CHECK(state.awake && state.continuousAbsentMs == 299999);
  state = policy.update(start + 300000U, true, true, true, false, 900000);
  CHECK(!state.awake && state.continuousAbsentMs == 300000);
  state = policy.update(start + 1U, true, true, true, false, 900000);
  CHECK(!state.awake && state.continuousAbsentMs == 300000);  // saturated after a full wrap
  state = policy.update(start + 2U, true, true, true, false, 0);
  CHECK(state.awake);  // a touch still wakes after long absence
}

void co2ExactContinuousThreshold() {
  Co2ReminderPolicy policy;
  auto state = highRun(policy, 0, 295000);
  CHECK(!state.highEpisode && !state.qualifyingHigh && !state.shouldRemind);
  CHECK(state.highForMs == 295000);
  state = policy.update(sample(299999));
  CHECK(!state.highEpisode && state.highForMs == 299999);
  state = policy.update(sample(300000));
  CHECK(state.highEpisode && state.qualifyingHigh && state.shouldRemind);
  CHECK(state.highForMs == 300000);
}

void sameSampleNeverAdvancesQualification() {
  Co2ReminderPolicy policy;
  auto in = sample(0);
  CHECK(policy.update(in).highForMs == 0);
  for (uint32_t now = 1; now <= 310000; now += 1000) {
    in.nowMs = now;
    const auto state = policy.update(in);
    CHECK(!state.highEpisode && !state.qualifyingHigh && !state.shouldRemind);
    CHECK(state.highForMs == 0);
  }
  // Caller freshness is defensively checked against the sample timestamp too.
  in.nowMs = 300000;
  CHECK(!policy.update(in).shouldRemind);
}

void lowSampleBreaksPendingAndHysteresis() {
  Co2ReminderPolicy policy;
  highRun(policy, 0, 295000);
  auto state = policy.update(sample(300000, 1499));
  CHECK(!state.highEpisode && state.highForMs == 0);
  state = highRun(policy, 305000);
  CHECK(state.highEpisode && state.shouldRemind);
  state = policy.update(sample(610000, 1300));
  CHECK(state.highEpisode && state.qualifyingHigh && state.shouldRemind);
  state = policy.update(sample(615000, 1499));
  CHECK(state.highEpisode && state.qualifyingHigh);
  state = policy.update(sample(620000, 1299));
  CHECK(!state.highEpisode && !state.qualifyingHigh && state.highForMs == 0);
  CHECK(!state.shouldRemind);
  state = policy.update(sample(625000, 1500));
  CHECK(!state.highEpisode && state.highForMs == 0);
}

void staleBreaksQualificationAndFreshRecovery() {
  Co2ReminderPolicy policy;
  highRun(policy, 0);
  auto in = sample(300000);
  in.nowMs = 315000;
  CHECK(policy.update(in).shouldRemind);  // fresh through exactly 15 seconds
  in.nowMs = 315001;
  auto state = policy.update(in);
  CHECK(state.highEpisode && !state.qualifyingHigh && !state.shouldRemind);
  CHECK(state.highForMs == 0);
  state = policy.update(sample(320000, 1499));
  CHECK(state.highEpisode && !state.qualifyingHigh);
  state = highRun(policy, 325000, 295000);
  CHECK(state.highEpisode && !state.qualifyingHigh && !state.shouldRemind);
  state = policy.update(sample(625000));
  CHECK(state.qualifyingHigh && state.shouldRemind);

  in = sample(630000);
  in.fresh = false;
  state = policy.update(in);
  CHECK(!state.qualifyingHigh && !state.shouldRemind);
  in.fresh = true;
  in.hasSample = false;
  CHECK(!policy.update(in).shouldRemind);
}

void missedStaleGapCannotJoinRuns() {
  Co2ReminderPolicy policy;
  highRun(policy, 0, 295000);
  auto state = policy.update(sample(310001));
  CHECK(!state.highEpisode && state.highForMs == 0 && !state.shouldRemind);
  state = highRun(policy, 315001);
  CHECK(state.shouldRemind);
  state = policy.update(sample(630002));
  CHECK(state.highEpisode && !state.qualifyingHigh && state.highForMs == 0);
  state = policy.update(sample(635002, 1299));
  CHECK(!state.highEpisode);
}

void deferredPresenceToggleAndAdmission() {
  Co2ReminderPolicy policy;
  auto state = highRun(policy, 0, 300000, false, false);
  CHECK(state.highEpisode && state.qualifyingHigh && !state.shouldRemind);
  auto in = sample(300000);
  in.presence = false;
  CHECK(!policy.update(in).shouldRemind);
  in.presence = true;
  in.presenceFresh = false;
  CHECK(!policy.update(in).shouldRemind);
  in.presenceFresh = true;
  in.presenceKnown = false;
  CHECK(!policy.update(in).shouldRemind);
  in.presenceKnown = true;
  in.reminderEnabled = false;
  CHECK(!policy.update(in).shouldRemind);
  in.reminderEnabled = true;
  in.canShowReminder = false;
  CHECK(!policy.update(in).shouldRemind);
  in.canShowReminder = true;
  CHECK(policy.update(in).shouldRemind);
  // A failed popup admission does not call markReminderShown and must retry.
  CHECK(policy.update(in).shouldRemind);
  policy.markReminderShown(in.nowMs);
  CHECK(!policy.update(in).shouldRemind);
  in.reminderEnabled = false;
  CHECK(policy.update(in).highEpisode);
  in.reminderEnabled = true;
  CHECK(!policy.update(in).shouldRemind);  // toggling never erases cooldown
}

void cooldownBoundaryAndEpisodeRecovery() {
  Co2ReminderPolicy policy;
  CHECK(highRun(policy, 0).shouldRemind);
  policy.markReminderShown(300000);
  auto state = highRun(policy, 305000, 1790000);
  CHECK(!state.shouldRemind);  // last sample at 2095000
  state = policy.update(sample(2099999));
  CHECK(!state.shouldRemind);
  state = policy.update(sample(2100000));
  CHECK(state.shouldRemind);
  policy.markReminderShown(2100000);
  CHECK(!policy.update(sample(2100000)).shouldRemind);
  CHECK(!policy.update(sample(2105000, 1299)).highEpisode);
  state = highRun(policy, 2110000);
  CHECK(state.highEpisode && !state.shouldRemind);  // new episode retains rate limit
}

void co2AndCooldownWrapIncludingTimestampZero() {
  Co2ReminderPolicy policy;
  const uint32_t start = 0U - limits::kHighCo2DurationMs;
  const auto initial = highRun(policy, start);
  CHECK(initial.highEpisode && initial.qualifyingHigh && initial.shouldRemind);
  policy.markReminderShown(0);  // zero is a valid shown-at timestamp
  auto state = highRun(policy, 5000, 1790000);
  CHECK(!state.shouldRemind);
  state = policy.update(sample(1799999));
  CHECK(!state.shouldRemind);
  state = policy.update(sample(1800000));
  CHECK(state.shouldRemind);

  Co2ReminderPolicy across;
  const uint32_t nonzeroStart = std::numeric_limits<uint32_t>::max() - 100000U;
  CHECK(highRun(across, nonzeroStart).shouldRemind);
  const uint32_t shown = nonzeroStart + 300000U;
  across.markReminderShown(shown);
  state = highRun(across, shown + 5000U, 1790000);
  CHECK(!state.shouldRemind);
  CHECK(across.update(sample(shown + 1800000U)).shouldRemind);
}

void elapsedCooldownStaysReadyAcrossLaterWrap() {
  Co2ReminderPolicy policy;
  CHECK(highRun(policy, 0).shouldRemind);
  policy.markReminderShown(300000);
  auto in = sample(2100000);
  in.presence = false;
  CHECK(!policy.update(in).shouldRemind);  // observes cooldown completion while away

  // A much later sample clock has passed another full millis() period. Its
  // modulo difference from the old shown-at value is only five minutes, but
  // the cooldown already completed and must not become armed again.
  in = sample(std::numeric_limits<uint32_t>::max());
  in.presence = false;
  CHECK(!policy.update(in).shouldRemind);
  const auto state = highRun(policy, 300000);
  CHECK(state.highEpisode && state.qualifyingHigh && state.shouldRemind);
  policy.markReminderShown(600000);
  CHECK(!policy.update(sample(605000)).shouldRemind);  // a real display rearms it
}

}  // namespace

int main() {
  struct Test { const char* name; void (*run)(); };
  const Test tests[] = {
    {"radar range inclusive 99/100/101 cm", radarRangeInclusiveBoundary},
    {"radar BOTH selects nearest qualified summary", radarRangeBothUsesNearestQualifiedSummary},
    {"radar range rejects absent/unknown/residual fields", radarRangeRejectsAbsenceUnknownAndResidualFields},
    {"radar range rejects missing distance/energy", radarRangeRejectsMissingDistanceAndEnergy},
    {"radar range freshness is required by caller", radarRangeCallerRequiresFreshnessAndRetainsFallback},
    {"far room presence dims but retains CO2 reminder", farRoomPresenceDimsWithoutSuppressingCo2Reminder},
    {"near range touch/night/return", nearRangeAbsenceTouchNightAndReturn},
    {"no qualified distance cannot keep screen awake", noQualifiedDistanceStillStartsNearAbsence},
    {"settings and presence defaults", settingsAndPresenceDefaults},
    {"settings codec and rejected records", settingsCodecRoundtripAndRejectedRecords},
    {"continuous absence and touch wake", continuousAbsenceAndTouchWake},
    {"night brightness, attention and fixed timeout", presenceNightAttentionAndFixedTimeout},
    {"radar offline, unknown and disabled fallback", radarOfflineUnknownAndDisabledFallback},
    {"presence timer wrap and long absence", presenceTimerWrapAndLongAbsence},
    {"CO2 exact continuous threshold", co2ExactContinuousThreshold},
    {"same sample never advances qualification", sameSampleNeverAdvancesQualification},
    {"pending reset and episode hysteresis", lowSampleBreaksPendingAndHysteresis},
    {"stale break and fresh recovery", staleBreaksQualificationAndFreshRecovery},
    {"missed stale gap cannot join runs", missedStaleGapCannotJoinRuns},
    {"presence, toggles and popup admission", deferredPresenceToggleAndAdmission},
    {"cooldown boundary and episode recovery", cooldownBoundaryAndEpisodeRecovery},
    {"CO2 and cooldown wrap, timestamp zero", co2AndCooldownWrapIncludingTimestampZero},
    {"elapsed cooldown survives a later wrap", elapsedCooldownStaysReadyAcrossLaterWrap},
  };
  for (const auto& test : tests) {
    try {
      test.run();
      std::cout << "PASS: " << test.name << '\n';
    } catch (const std::exception& error) {
      std::cerr << "FAIL: " << test.name << ": " << error.what() << '\n';
      return 1;
    }
  }
  std::cout << "PASS: all " << sizeof(tests) / sizeof(tests[0])
            << " sensor policy groups (offline fixtures only)\n";
  return 0;
}
