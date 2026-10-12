#pragma once

#include <stdint.h>
#include <algorithm>

#include "sensors/air_history.h"

namespace environment_chart {

// Values retain the SD codec's integer units: ppm, centi-degrees and centi-%RH.
inline int32_t value(const sensors::AirHistoryBucket& bucket, uint8_t metric) {
  if (metric == 1) return bucket.temperatureCentiC;
  if (metric == 2) return bucket.humidityCentiRh;
  return bucket.co2Ppm;
}

struct Scale {
  int32_t low = 0;
  int32_t high = 2000;
  uint16_t points = 0;
};

inline Scale scale(const sensors::AirHistoryView& view, uint8_t metric) {
  Scale result;
  if (metric == 1) { result.low = 1800; result.high = 3000; }
  else if (metric == 2) { result.low = 0; result.high = 10000; }
  if (!view.buckets) return result;
  int32_t minimum = INT32_MAX, maximum = INT32_MIN;
  for (uint16_t index = 0; index < std::min<uint16_t>(
           view.bucketCount, sensors::kAirHistoryBuckets); ++index) {
    const auto& bucket = view.buckets[index];
    if (!bucket.valid) continue;
    const int32_t point = value(bucket, metric);
    minimum = std::min(minimum, point);
    maximum = std::max(maximum, point);
    ++result.points;
  }
  if (result.points == 0) return result;
  const int32_t step = metric == 0 ? 100 : metric == 1 ? 100 : 500;
  const int32_t minimumSpan = metric == 0 ? 400 : metric == 1 ? 400 : 2000;
  const int32_t padding = std::max<int32_t>((maximum - minimum) / 10, step);
  minimum -= padding;
  maximum += padding;
  if (maximum - minimum < minimumSpan) {
    const int32_t extra = minimumSpan - (maximum - minimum);
    minimum -= extra / 2;
    maximum += extra - extra / 2;
  }
  // Signed division truncates toward zero; floor explicitly for negatives.
  result.low = minimum >= 0 ? minimum / step * step :
      -((-minimum + step - 1) / step) * step;
  result.high = maximum >= 0 ? (maximum + step - 1) / step * step :
      -((-maximum) / step) * step;
  if (metric != 1) result.low = std::max<int32_t>(0, result.low);
  if (metric == 2) result.high = std::min<int32_t>(10000, result.high);
  return result;
}

inline int16_t x(uint16_t bucket) {
  // Axis ends at 24:00; bucket 287 begins at 23:55.
  return static_cast<int16_t>(66 + static_cast<uint32_t>(bucket) * 380U / 288U);
}

inline int16_t y(int32_t point, const Scale& range) {
  const int32_t clamped = std::min(range.high, std::max(range.low, point));
  return static_cast<int16_t>(226 -
      static_cast<int64_t>(clamped - range.low) * 84 /
      std::max<int32_t>(1, range.high - range.low));
}

inline bool connects(const sensors::AirHistoryView& view, uint16_t index) {
  return view.buckets && index > 0 && index < view.bucketCount &&
      index < sensors::kAirHistoryBuckets && view.buckets[index].valid &&
      view.buckets[index - 1].valid && view.buckets[index].connectFromPrevious;
}

}  // namespace environment_chart
