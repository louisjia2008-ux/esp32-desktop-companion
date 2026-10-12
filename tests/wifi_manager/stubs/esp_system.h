#pragma once

#include <Arduino.h>

namespace fake_esp_system {

inline uint32_t randomState = 0x12345678U;

inline void reset() { randomState = 0x12345678U; }

}  // namespace fake_esp_system

inline uint32_t esp_random() {
  fake_esp_system::randomState =
      fake_esp_system::randomState * 1664525U + 1013904223U;
  return fake_esp_system::randomState;
}

