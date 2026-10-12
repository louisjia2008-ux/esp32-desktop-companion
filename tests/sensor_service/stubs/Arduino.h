#pragma once
#include "mock_runtime.h"

#define ARDUINO_RUNNING_CORE 1
#define pdPASS 1
#define pdMS_TO_TICKS(value) (value)
#define portMUX_INITIALIZER_UNLOCKED 0
using portMUX_TYPE = int;
using TaskHandle_t = void*;
using UBaseType_t = unsigned;

inline void portENTER_CRITICAL(portMUX_TYPE*) { ++mock::criticalDepth; }
inline void portEXIT_CRITICAL(portMUX_TYPE*) { --mock::criticalDepth; }
inline uint32_t millis() { return mock::now; }
inline void vTaskDelay(uint32_t ticks) { mock::advance(ticks); }
inline void vTaskDelete(void*) { throw mock::Stop(); }
inline UBaseType_t uxTaskGetStackHighWaterMark(void*) { return 2048; }
inline int xTaskCreatePinnedToCore(mock::TaskFunction function, const char* name,
                                   uint32_t stack, void*, unsigned,
                                   TaskHandle_t*, int core) {
  mock::hardwareCall();
  if (mock::failTask == name) return 0;
  mock::tasks.push_back({function, name, stack, core});
  return pdPASS;
}

// Any future accidental logging from a sampling task fails the host harness.
class ForbiddenUsb {
 public:
  template <typename... Args> void println(Args...) {
    throw std::runtime_error("USB logging in sampling service");
  }
  template <typename... Args> void printf(Args...) {
    throw std::runtime_error("USB logging in sampling service");
  }
  template <typename... Args> size_t write(Args...) {
    throw std::runtime_error("USB logging in sampling service");
  }
};
extern ForbiddenUsb Serial;
