#include "diagnostic_log.h"
#if defined(SENSOR_DUAL_PROBE)
#include <Arduino.h>
#include <freertos/semphr.h>

namespace sensors {
namespace {
SemaphoreHandle_t outputMutex = nullptr;
}
bool beginDiagnosticLog() {
  if (outputMutex == nullptr) outputMutex = xSemaphoreCreateMutex();
  return outputMutex != nullptr;
}
size_t writeDiagnosticLine(const uint8_t* bytes, size_t length) {
  if (outputMutex == nullptr ||
      xSemaphoreTake(outputMutex, portMAX_DELAY) != pdTRUE) return 0;
  const size_t written = Serial.write(bytes, length);
  // One shared pacing interval, even when both sensors report together.
  // Never called by the UART receiver, touch sampler or main display loop.
  vTaskDelay(pdMS_TO_TICKS(5));
  xSemaphoreGive(outputMutex);
  return written;
}
}
#endif
