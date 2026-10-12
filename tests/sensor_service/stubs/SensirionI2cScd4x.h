#pragma once
#include "Wire.h"

enum SCD4xSensorVariant { SCD4X_SENSOR_VARIANT_MASK = 0xF000,
                         SCD4X_SENSOR_VARIANT_SCD41 = 0x1000 };
enum { I2cAddressNack = 12, I2cDataNack = 13 };

// The stub exposes only the approved command allowlist. A service change that
// attempts calibration, factory reset, persist, or reset cannot compile here.
class SensirionI2cScd4x {
 public:
  void begin(TwoWire&, uint8_t address) {
    mock::hardwareCall();
    if (address != 0x62) throw std::runtime_error("unexpected SCD4x address");
  }
  int16_t stopPeriodicMeasurement() {
    mock::hardwareCall();
    mock::commands.push_back("stop");
    mock::stopTimes.push_back(mock::now);
    mock::advance(500);
    return 0;
  }
  int16_t getSerialNumber(uint64_t& serial) {
    mock::hardwareCall();
    mock::commands.push_back("serial");
    serial = 1;
    return 0;
  }
  int16_t getSensorVariant(SCD4xSensorVariant& variant) {
    mock::hardwareCall();
    mock::commands.push_back("variant");
    variant = static_cast<SCD4xSensorVariant>(mock::variant);
    return 0;
  }
  int16_t startPeriodicMeasurement() {
    mock::hardwareCall();
    mock::commands.push_back("start");
    mock::startAt = mock::now;
    mock::lastEmissionAt = mock::now;
    return 0;
  }
  int16_t getDataReadyStatus(bool& ready) {
    mock::hardwareCall();
    if (mock::readyError) {
      const int16_t result = mock::readyError();
      if (result != 0) return result;
    }
    ready = !mock::neverReady && uint32_t(mock::now - mock::lastEmissionAt) >= 5000;
    return 0;
  }
  int16_t readMeasurement(uint16_t& co2, float& temperature, float& humidity) {
    mock::hardwareCall();
    if (mock::measurementError) {
      const int16_t result = mock::measurementError();
      if (result != 0) return result;
    }
    ++mock::measurements;
    mock::lastEmissionAt = mock::now;
    co2 = mock::fixtureCo2;
    temperature = 23.0f;
    humidity = 45.0f;
    return 0;
  }
};
