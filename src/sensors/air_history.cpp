#include "air_history.h"

#include <math.h>
#include <string.h>

namespace sensors {
namespace {
constexpr uint32_t kRecordMagic = 0x31524841u;
const uint8_t kHeaderMagic[8] = {'D', 'P', 'H', 'A', 'H', '0', '0', '1'};
uint16_t read16(const uint8_t* p) {
  return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
}
uint32_t read32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}
void write16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void write32(uint8_t* p, uint32_t v) {
  for (uint8_t i = 0; i < 4; ++i) p[i] = uint8_t(v >> (i * 8));
}
bool validDay(int32_t day) { return day >= 10957 && day <= 47481; }
}

uint32_t airHistoryCrc(const uint8_t* data, size_t bytes) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < bytes; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xffffffffu;
}

void encodeAirHistoryHeader(int32_t dayOrdinal, uint8_t* output) {
  memset(output, 0, kAirHistoryHeaderBytes);
  memcpy(output, kHeaderMagic, sizeof(kHeaderMagic));
  write16(output + 8, 1);
  write16(output + 10, kAirHistoryHeaderBytes);
  write32(output + 12, uint32_t(dayOrdinal));
  write16(output + 16, kAirHistoryRecordBytes);
  write32(output + 28, airHistoryCrc(output, 28));
}

bool decodeAirHistoryHeader(const uint8_t* input, int32_t& dayOrdinal) {
  if (memcmp(input, kHeaderMagic, sizeof(kHeaderMagic)) != 0 ||
      read16(input + 8) != 1 || read16(input + 10) != kAirHistoryHeaderBytes ||
      read16(input + 16) != kAirHistoryRecordBytes ||
      read32(input + 28) != airHistoryCrc(input, 28)) return false;
  for (uint8_t i = 18; i < 28; ++i) if (input[i] != 0) return false;
  dayOrdinal = int32_t(read32(input + 12));
  return validDay(dayOrdinal);
}

void encodeAirHistoryRecord(const AirHistoryRecord& value, uint8_t* output) {
  memset(output, 0, kAirHistoryRecordBytes);
  write32(output, kRecordMagic);
  write32(output + 4, value.utcEpochSeconds);
  write16(output + 8, value.minuteOfDay);
  write16(output + 10, value.sampleCount);
  write16(output + 12, value.co2Ppm);
  write16(output + 14, uint16_t(value.temperatureCentiC));
  write16(output + 16, value.humidityCentiRh);
  write16(output + 18, value.discontinuity ? 1 : 0);
  write32(output + 20, value.uptimeMs);
  write32(output + 28, airHistoryCrc(output, 28));
}

bool decodeAirHistoryRecord(const uint8_t* input, int32_t dayOrdinal,
                            AirHistoryRecord& value) {
  if (!validDay(dayOrdinal) || read32(input) != kRecordMagic ||
      read32(input + 28) != airHistoryCrc(input, 28) || read32(input + 24) != 0 ||
      read16(input + 18) > 1) return false;
  value.dayOrdinal = dayOrdinal;
  value.utcEpochSeconds = read32(input + 4);
  value.minuteOfDay = read16(input + 8);
  value.sampleCount = read16(input + 10);
  value.co2Ppm = read16(input + 12);
  value.temperatureCentiC = int16_t(read16(input + 14));
  value.humidityCentiRh = read16(input + 16);
  value.discontinuity = read16(input + 18) != 0;
  value.uptimeMs = read32(input + 20);
  return value.utcEpochSeconds >= 946684800u &&
         value.utcEpochSeconds <= 4102444799u && value.minuteOfDay < 1440 &&
         value.sampleCount > 0 && value.sampleCount <= 1200 && value.co2Ppm > 0 &&
         value.temperatureCentiC >= -4000 && value.temperatureCentiC <= 12500 &&
         value.humidityCentiRh <= 10000;
}

bool historyDay(const HistoryClock& clock, int32_t& dayOrdinal,
                uint16_t& minuteOfDay) {
  if (!clock.valid || clock.utcEpochSeconds < 946684800u ||
      clock.utcEpochSeconds > 4102444799u || clock.utcOffsetSeconds < -50400 ||
      clock.utcOffsetSeconds > 50400) return false;
  const int64_t local = int64_t(clock.utcEpochSeconds) + clock.utcOffsetSeconds;
  if (local < 0) return false;
  dayOrdinal = int32_t(local / 86400);
  minuteOfDay = uint16_t((local % 86400) / 60);
  return validDay(dayOrdinal);
}

