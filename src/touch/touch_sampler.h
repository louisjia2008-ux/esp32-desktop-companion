#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "touch_driver.h"

namespace touch {

struct Sample {
  uint32_t sampledAtMs = 0;
  TouchPoint point{};
  TouchReadResult result = TouchReadResult::ERROR;
  TouchReadDiagnostics diagnostics{};
  bool gap = false;
};

struct SamplerStatistics {
  uint32_t samples = 0;
  uint32_t downSamples = 0;
  uint32_t readErrors = 0;
  uint32_t idleReports = 0;
  uint32_t queueOverflows = 0;
  uint32_t maximumGapMs = 0;
  uint32_t commandErrors = 0;
  uint32_t responseErrors = 0;
  uint32_t pointCountErrors = 0;
  uint32_t coordinateErrors = 0;
  TouchReadDiagnostics lastError{};
};

// Owns runtime I2C reads. Only timestamped samples cross to the UI task;
// neither the UI nor the display is accessed from this background task.
class Sampler {
 public:
  static constexpr uint32_t kPeriodMs = 10;
  // Retain more than the bounded 1.5 s network wait at the 10 ms cadence.
  static constexpr size_t kQueueCapacity = 256;

  bool begin(Axs15231bTouch& device);
  bool pop(Sample& sample);
  SamplerStatistics statistics();

 private:
  static void taskEntry(void* context);
  void run();

  Axs15231bTouch* device_ = nullptr;
  QueueHandle_t queue_ = nullptr;
  TaskHandle_t task_ = nullptr;
  portMUX_TYPE statisticsMutex_ = portMUX_INITIALIZER_UNLOCKED;
  SamplerStatistics statistics_{};
};

}  // namespace touch
