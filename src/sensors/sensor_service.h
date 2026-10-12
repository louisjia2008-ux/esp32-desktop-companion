#pragma once

#include <stdint.h>
#include "ld2410c_parser.h"

namespace sensors {

enum class Health : uint8_t { Starting, Healthy, Stale, Recovering, Offline };

inline const char* healthName(Health health) {
  switch (health) {
    case Health::Starting: return "STARTING";
    case Health::Healthy: return "HEALTHY";
    case Health::Stale: return "STALE";
    case Health::Recovering: return "RECOVERING";
    case Health::Offline: return "OFFLINE";
  }
  return "OFFLINE";
}

// Negative values describe service failures. Positive air errors are the
// official Sensirion codes; positive radar errors are hardwareSerial_error_t.
enum class ServiceError : int32_t {
  None = 0,
  TaskAllocation = -1001,
  ResourceBusy = -1002,
  BusInitialization = -1003,
  RxOnlyInvariant = -1004,
  AddressProbe = -1005,
  WrongSensorVariant = -1006,
  SampleTimeout = -1007,
  InvalidSample = -1008,
};

struct RadarSnapshot {
  Health health = Health::Offline;
  bool hasSample = false;
  RadarSample sample{};
  RadarCounters counters{};
  uint32_t uartErrors = 0;
  uint32_t uartBufferFull = 0;
  uint32_t uartFifoOverflow = 0;
  uint32_t uartFramingErrors = 0;
  uint32_t uartParityErrors = 0;
  uint32_t uartBreaks = 0;
  uint32_t initializationFailures = 0;
  // ESP-IDF/Arduino-ESP32 high-water marks are bytes, unlike upstream FreeRTOS.
  uint32_t workerStackFree = 0;
  int32_t lastError = 0;
};

struct AirSnapshot {
  Health health = Health::Offline;
  bool hasSample = false;
  uint32_t sampledAtMs = 0;
  uint16_t co2Ppm = 0;
  float temperatureC = 0;
  float humidityRh = 0;
  uint32_t validSamples = 0;
  uint32_t errors = 0;
  uint32_t nackErrors = 0;
  uint32_t crcErrors = 0;
  uint32_t shortReads = 0;
  uint32_t otherIoErrors = 0;
  uint32_t consecutiveErrors = 0;
  uint32_t notReady = 0;
  uint32_t invalidSamples = 0;
  uint32_t recoveryAttempts = 0;
  uint32_t initializationFailures = 0;
  uint32_t workerStackFree = 0;
  uint8_t lastWireResult = 0;
  int32_t lastError = 0;
};

struct SensorSnapshot {
  RadarSnapshot radar{};
  AirSnapshot air{};
};

constexpr uint32_t kRadarFreshMs = 2000;
constexpr uint32_t kAirFreshMs = 15000;

// A sample can be published between a caller reading its clock and copying the
// snapshot. Treat that small future timestamp as age zero. Real sample ages and
// timer differences are bounded below half the uint32_t clock period.
inline uint32_t sampleAgeMs(uint32_t nowMs, uint32_t sampledAtMs) {
  const uint32_t age = nowMs - sampledAtMs;
  return age > UINT32_MAX - 1000u ? 0u : age;
}

inline bool radarFresh(const RadarSnapshot& value, uint32_t nowMs) {
  return value.health == Health::Healthy && value.hasSample &&
         sampleAgeMs(nowMs, value.sample.receivedAtMs) <= kRadarFreshMs;
}

inline bool airFresh(const AirSnapshot& value, uint32_t nowMs) {
  return value.health == Health::Healthy && value.hasSample &&
         sampleAgeMs(nowMs, value.sampledAtMs) <= kAirFreshMs;
}

// Safe to call repeatedly. Sampling never depends on USB or a log reporter.
// Only the worker tasks access UART/I2C; callers receive a consistent value copy.
void beginSensors();
SensorSnapshot readSensorSnapshot(uint32_t nowMs);

}  // namespace sensors
