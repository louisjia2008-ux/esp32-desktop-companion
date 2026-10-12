#pragma once

#include "ld2410c_parser.h"

namespace sensors {

constexpr uint16_t kWakeRangeCm = 100;

struct RadarRange {
  bool movingValid = false;
  bool stillValid = false;
  bool hasDistance = false;
  // The nearest qualified moving/still summary, not an individual person ID
  // or a guarantee that all people in the room have been resolved.
  uint16_t nearestDistanceCm = 0;
  bool nearPresence = false;
};

// Pure interpretation of one parsed frame. Callers must separately require a
// fresh, healthy radar snapshot before using this result as current data.
// Target states can retain old distance/energy fields during absence; an
// active state alone also does not guarantee a usable distance. Requiring
// presence, the matching state flag, positive energy and a positive distance
// keeps either case from waking the screen on an unqualified value.
inline RadarRange radarRange(const RadarSample& sample) {
  RadarRange result;
  if (!sample.presenceKnown || !sample.presence) return result;

  result.movingValid = sample.moving && sample.movingEnergy > 0 &&
                       sample.movingDistanceCm > 0;
  result.stillValid = sample.stationary && sample.stillEnergy > 0 &&
                     sample.stillDistanceCm > 0;
  result.hasDistance = result.movingValid || result.stillValid;
  if (result.movingValid) result.nearestDistanceCm = sample.movingDistanceCm;
  if (result.stillValid &&
      (!result.movingValid || sample.stillDistanceCm < result.nearestDistanceCm)) {
    result.nearestDistanceCm = sample.stillDistanceCm;
  }
  result.nearPresence = result.hasDistance &&
                        result.nearestDistanceCm <= kWakeRangeCm;
  return result;
}

}  // namespace sensors
