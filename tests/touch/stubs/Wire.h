#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class FakeWire {
 public:
  bool beginOk = true;
  size_t writeLength = 11;
  uint8_t endStatus = 0;
  size_t receiveLength = 8;
  size_t readIndex = 0;
  uint32_t endedTransactions = 0;
  std::vector<uint8_t> payload = std::vector<uint8_t>(8, 0);
  std::vector<uint8_t> command;

  bool begin(int, int, uint32_t) { return beginOk; }
  void setClock(uint32_t) {}
  void beginTransmission(uint8_t) {}
  size_t write(const uint8_t* data, size_t length) {
    command.assign(data, data + length);
    return writeLength;
  }
  uint8_t endTransmission() {
    ++endedTransactions;
    return endStatus;
  }
  size_t requestFrom(uint8_t, uint8_t) {
    readIndex = 0;
    return receiveLength;
  }
  int available() { return static_cast<int>(payload.size() - readIndex); }
  int read() { return readIndex < payload.size() ? payload[readIndex++] : -1; }
};

inline FakeWire Wire;
