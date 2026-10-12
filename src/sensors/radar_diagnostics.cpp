#include "radar_diagnostics.h"

#if defined(SENSOR_LD2410C_RX_PROBE)
#include <Arduino.h>
#include <HardwareSerial.h>
#include <esp32-hal-uart.h>
#include <stdarg.h>
#include <stdio.h>
#include "ld2410c_parser.h"
#include "diagnostic_log.h"

namespace sensors {
namespace {
// JC3248W535 schematic: P1 RXD2 -> R7 -> U0RXD (GPIO44).
// SC3248W535 equivalence is unverified; user authorized this single candidate
// RX-only experiment on 2026-10-11. Do not scan pins or initialize any TX pin.
constexpr int8_t kCandidateRx = 44;
constexpr uint32_t kBaud = 256000;
constexpr uint32_t kReportMs = 1000;
constexpr size_t kRxBufferBytes = 4096;
static_assert(ARDUINO_RUNNING_CORE == 0 || ARDUINO_RUNNING_CORE == 1,
              "Radar reporting requires a pinned Arduino core");

struct Snapshot {
  RadarSample sample{};
  RadarCounters counters{};
  bool hasSample = false;
};
struct UartErrors {
  uint32_t bufferFull = 0, fifoOverflow = 0, framing = 0, parity = 0, breaks = 0;
};
QueueHandle_t snapshots = nullptr;
TaskHandle_t worker = nullptr;
TaskHandle_t reporter = nullptr;
portMUX_TYPE errorMux = portMUX_INITIALIZER_UNLOCKED;
UartErrors uartErrors;
bool started = false;
uint32_t lastReportMs = 0, lastReportedFrames = 0;
// Only the reporter task accesses these counters and the reporting timestamps.
uint32_t reportSequence = 0, usbLogShortWrites = 0, formatErrors = 0;

void reportLine(const char* format, ...) __attribute__((format(printf, 1, 2)));
void reportLine(const char* format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  const int length = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(line)) {
    ++formatErrors;
#if defined(SENSOR_DUAL_PROBE)
  } else if (writeDiagnosticLine(reinterpret_cast<const uint8_t*>(line),
#else
  } else if (Serial.write(reinterpret_cast<const uint8_t*>(line),
#endif
                          static_cast<size_t>(length)) != static_cast<size_t>(length)) {
    ++usbLogShortWrites;
  }
  // HWCDC can also discard queued bytes after accepting a complete write.
  // The host must validate line syntax, sequence and END; return counts alone
  // cannot prove delivery. No retry loop or logging on the receiver task.
#if !defined(SENSOR_DUAL_PROBE)
  vTaskDelay(pdMS_TO_TICKS(5));
#endif
}

void onUartError(hardwareSerial_error_t error) {
  portENTER_CRITICAL(&errorMux);
  switch (error) {
    case UART_BUFFER_FULL_ERROR: ++uartErrors.bufferFull; break;
    case UART_FIFO_OVF_ERROR: ++uartErrors.fifoOverflow; break;
    case UART_FRAME_ERROR: ++uartErrors.framing; break;
    case UART_PARITY_ERROR: ++uartErrors.parity; break;
    case UART_BREAK_ERROR: ++uartErrors.breaks; break;
    default: break;
  }
  portEXIT_CRITICAL(&errorMux);
}

void receiveTask(void*) {
  Ld2410cParser parser;
  for (;;) {
    const uint32_t now = millis();
    // Bound each pass even if the candidate line contains continuous noise.
    // The independent receiver keeps draining UART during display/network work.
    for (size_t count = 0; count < 256 && Serial1.available() > 0; ++count) {
      const int byte = Serial1.read();
      if (byte >= 0) parser.feed(static_cast<uint8_t>(byte), now);
    }
    parser.expire(now);
    Snapshot value;
    value.sample = parser.latest();
    value.counters = parser.counters();
    value.hasSample = parser.hasSample();
    xQueueOverwrite(snapshots, &value);
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void reportTask(void*) {
  for (;;) {
    serviceRadarDiagnostics(millis());
    // USB CDC writes may wait for the host. Keep them off both the UART
    // receiver and the display/touch dispatch loop.
    vTaskDelay(pdMS_TO_TICKS(kReportMs));
  }
}
}  // namespace

void beginRadarDiagnostics() {
  if (started) return;
  Serial.println("RADAR: PROBE=CANDIDATE_RX_ONLY UART=1 RX_GPIO=44 TX=DISABLED BAUD=256000 FORMAT=8N1");
  // Fail instead of inheriting a UART previously configured by another feature.
  if (uart_get_RxPin(1) >= 0 || uart_get_TxPin(1) >= 0) {
    Serial.println("RADAR: INIT_FAILED UART_ALREADY_ASSIGNED");
    return;
  }
  snapshots = xQueueCreate(1, sizeof(Snapshot));
  if (snapshots == nullptr) {
    Serial.println("RADAR: INIT_FAILED QUEUE_ALLOCATION");
    return;
  }
  Serial1.setRxBufferSize(kRxBufferBytes);
  Serial1.onReceiveError(onUartError);
  // In Arduino-ESP32 2.0.17 an explicit RX with TX=-1 does not select TX1's
  // arbitrary default pin. Verify that invariant after initialization as well.
  Serial1.begin(kBaud, SERIAL_8N1, kCandidateRx, -1);
  if (!Serial1 || uart_get_RxPin(1) != kCandidateRx || uart_get_TxPin(1) != -1) {
    Serial1.end();
    vQueueDelete(snapshots); snapshots = nullptr;
    Serial.println("RADAR: INIT_FAILED RX_ONLY_INVARIANT");
    return;
  }
  // Lower priority than the existing touch sampler; no USB logging on this task.
  if (xTaskCreatePinnedToCore(receiveTask, "radar-rx", 4096, nullptr, 1,
                            &worker, 0) != pdPASS) {
    Serial1.end();
    vQueueDelete(snapshots); snapshots = nullptr;
    Serial.println("RADAR: INIT_FAILED TASK_ALLOCATION");
    return;
  }
  started = true;
  lastReportMs = millis();
  Serial.println("RADAR: LISTENING CONNECTION_NOT_YET_VERIFIED");
  // Keep this separate task on the core where setup() initialized USB and its
  // ISR. This reduces cross-core FIFO access; it is not proof of the loss cause.
  if (xTaskCreatePinnedToCore(reportTask, "radar-report", 4096, nullptr, 1,
                            &reporter, ARDUINO_RUNNING_CORE) != pdPASS) {
    Serial.println("RADAR: REPORTING_FAILED TASK_ALLOCATION");
  }
}

void serviceRadarDiagnostics(uint32_t nowMs) {
  // Enforce a single reporter owner even if another caller is added later.
  if (reporter == nullptr || xTaskGetCurrentTaskHandle() != reporter) return;
  if (!started || uint32_t(nowMs - lastReportMs) < kReportMs) return;
  Snapshot value;
  if (xQueuePeek(snapshots, &value, 0) != pdTRUE) return;
  // The receiver may publish after the caller sampled its loop timestamp.
  nowMs = millis();
  UartErrors errors;
  portENTER_CRITICAL(&errorMux);
  errors = uartErrors;
  portEXIT_CRITICAL(&errorMux);
  const uint32_t elapsed = nowMs - lastReportMs;
  const bool fresh = value.hasSample && uint32_t(nowMs - value.sample.receivedAtMs) <= 2000;
  const bool connected = fresh && value.counters.validFrames >= 3;
  const unsigned long sequence = static_cast<unsigned long>(++reportSequence);
  const unsigned reportLines = connected ? 8 : 5;
  reportLine("RADAR: BEGIN SEQ=%lu LINES=%u USB_LOG_SHORT_WRITES=%lu FORMAT_ERRORS=%lu\n",
             sequence, reportLines, (unsigned long)usbLogShortWrites,
             (unsigned long)formatErrors);
  reportLine("RADAR: %s UPTIME_MS=%lu SAMPLE_AGE_MS=%lu SEQ=%lu\n",
                connected ? "CONNECTED" : "NO_RECENT_VALID_DATA",
                (unsigned long)nowMs,
                (unsigned long)(value.hasSample ? uint32_t(nowMs - value.sample.receivedAtMs) : UINT32_MAX),
                sequence);
  if (connected) {
    const RadarSample& s = value.sample;
    reportLine("RADAR: PRESENCE=%s STATE=%u MODE=%s MOVING=%s STILL=%s SEQ=%lu\n",
                  s.presenceKnown ? (s.presence ? "YES" : "NO") : "UNKNOWN",
                  unsigned(s.targetState), s.dataType == 2 ? "NORMAL" : "ENGINEERING",
                  s.moving ? "YES" : "NO", s.stationary ? "YES" : "NO", sequence);
    reportLine("RADAR: MOVING_DISTANCE=%u cm MOVING_ENERGY=%u SEQ=%lu\n",
                  unsigned(s.movingDistanceCm), unsigned(s.movingEnergy), sequence);
    reportLine("RADAR: STILL_DISTANCE=%u cm STILL_ENERGY=%u DETECTION_DISTANCE=%u cm SEQ=%lu\n",
                  unsigned(s.stillDistanceCm), unsigned(s.stillEnergy), unsigned(s.detectionDistanceCm), sequence);
  }
  reportLine("RADAR: VALID_FRAMES=%lu INVALID_FRAMES=%lu UNSUPPORTED_FRAMES=%lu RX_BYTES=%lu DISCARDED_BYTES=%lu FPS=%.2f WINDOW_MS=%lu SEQ=%lu\n",
                (unsigned long)value.counters.validFrames, (unsigned long)value.counters.invalidFrames,
                (unsigned long)value.counters.unsupportedFrames, (unsigned long)value.counters.receivedBytes,
                (unsigned long)value.counters.discardedBytes,
                double(value.counters.validFrames - lastReportedFrames) * 1000.0 / elapsed,
                (unsigned long)elapsed, sequence);
  reportLine("RADAR: UART_BUFFER_FULL=%lu UART_FIFO_OVERFLOW=%lu UART_FRAME_ERRORS=%lu UART_PARITY_ERRORS=%lu UART_BREAKS=%lu SEQ=%lu\n",
                (unsigned long)errors.bufferFull, (unsigned long)errors.fifoOverflow,
                (unsigned long)errors.framing, (unsigned long)errors.parity, (unsigned long)errors.breaks, sequence);
  reportLine("RADAR: END SEQ=%lu LINES=%u USB_LOG_SHORT_WRITES=%lu FORMAT_ERRORS=%lu\n",
             sequence, reportLines, (unsigned long)usbLogShortWrites,
             (unsigned long)formatErrors);
  lastReportMs = nowMs;
  lastReportedFrames = value.counters.validFrames;
}
}  // namespace sensors
#else
namespace sensors {
void beginRadarDiagnostics() {}
void serviceRadarDiagnostics(uint32_t) {}
}  // namespace sensors
#endif
