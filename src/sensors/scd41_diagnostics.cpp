#include "scd41_diagnostics.h"

#if defined(SENSOR_SCD41_PROBE)
#include <Arduino.h>
#include <SensirionI2cScd4x.h>
#include <Wire.h>
#include <esp32-hal-i2c.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include "diagnostic_log.h"

namespace sensors {
namespace {
constexpr uint8_t kAddress = 0x62;
constexpr uint32_t kSampleTimeoutMs = 20000;
TwoWire sensorWire(1);
SensirionI2cScd4x sensor;
bool started = false;
// All counters below have one owner: the worker task.
uint32_t sequence = 0, shortWrites = 0, formatErrors = 0;
uint32_t validSamples = 0, invalidSamples = 0, notReady = 0;
uint32_t commErrors = 0, nackErrors = 0, crcErrors = 0, shortReads = 0;
uint32_t otherIoErrors = 0, consecutiveErrors = 0, co2RangeWarnings = 0;

void line(const char* format, ...) __attribute__((format(printf, 1, 2)));
void line(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  const int length = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(buffer)) {
    ++formatErrors;
#if defined(SENSOR_DUAL_PROBE)
  } else if (writeDiagnosticLine(reinterpret_cast<const uint8_t*>(buffer), length) !=
#else
  } else if (Serial.write(reinterpret_cast<const uint8_t*>(buffer), length) !=
#endif
             static_cast<size_t>(length)) {
    ++shortWrites;
  }
  // USB output stays off the display loop. Host verification of sequences and
  // line counts is required: HWCDC can discard bytes after accepting a write.
#if !defined(SENSOR_DUAL_PROBE)
  vTaskDelay(pdMS_TO_TICKS(5));
#endif
}

void beginReport(unsigned lines) {
  ++sequence;
  line("SCD41: BEGIN SEQ=%lu LINES=%u USB_LOG_SHORT_WRITES=%lu FORMAT_ERRORS=%lu\n",
       (unsigned long)sequence, lines, (unsigned long)shortWrites,
       (unsigned long)formatErrors);
}
void endReport(unsigned lines) {
  line("SCD41: END SEQ=%lu LINES=%u USB_LOG_SHORT_WRITES=%lu FORMAT_ERRORS=%lu\n",
       (unsigned long)sequence, lines, (unsigned long)shortWrites,
       (unsigned long)formatErrors);
}

void event(const char* text) {
  beginReport(3);
  line("SCD41: %s UPTIME_MS=%lu SEQ=%lu\n", text,
       (unsigned long)millis(), (unsigned long)sequence);
  endReport(3);
}

void communicationError(const char* operation, int16_t result) {
  const uint16_t raw = static_cast<uint16_t>(result);
  const uint8_t low = static_cast<uint8_t>(raw);
  const char* category = "OTHER_IO";
  ++commErrors;
  ++consecutiveErrors;
  if (raw == 0x020B) { ++crcErrors; category = "CRC"; }
  else if (raw == 0x020F) { ++shortReads; category = "SHORT_READ"; }
  else if ((raw & 0xFF00) == 0x0100 &&
           (low == I2cAddressNack || low == I2cDataNack)) {
    ++nackErrors;
    category = "NACK_OR_WIRE_FAILURE";
  } else { ++otherIoErrors; }
  beginReport(4);
  line("SCD41: ERROR OP=%s CODE=0x%04X CATEGORY=%s CONSECUTIVE=%lu UPTIME_MS=%lu SEQ=%lu\n",
       operation, unsigned(raw), category, (unsigned long)consecutiveErrors,
       (unsigned long)millis(), (unsigned long)sequence);
  line("SCD41: COMM_ERRORS=%lu NACK_OR_WIRE_FAILURES=%lu CRC_ERRORS=%lu SHORT_READS=%lu OTHER_IO_ERRORS=%lu SEQ=%lu\n",
       (unsigned long)commErrors, (unsigned long)nackErrors,
       (unsigned long)crcErrors, (unsigned long)shortReads,
       (unsigned long)otherIoErrors, (unsigned long)sequence);
  endReport(4);
}

void abortTask(const char* reason) {
  event(reason);
  event("DIAGNOSTIC_HALTED SENSOR_RESET=UNCONFIRMED AUTOMATIC_RECOVERY=DISABLED");
  // No further commands, reset, or automatic restart hides the first failure.
  vTaskDelete(nullptr);
}

void sampleReport(uint16_t co2, float temperature, float humidity, uint32_t now) {
  beginReport(8);
  line("SCD41: CO2=%u ppm SAMPLE=%lu UPTIME_MS=%lu SEQ=%lu\n",
       unsigned(co2), (unsigned long)validSamples, (unsigned long)now,
       (unsigned long)sequence);
  line("SCD41: TEMPERATURE=%.2f C SAMPLE=%lu SEQ=%lu\n", double(temperature),
       (unsigned long)validSamples, (unsigned long)sequence);
  line("SCD41: HUMIDITY=%.2f %% SAMPLE=%lu SEQ=%lu\n", double(humidity),
       (unsigned long)validSamples, (unsigned long)sequence);
  line("SCD41: VALID_SAMPLES=%lu INVALID_SAMPLES=%lu NOT_READY=%lu CO2_SPEC_RANGE=%s CO2_RANGE_WARNINGS=%lu SEQ=%lu\n",
       (unsigned long)validSamples, (unsigned long)invalidSamples,
       (unsigned long)notReady, co2 >= 400 && co2 <= 5000 ? "WITHIN" : "OUTSIDE",
       (unsigned long)co2RangeWarnings, (unsigned long)sequence);
  line("SCD41: COMM_ERRORS=%lu NACK_OR_WIRE_FAILURES=%lu CRC_ERRORS=%lu SHORT_READS=%lu OTHER_IO_ERRORS=%lu SEQ=%lu\n",
       (unsigned long)commErrors, (unsigned long)nackErrors,
       (unsigned long)crcErrors, (unsigned long)shortReads,
       (unsigned long)otherIoErrors, (unsigned long)sequence);
  line("SCD41: SENSOR_RESET=UNCONFIRMED SEQ=%lu\n", (unsigned long)sequence);
  endReport(8);
}

void worker(void*) {
  event("PROBE I2C=1 SDA=17 SCL=18 HZ=100000 TIMEOUT_MS=50");
  // Never reconfigure another owner's initialized controller.
  if (i2cIsInit(1)) { abortTask("INIT_FAILED I2C1_ALREADY_INITIALIZED"); return; }
  if (!sensorWire.begin(17, 18, 100000)) {
    abortTask("INIT_FAILED I2C_BEGIN"); return;
  }
  sensorWire.setTimeOut(50);
  vTaskDelay(pdMS_TO_TICKS(50));
  sensorWire.beginTransmission(kAddress);
  const uint8_t probeResult = sensorWire.endTransmission();
  beginReport(3);
  line("SCD41: ADDRESS_PROBE ADDRESS=0x62 WIRE_RESULT=%u SEQ=%lu\n",
       unsigned(probeResult), (unsigned long)sequence);
  endReport(3);
  if (probeResult != 0) { abortTask("INIT_FAILED ADDRESS_PROBE"); return; }
  event("FOUND 0x62 MEASUREMENT_NOT_YET_VERIFIED");
  sensor.begin(sensorWire, kAddress);
  // The official library waits 500 ms here; only this worker is blocked.
  int16_t result = sensor.stopPeriodicMeasurement();
  if (result != 0) {
    communicationError("STOP_PERIODIC", result);
    abortTask("INIT_FAILED STOP_PERIODIC"); return;
  }
  event("STOP_PERIODIC_OK");
  uint64_t serialNumber = 0;
  result = sensor.getSerialNumber(serialNumber);
  if (result != 0) {
    communicationError("SERIAL_NUMBER", result);
    abortTask("INIT_FAILED SERIAL_NUMBER"); return;
  }
  // Verify the response without exposing the sensor's unique identifier.
  event("SERIAL_NUMBER_READ_OK");
  SCD4xSensorVariant variant = SCD4X_SENSOR_VARIANT_MASK;
  result = sensor.getSensorVariant(variant);
  if (result != 0) {
    communicationError("SENSOR_VARIANT", result);
    abortTask("INIT_FAILED SENSOR_VARIANT"); return;
  }
  beginReport(3);
  line("SCD41: SENSOR_VARIANT=0x%04X EXPECTED=0x1000 SEQ=%lu\n",
       unsigned(variant), (unsigned long)sequence);
  endReport(3);
  if (variant != SCD4X_SENSOR_VARIANT_SCD41) {
    abortTask("INIT_FAILED SENSOR_VARIANT_MISMATCH"); return;
  }
  result = sensor.startPeriodicMeasurement();
  if (result != 0) {
    communicationError("START_PERIODIC", result);
    abortTask("INIT_FAILED START_PERIODIC"); return;
  }
  uint32_t lastSampleMs = millis();
  event("INITIALIZED PERIODIC_STARTED MEASUREMENT_NOT_YET_VERIFIED");
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (uint32_t(millis() - lastSampleMs) >= kSampleTimeoutMs) {
      abortTask("SAMPLE_TIMEOUT NO_VALID_SAMPLE_FOR_20000_MS"); return;
    }
    bool ready = false;
    result = sensor.getDataReadyStatus(ready);
    if (result != 0) {
      communicationError("DATA_READY", result);
      if (consecutiveErrors >= 3) {
        abortTask("COMMUNICATION_ABORT THREE_CONSECUTIVE_ERRORS"); return;
      }
      continue;
    }
    if (!ready) { ++notReady; consecutiveErrors = 0; continue; }
    uint16_t co2 = 0;
    float temperature = NAN, humidity = NAN;
    result = sensor.readMeasurement(co2, temperature, humidity);
    if (result != 0) {
      communicationError("READ_MEASUREMENT", result);
      if (consecutiveErrors >= 3) {
        abortTask("COMMUNICATION_ABORT THREE_CONSECUTIVE_ERRORS"); return;
      }
      continue;
    }
    consecutiveErrors = 0;
    const uint32_t now = millis();
    if (co2 == 0 || co2 > 40000 || !isfinite(temperature) || !isfinite(humidity) ||
        temperature < -10 || temperature > 60 || humidity < 0 || humidity > 100) {
      ++invalidSamples;
      beginReport(3);
      line("SCD41: INVALID_SAMPLE CO2=%u TEMPERATURE=%.2f HUMIDITY=%.2f INVALID_SAMPLES=%lu UPTIME_MS=%lu SEQ=%lu\n",
           unsigned(co2), double(temperature), double(humidity),
           (unsigned long)invalidSamples, (unsigned long)now, (unsigned long)sequence);
      endReport(3);
      continue;
    }
    ++validSamples;
    lastSampleMs = now;
    if (co2 < 400 || co2 > 5000) ++co2RangeWarnings;
    sampleReport(co2, temperature, humidity, now);
  }
}
}  // namespace

void beginScd41Diagnostics() {
  if (started) return;
  started = true;
  // USB pacing, the 500 ms initialization wait, and every I2C transaction stay
  // on this worker, never in the wallpaper/main-loop scheduler.
  if (xTaskCreatePinnedToCore(worker, "scd41-probe", 6144, nullptr, 1, nullptr,
                            ARDUINO_RUNNING_CORE) != pdPASS) {
    Serial.println("SCD41: INIT_FAILED TASK_ALLOCATION");
  }
}
}  // namespace sensors
#else
namespace sensors {
void beginScd41Diagnostics() {}
}  // namespace sensors
#endif
