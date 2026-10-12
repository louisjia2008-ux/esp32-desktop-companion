#pragma once
#include "mock_runtime.h"
inline bool i2cIsInit(uint8_t controller) {
  mock::hardwareCall();
  if (controller != 1) throw std::runtime_error("touch controller was accessed");
  return mock::busInitialized;
}
