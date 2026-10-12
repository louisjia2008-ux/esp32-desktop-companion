#pragma once

#include <stddef.h>
#include <stdint.h>
#include "sensor_service.h"

namespace sensors {

constexpr uint16_t kAirHistoryBuckets = 288;
constexpr uint16_t kAirHistoryRecordBytes = 32;
constexpr uint16_t kAirHistoryHeaderBytes = 32;
constexpr uint16_t kAirHistoryMaxRecords = 2880;
constexpr uint16_t kAirHistoryIoBytes = 256;

struct HistoryClock {
  uint32_t utcEpochSeconds = 0;
  int32_t utcOffsetSeconds = 0;
  bool valid = false;
};

enum class HistoryState : uint8_t {
  WaitingForClock, Starting, Loading, Ready, SdUnavailable, StorageError,
  MemoryUnavailable
};

inline const char* historyStateName(HistoryState state) {
  switch (state) {
    case HistoryState::WaitingForClock: return "WAITING_FOR_CLOCK";
    case HistoryState::Starting: return "STARTING";
    case HistoryState::Loading: return "LOADING";
    case HistoryState::Ready: return "READY";
    case HistoryState::SdUnavailable: return "SD_UNAVAILABLE";
    case HistoryState::StorageError: return "STORAGE_ERROR";
    case HistoryState::MemoryUnavailable: return "MEMORY_UNAVAILABLE";
  }
  return "STORAGE_ERROR";
}

struct AirHistoryBucket {
  bool valid = false;
  bool connectFromPrevious = false;
  uint16_t co2Ppm = 0;
  uint16_t humidityCentiRh = 0;
  int16_t temperatureCentiC = 0;
  uint8_t minuteCount = 0;
  // Coverage is also used to ignore repeated local minutes after reboot or
  // clock rollback. A gap inside a 5-minute bin breaks both adjoining lines.
  uint8_t minuteMask = 0;
  bool discontinuous = false;
};

struct AirHistoryView {
  HistoryState state = HistoryState::WaitingForClock;
  bool clockValid = false;
  int32_t dayOrdinal = -1;
  int32_t todayOrdinal = -1;
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t daysAgo = 0;
  uint8_t availableDayMask = 0;
  const AirHistoryBucket* buckets = nullptr;
  uint16_t bucketCount = kAirHistoryBuckets;
  uint32_t validMinutes = 0;
};

struct AirHistoryCounters {
  uint32_t acceptedSamples = 0;
  uint32_t recordedMinutes = 0;
  uint32_t writtenMinutes = 0;
  uint32_t droppedMinutes = 0;
  uint32_t ioErrors = 0;
  uint32_t crcErrors = 0;
  uint32_t invalidRecords = 0;
  uint32_t tornTails = 0;
  uint32_t protectedFiles = 0;
  uint32_t clockDiscontinuities = 0;
  uint32_t maxIoMs = 0;
  int32_t lastError = 0;
};

struct AirHistoryRecord {
  int32_t dayOrdinal = -1;
  uint32_t utcEpochSeconds = 0;
  uint32_t uptimeMs = 0;
  uint16_t minuteOfDay = 0;
  uint16_t sampleCount = 0;
  uint16_t co2Ppm = 0;
  int16_t temperatureCentiC = 0;
  uint16_t humidityCentiRh = 0;
  bool discontinuity = true;
};

// Portable codec. The SD format is explicitly little endian, versioned and CRC
// checked; no native struct layout, floating-point ABI or calendar guesswork.
uint32_t airHistoryCrc(const uint8_t* data, size_t bytes);
void encodeAirHistoryHeader(int32_t dayOrdinal, uint8_t* output);
bool decodeAirHistoryHeader(const uint8_t* input, int32_t& dayOrdinal);
void encodeAirHistoryRecord(const AirHistoryRecord& value, uint8_t* output);
bool decodeAirHistoryRecord(const uint8_t* input, int32_t dayOrdinal,
                            AirHistoryRecord& value);
bool historyDay(const HistoryClock& clock, int32_t& dayOrdinal,
                uint16_t& minuteOfDay);
void historyCivilDate(int32_t dayOrdinal, uint16_t& year, uint8_t& month,
                      uint8_t& day);

// Main-loop-only aggregation, independent of whether SD IO is currently allowed.
// Each queued record represents real fresh, unique SCD41 samples in one minute.
class AirHistoryAccumulator {
 public:
  void observe(uint32_t nowMs, const AirSnapshot& air, const HistoryClock& clock,
               AirHistoryCounters& counters);
  bool pop(AirHistoryRecord& record);
  bool pending() const { return count_ != 0; }
 private:
  void finish(AirHistoryCounters& counters);
  AirHistoryRecord active_{};
  AirHistoryRecord queue_[16]{};
  uint32_t co2Sum_ = 0;
  int32_t temperatureSum_ = 0;
  uint32_t humiditySum_ = 0;
  uint32_t lastSampleCounter_ = 0;
  uint32_t lastSampleAt_ = 0;
  uint32_t lastClockAt_ = 0;
  uint32_t lastClockEpoch_ = 0;
  uint32_t lastAcceptedAt_ = 0;
  int32_t lastOffset_ = 0;
  uint8_t head_ = 0;
  uint8_t count_ = 0;
  bool hasSample_ = false;
  bool hasClock_ = false;
  bool nextDiscontinuity_ = true;
};

// Build an immutable UI view incrementally without joining a missing minute,
// clock jump, corrupt record, duplicate local minute or backwards chronology.
class AirHistoryPlot {
 public:
  void reset(AirHistoryBucket* buckets);
  void add(const AirHistoryRecord& record);
  void gap() { nextGap_ = true; }
  uint32_t validMinutes() const { return validMinutes_; }
 private:
  AirHistoryBucket* buckets_ = nullptr;
  uint32_t validMinutes_ = 0;
  uint16_t previousMinute_ = 0;
  uint16_t activeBucket_ = UINT16_MAX;
  bool hasPrevious_ = false;
  bool nextGap_ = true;
};

}  // namespace sensors
