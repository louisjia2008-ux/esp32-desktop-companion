#include "sensor_service.h"

#if defined(SENSOR_SERVICE_ENABLED)

#include <Arduino.h>
#include <HardwareSerial.h>
#include <SensirionI2cScd4x.h>
#include <Wire.h>
#include <esp32-hal-i2c.h>
#include <esp32-hal-uart.h>
#include <math.h>

#if defined(SENSOR_LD2410C_RX_PROBE) || defined(SENSOR_SCD41_PROBE)
#error "Production sensor service and sensor probes cannot own the same buses"
#endif

namespace sensors {
namespace {
constexpr uint8_t kAirAddress = 0x62;
constexpr uint32_t kAirSampleTimeoutMs = 20000;
constexpr uint32_t kAirRecoveryPeriodMs = 30000;
constexpr uint32_t kAirPollMs = 500;
constexpr uint32_t kRadarBaud = 256000;
constexpr int8_t kRadarRxPin = 44;
constexpr uint32_t kRadarStackBytes = 4096;
constexpr uint32_t kAirStackBytes = 6144;

portMUX_TYPE snapshotMux = portMUX_INITIALIZER_UNLOCKED;
SensorSnapshot published;
struct UartErrors {
  uint32_t total = 0;
  uint32_t bufferFull = 0;
  uint32_t fifoOverflow = 0;
  uint32_t framing = 0;
  uint32_t parity = 0;
  uint32_t breaks = 0;
  int32_t lastError = 0;
};
UartErrors uartErrors;
bool started = false;
TwoWire airWire(1);
SensirionI2cScd4x airSensor;

int32_t serviceError(ServiceError value) {
  return static_cast<int32_t>(value);
}

void publishRadar(const RadarSnapshot& value) {
  portENTER_CRITICAL(&snapshotMux);
  published.radar = value;
  portEXIT_CRITICAL(&snapshotMux);
}

void publishAir(const AirSnapshot& value) {
  portENTER_CRITICAL(&snapshotMux);
  published.air = value;
  portEXIT_CRITICAL(&snapshotMux);
}

void onUartError(hardwareSerial_error_t error) {
  portENTER_CRITICAL(&snapshotMux);
  if (error != UART_NO_ERROR) uartErrors.lastError = static_cast<int32_t>(error);
  switch (error) {
    case UART_BUFFER_FULL_ERROR: ++uartErrors.bufferFull; ++uartErrors.total; break;
    case UART_FIFO_OVF_ERROR: ++uartErrors.fifoOverflow; ++uartErrors.total; break;
    case UART_FRAME_ERROR: ++uartErrors.framing; ++uartErrors.total; break;
    case UART_PARITY_ERROR: ++uartErrors.parity; ++uartErrors.total; break;
    case UART_BREAK_ERROR: ++uartErrors.breaks; ++uartErrors.total; break;
    default:
      break;
  }
  portEXIT_CRITICAL(&snapshotMux);
}

void radarWorker(void*) {
  RadarSnapshot value;
  value.health = Health::Starting;
  // This exact RX-only mapping was verified by real LD2410C frames. Do not
  // inherit another feature's UART configuration or ever select a TX pin.
  if (uart_get_RxPin(1) >= 0 || uart_get_TxPin(1) >= 0) {
    value.health = Health::Offline;
    ++value.initializationFailures;
    value.lastError = serviceError(ServiceError::ResourceBusy);
    publishRadar(value);
    vTaskDelete(nullptr);
    return;
  }
  Serial1.setRxBufferSize(4096);
  Serial1.onReceiveError(onUartError);
  Serial1.begin(kRadarBaud, SERIAL_8N1, kRadarRxPin, -1);
  if (!Serial1 || uart_get_RxPin(1) != kRadarRxPin ||
      uart_get_TxPin(1) != -1) {
    Serial1.end();
    value.health = Health::Offline;
    ++value.initializationFailures;
    value.lastError = serviceError(ServiceError::RxOnlyInvariant);
    publishRadar(value);
    vTaskDelete(nullptr);
    return;
  }

  Ld2410cParser parser;
  const uint32_t listeningAtMs = millis();
  uint32_t lastStackCheckMs = listeningAtMs;
  value.workerStackFree = uxTaskGetStackHighWaterMark(nullptr);
  for (;;) {
    const uint32_t nowMs = millis();
    // Bound each pass even if the wire carries continuous noise. This task
    // cannot wait on USB, UI, networking or the independently owned I2C bus.
    for (size_t count = 0; count < 256 && Serial1.available() > 0; ++count) {
      const int next = Serial1.read();
      if (next >= 0) parser.feed(static_cast<uint8_t>(next), nowMs);
    }
    parser.expire(nowMs);
    value.sample = parser.latest();
    value.counters = parser.counters();
    value.hasSample = parser.hasSample();
    if (value.hasSample) {
      value.health = parser.isFresh(nowMs, kRadarFreshMs) ? Health::Healthy
                                                        : Health::Stale;
    } else if (uint32_t(nowMs - listeningAtMs) > kRadarFreshMs) {
      value.health = Health::Stale;
    }
    if (uint32_t(nowMs - lastStackCheckMs) >= 1000) {
      value.workerStackFree = uxTaskGetStackHighWaterMark(nullptr);
      lastStackCheckMs = nowMs;
    }
    publishRadar(value);
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void recordCommunicationError(AirSnapshot& value, int16_t result,
                              uint32_t& consecutiveErrors) {
  const uint16_t raw = static_cast<uint16_t>(result);
  ++value.errors;
  ++consecutiveErrors;
  value.lastError = raw;
  if (raw == 0x020B) {
    ++value.crcErrors;
  } else if (raw == 0x020F) {
    ++value.shortReads;
  } else if ((raw & 0xFF00) == 0x0100 &&
             (uint8_t(raw) == I2cAddressNack ||
              uint8_t(raw) == I2cDataNack)) {
    // Arduino Wire can collapse several failures into these library codes.
    // The field therefore counts NACK-or-Wire-failure observations.
    ++value.nackErrors;
  } else {
    ++value.otherIoErrors;
  }
}

bool initializeAir(AirSnapshot& value, bool& ownsBus) {
  if (!ownsBus) {
    // Controller zero remains exclusively owned by the touch screen.
    if (i2cIsInit(1)) {
      value.lastError = serviceError(ServiceError::ResourceBusy);
      return false;
    }
    if (!airWire.begin(17, 18, 100000)) {
      value.lastError = serviceError(ServiceError::BusInitialization);
      return false;
    }
    ownsBus = true;
    airWire.setTimeOut(50);
    airSensor.begin(airWire, kAirAddress);
  }

  airWire.beginTransmission(kAirAddress);
  const uint8_t probeResult = airWire.endTransmission();
  value.lastWireResult = probeResult;
  if (probeResult != 0) {
    ++value.errors;
    if (probeResult == 2 || probeResult == 3) ++value.nackErrors;
    value.lastError = serviceError(ServiceError::AddressProbe);
    return false;
  }
  uint32_t ignoredConsecutiveErrors = 0;
  // This official command waits 500 ms on this worker only. It neither erases
  // calibration nor changes nonvolatile configuration. It allows safe re-entry
  // if the previous owner left the sensor in periodic measurement mode.
  int16_t result = airSensor.stopPeriodicMeasurement();
  if (result != 0) {
    recordCommunicationError(value, result, ignoredConsecutiveErrors);
    return false;
  }
  uint64_t serialNumber = 0;
  result = airSensor.getSerialNumber(serialNumber);
  if (result != 0) {
    recordCommunicationError(value, result, ignoredConsecutiveErrors);
    return false;
  }
  // Verify CRC-protected identification without publishing a unique serial.
  SCD4xSensorVariant variant = SCD4X_SENSOR_VARIANT_MASK;
  result = airSensor.getSensorVariant(variant);
  if (result != 0) {
    recordCommunicationError(value, result, ignoredConsecutiveErrors);
    return false;
  }
  if (variant != SCD4X_SENSOR_VARIANT_SCD41) {
    value.lastError = serviceError(ServiceError::WrongSensorVariant);
    return false;
  }
  result = airSensor.startPeriodicMeasurement();
  if (result != 0) {
    recordCommunicationError(value, result, ignoredConsecutiveErrors);
    return false;
  }
  return true;
}

void airWorker(void*) {
  AirSnapshot value;
  value.health = Health::Starting;
  bool ownsBus = false;
  bool measuring = false;
  bool recovery = false;
  bool attempted = false;
  uint32_t lastAttemptAtMs = 0;
  uint32_t waitingForSampleSinceMs = 0;
  uint32_t consecutiveErrors = 0;
  for (;;) {
    uint32_t nowMs = millis();
    if (!measuring) {
      // Retries are bounded by time and kept away from the display scheduler.
      if (!attempted || uint32_t(nowMs - lastAttemptAtMs) >= kAirRecoveryPeriodMs) {
        if (attempted) ++value.recoveryAttempts;
        attempted = true;
        lastAttemptAtMs = nowMs;
        value.health = recovery ? Health::Recovering : Health::Starting;
        publishAir(value);
        if (initializeAir(value, ownsBus)) {
          measuring = true;
          consecutiveErrors = 0;
          waitingForSampleSinceMs = millis();
          // Retained readings remain explicitly Recovering until a new valid
          // sample has proved that measurement restarted successfully.
          value.health = recovery ? Health::Recovering : Health::Starting;
        } else {
          ++value.initializationFailures;
          recovery = true;
          value.health = Health::Recovering;
        }
      }
      value.workerStackFree = uxTaskGetStackHighWaterMark(nullptr);
      publishAir(value);
      vTaskDelay(pdMS_TO_TICKS(kAirPollMs));
      continue;
    }

    nowMs = millis();
    if (uint32_t(nowMs - waitingForSampleSinceMs) >= kAirSampleTimeoutMs) {
      measuring = false;
      recovery = true;
      value.health = Health::Recovering;
      value.lastError = serviceError(ServiceError::SampleTimeout);
      // The 30-second recovery interval starts at the observed failure, not
      // an old successful initialization time that may be hours in the past.
      lastAttemptAtMs = nowMs;
    } else {
      bool ready = false;
      int16_t result = airSensor.getDataReadyStatus(ready);
      if (result != 0) {
        recordCommunicationError(value, result, consecutiveErrors);
      } else if (!ready) {
        consecutiveErrors = 0;
        ++value.notReady;
      } else {
        uint16_t co2 = 0;
        float temperature = NAN;
        float humidity = NAN;
        result = airSensor.readMeasurement(co2, temperature, humidity);
        if (result != 0) {
          recordCommunicationError(value, result, consecutiveErrors);
        } else {
          consecutiveErrors = 0;
          if (co2 == 0 || co2 > 40000 || !isfinite(temperature) ||
              !isfinite(humidity) || temperature < -10 || temperature > 60 ||
              humidity < 0 || humidity > 100) {
            ++value.invalidSamples;
            value.lastError = serviceError(ServiceError::InvalidSample);
          } else {
            value.hasSample = true;
            value.sampledAtMs = millis();
            value.co2Ppm = co2;
            value.temperatureC = temperature;
            value.humidityRh = humidity;
            ++value.validSamples;
            waitingForSampleSinceMs = value.sampledAtMs;
            value.health = Health::Healthy;
            value.lastError = 0;
            recovery = false;
          }
        }
      }
      if (consecutiveErrors >= 3) {
        measuring = false;
        recovery = true;
        value.health = Health::Recovering;
        lastAttemptAtMs = millis();
      } else if (value.health == Health::Healthy && value.hasSample &&
                 sampleAgeMs(millis(), value.sampledAtMs) > kAirFreshMs) {
        value.health = Health::Stale;
      }
    }
    value.workerStackFree = uxTaskGetStackHighWaterMark(nullptr);
    value.consecutiveErrors = consecutiveErrors;
    publishAir(value);
    vTaskDelay(pdMS_TO_TICKS(kAirPollMs));
  }
}
}  // namespace

void beginSensors() {
  if (started) return;
  started = true;
  RadarSnapshot radar;
  radar.health = Health::Starting;
  publishRadar(radar);
  AirSnapshot air;
  air.health = Health::Starting;
  publishAir(air);
  // Each allocation is independent: one failure never prevents the other
  // module from starting. No serial/logger initialization is required here.
  if (xTaskCreatePinnedToCore(radarWorker, "radar-service", kRadarStackBytes,
                            nullptr, 1, nullptr, 0) != pdPASS) {
    radar.health = Health::Offline;
    ++radar.initializationFailures;
    radar.lastError = serviceError(ServiceError::TaskAllocation);
    publishRadar(radar);
  }
  if (xTaskCreatePinnedToCore(airWorker, "scd41-service", kAirStackBytes,
                            nullptr, 1, nullptr, ARDUINO_RUNNING_CORE) != pdPASS) {
    air.health = Health::Offline;
    ++air.initializationFailures;
    air.lastError = serviceError(ServiceError::TaskAllocation);
    publishAir(air);
  }
}

SensorSnapshot readSensorSnapshot(uint32_t nowMs) {
  SensorSnapshot value;
  portENTER_CRITICAL(&snapshotMux);
  value = published;
  value.radar.uartErrors = uartErrors.total;
  value.radar.uartBufferFull = uartErrors.bufferFull;
  value.radar.uartFifoOverflow = uartErrors.fifoOverflow;
  value.radar.uartFramingErrors = uartErrors.framing;
  value.radar.uartParityErrors = uartErrors.parity;
  value.radar.uartBreaks = uartErrors.breaks;
  if (value.radar.lastError == 0) value.radar.lastError = uartErrors.lastError;
  portEXIT_CRITICAL(&snapshotMux);
  if (value.radar.health == Health::Healthy &&
      !radarFresh(value.radar, nowMs)) value.radar.health = Health::Stale;
  if (value.air.health == Health::Healthy &&
      !airFresh(value.air, nowMs)) value.air.health = Health::Stale;
  return value;
}
}  // namespace sensors

#else
namespace sensors {
void beginSensors() {}
SensorSnapshot readSensorSnapshot(uint32_t) { return SensorSnapshot{}; }
}  // namespace sensors
#endif
