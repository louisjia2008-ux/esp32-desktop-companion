#pragma once
#include "mock_runtime.h"

class TwoWire {
 public:
  explicit TwoWire(uint8_t controller) {
    if (controller != 1) throw std::runtime_error("service must use independent I2C1");
  }
  bool begin(int sda, int scl, uint32_t clock) {
    mock::hardwareCall();
    ++mock::wireBeginCalls;
    mock::wireSda = sda;
    mock::wireScl = scl;
    mock::wireClock = clock;
    mock::busInitialized = mock::wireBeginSucceeds;
    return mock::wireBeginSucceeds;
  }
  void setTimeOut(uint16_t timeout) { mock::hardwareCall(); mock::wireTimeout = timeout; }
  void beginTransmission(uint8_t address) {
    mock::hardwareCall();
    if (address != 0x62) throw std::runtime_error("unexpected I2C address");
    mock::probeTimes.push_back(mock::now);
  }
  uint8_t endTransmission() { mock::hardwareCall(); return mock::wireProbeResult; }
};
