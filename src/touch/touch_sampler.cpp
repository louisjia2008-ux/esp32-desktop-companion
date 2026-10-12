#include "touch_sampler.h"

namespace touch {

bool Sampler::begin(Axs15231bTouch& device) {
  if (task_ != nullptr) {
    return true;
  }
  queue_ = xQueueCreate(kQueueCapacity, sizeof(Sample));
  if (queue_ == nullptr) {
    return false;
  }
  device_ = &device;
  // Arduino's display/decode loop runs on core 1. Keep touch acquisition on
  // core 0 so a synchronous QSPI flush cannot hide a complete short tap.
  if (xTaskCreatePinnedToCore(taskEntry, "touch-sampler", 3072, this, 2,
                            &task_, 0) != pdPASS) {
    vQueueDelete(queue_);
    queue_ = nullptr;
    device_ = nullptr;
    task_ = nullptr;
    return false;
  }
  return true;
}

bool Sampler::pop(Sample& sample) {
  return queue_ != nullptr && xQueueReceive(queue_, &sample, 0) == pdTRUE;
}

SamplerStatistics Sampler::statistics() {
  portENTER_CRITICAL(&statisticsMutex_);
  const SamplerStatistics snapshot = statistics_;
  portEXIT_CRITICAL(&statisticsMutex_);
  return snapshot;
}

void Sampler::taskEntry(void* context) {
  static_cast<Sampler*>(context)->run();
}

void Sampler::run() {
  bool havePreviousSample = false;
  uint32_t previousSampleMs = 0;
  for (;;) {
    Sample sample;
    sample.result = device_->read(sample.point);
    const TouchReadDiagnostics diagnostics = device_->diagnostics();
    sample.diagnostics = diagnostics;
    sample.sampledAtMs = millis();

    portENTER_CRITICAL(&statisticsMutex_);
    ++statistics_.samples;
    if (sample.result == TouchReadResult::DOWN) {
      ++statistics_.downSamples;
    } else if (diagnostics.idleReport) {
      ++statistics_.idleReports;
    } else if (sample.result == TouchReadResult::ERROR) {
      ++statistics_.readErrors;
      statistics_.lastError = diagnostics;
      switch (diagnostics.error) {
        case TouchReadError::COMMAND: ++statistics_.commandErrors; break;
        case TouchReadError::RESPONSE: ++statistics_.responseErrors; break;
        case TouchReadError::POINT_COUNT: ++statistics_.pointCountErrors; break;
        case TouchReadError::COORDINATES: ++statistics_.coordinateErrors; break;
        case TouchReadError::NONE: break;
      }
    }
    if (havePreviousSample) {
      const uint32_t gapMs = sample.sampledAtMs - previousSampleMs;
      if (gapMs > statistics_.maximumGapMs) {
        statistics_.maximumGapMs = gapMs;
      }
    }
    portEXIT_CRITICAL(&statisticsMutex_);
    previousSampleMs = sample.sampledAtMs;
    havePreviousSample = true;

    if (xQueueSend(queue_, &sample, 0) != pdTRUE) {
      // A long main-loop stall must not turn an incomplete gesture into an
      // action. Discard the incomplete history and explicitly re-arm the UI's
      // release barrier before accepting samples after the gap.
      xQueueReset(queue_);
      sample.gap = true;
      xQueueSend(queue_, &sample, 0);
      portENTER_CRITICAL(&statisticsMutex_);
      ++statistics_.queueOverflows;
      portEXIT_CRITICAL(&statisticsMutex_);
    }
    vTaskDelay(pdMS_TO_TICKS(kPeriodMs));
  }
}

}  // namespace touch
