#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
struct SerialStub {
  int room = 512;
  size_t writes = 0;
  bool shortWrite = false;
  std::string output;
  int availableForWrite() const { return room; }
  size_t write(const uint8_t* data, size_t size) {
    ++writes;
    const size_t written = shortWrite ? size - 1 : size;
    output.append(reinterpret_cast<const char*>(data), written);
    return written;
  }
};
extern SerialStub Serial;