void historyCivilDate(int32_t dayOrdinal, uint16_t& year, uint8_t& month,
                      uint8_t& day) {
  // Gregorian conversion using the 400-year civil calendar cycle.
  const int32_t z = dayOrdinal + 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = uint32_t(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int32_t y = int32_t(yoe) + era * 400;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  day = uint8_t(doy - (153 * mp + 2) / 5 + 1);
  month = uint8_t(int32_t(mp) + (mp < 10 ? 3 : -9));
  y += month <= 2;
  year = uint16_t(y);
}

void AirHistoryAccumulator::finish(AirHistoryCounters& counters) {
  if (active_.sampleCount == 0) return;
  active_.co2Ppm = uint16_t(co2Sum_ / active_.sampleCount);
  active_.temperatureCentiC = int16_t(temperatureSum_ / active_.sampleCount);
  active_.humidityCentiRh = uint16_t(humiditySum_ / active_.sampleCount);
  if (count_ == 16) {
    head_ = uint8_t((head_ + 1) % 16);
    --count_;
    ++counters.droppedMinutes;
    if (count_) queue_[head_].discontinuity = true;
  }
  queue_[(head_ + count_) % 16] = active_;
  ++count_;
  ++counters.recordedMinutes;
  active_ = AirHistoryRecord{};
  co2Sum_ = humiditySum_ = 0;
  temperatureSum_ = 0;
}

void AirHistoryAccumulator::observe(uint32_t nowMs, const AirSnapshot& air,
                                    const HistoryClock& clock,
                                    AirHistoryCounters& counters) {
  int32_t nowDay = -1;
  uint16_t nowMinute = 0;
  if (!historyDay(clock, nowDay, nowMinute)) {
    finish(counters);
    hasClock_ = false;
    nextDiscontinuity_ = true;
    return;
  }
  if (hasClock_) {
    const int64_t elapsed = uint32_t(nowMs - lastClockAt_) / 1000;
    const int64_t clockElapsed = int64_t(clock.utcEpochSeconds) - lastClockEpoch_;
    if (clock.utcOffsetSeconds != lastOffset_ || clockElapsed < 0 ||
        clockElapsed - elapsed > 2 || elapsed - clockElapsed > 2) {
      finish(counters);
      nextDiscontinuity_ = true;
      ++counters.clockDiscontinuities;
    }
  }
  lastClockAt_ = nowMs;
  lastClockEpoch_ = clock.utcEpochSeconds;
  lastOffset_ = clock.utcOffsetSeconds;
  hasClock_ = true;
  if (active_.sampleCount &&
      (active_.dayOrdinal != nowDay || active_.minuteOfDay != nowMinute))
    finish(counters);
  if (!airFresh(air, nowMs)) {
    if (active_.sampleCount) active_.discontinuity = true;
    nextDiscontinuity_ = true;
    return;
  }
  if (hasSample_ && lastSampleCounter_ == air.validSamples &&
      lastSampleAt_ == air.sampledAtMs) return;
  lastSampleCounter_ = air.validSamples;
  lastSampleAt_ = air.sampledAtMs;
  hasSample_ = true;
  if (!isfinite(air.temperatureC) || !isfinite(air.humidityRh) ||
      air.co2Ppm == 0 || air.temperatureC < -40 || air.temperatureC > 125 ||
      air.humidityRh < 0 || air.humidityRh > 100) {
    if (active_.sampleCount) active_.discontinuity = true;
    nextDiscontinuity_ = true;
    return;
  }
  HistoryClock sampledClock = clock;
  sampledClock.utcEpochSeconds -= sampleAgeMs(nowMs, air.sampledAtMs) / 1000;
  int32_t sampleDay = -1;
  uint16_t sampleMinute = 0;
  if (!historyDay(sampledClock, sampleDay, sampleMinute)) return;
  if (active_.sampleCount && (active_.dayOrdinal != sampleDay ||
                             active_.minuteOfDay != sampleMinute)) finish(counters);
  const bool sampleGap = counters.acceptedSamples &&
      uint32_t(air.sampledAtMs - lastAcceptedAt_) > kAirFreshMs;
  if (sampleGap) {
    if (active_.sampleCount) active_.discontinuity = true;
    nextDiscontinuity_ = true;
  }
  if (active_.sampleCount == 0) {
    active_.dayOrdinal = sampleDay;
    active_.minuteOfDay = sampleMinute;
    active_.utcEpochSeconds = sampledClock.utcEpochSeconds;
    active_.uptimeMs = air.sampledAtMs;
    active_.discontinuity = nextDiscontinuity_;
    nextDiscontinuity_ = false;
  }
  if (active_.sampleCount >= 1200) return;
  co2Sum_ += air.co2Ppm;
  temperatureSum_ += int32_t(lroundf(air.temperatureC * 100));
  humiditySum_ += uint32_t(lroundf(air.humidityRh * 100));
  ++active_.sampleCount;
  ++counters.acceptedSamples;
  lastAcceptedAt_ = air.sampledAtMs;
}

bool AirHistoryAccumulator::pop(AirHistoryRecord& record) {
  if (!count_) return false;
  record = queue_[head_];
  head_ = uint8_t((head_ + 1) % 16);
  --count_;
  return true;
}

void AirHistoryPlot::reset(AirHistoryBucket* buckets) {
  buckets_ = buckets;
  if (buckets_) for (uint16_t i = 0; i < kAirHistoryBuckets; ++i)
    buckets_[i] = AirHistoryBucket{};
  validMinutes_ = 0;
  previousMinute_ = 0;
  activeBucket_ = UINT16_MAX;
  hasPrevious_ = false;
  nextGap_ = true;
}

void AirHistoryPlot::add(const AirHistoryRecord& record) {
  if (!buckets_ || record.minuteOfDay >= 1440) return;
  const uint16_t index = record.minuteOfDay / 5;
  const uint8_t bit = uint8_t(1u << (record.minuteOfDay % 5));
  const bool contiguous = hasPrevious_ && !nextGap_ && !record.discontinuity &&
                          record.minuteOfDay == previousMinute_ + 1;
  AirHistoryBucket& bucket = buckets_[index];
  if ((bucket.minuteMask & bit) != 0) {
    // Retain the first stored value for a repeated wall-clock minute. Raw
    // later records stay on SD, but unique-minute counts cannot be inflated.
    bucket.discontinuous = true;
    bucket.connectFromPrevious = false;
    if (index + 1 < kAirHistoryBuckets) buckets_[index + 1].connectFromPrevious = false;
    previousMinute_ = record.minuteOfDay;
    hasPrevious_ = true;
    nextGap_ = true;
    return;
  }
  if (!bucket.valid) {
    bucket.connectFromPrevious = contiguous && index > 0 &&
        buckets_[index - 1].valid && !buckets_[index - 1].discontinuous;
  }
  if (!contiguous) {
    bucket.connectFromPrevious = false;
    if (bucket.valid || (hasPrevious_ && previousMinute_ / 5 == index))
      bucket.discontinuous = true;
    if (index + 1 < kAirHistoryBuckets) buckets_[index + 1].connectFromPrevious = false;
  }
  const uint8_t previousCount = bucket.minuteCount;
  ++bucket.minuteCount;
  bucket.minuteMask |= bit;
  // Values are integer display precision (ppm / 0.01 C / 0.01 %RH). The
  // incremental mean may round by one unit; no synthetic sample fills a gap.
  bucket.co2Ppm = uint16_t((uint32_t(bucket.co2Ppm) * previousCount +
                           record.co2Ppm) / bucket.minuteCount);
  bucket.temperatureCentiC = int16_t((int32_t(bucket.temperatureCentiC) *
      previousCount + record.temperatureCentiC) / bucket.minuteCount);
  bucket.humidityCentiRh = uint16_t((uint32_t(bucket.humidityCentiRh) *
      previousCount + record.humidityCentiRh) / bucket.minuteCount);
  bucket.valid = true;
  ++validMinutes_;
  previousMinute_ = record.minuteOfDay;
  activeBucket_ = index;
  hasPrevious_ = true;
  nextGap_ = false;
}

}  // namespace sensors
