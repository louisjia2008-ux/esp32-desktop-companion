#pragma once
#include "mock_runtime.h"

#define SERIAL_8N1 0x800001c
enum hardwareSerial_error_t {
  UART_NO_ERROR, UART_BREAK_ERROR, UART_BUFFER_FULL_ERROR, UART_FIFO_OVF_ERROR,
  UART_FRAME_ERROR, UART_PARITY_ERROR
};

class HardwareSerial {
 public:
  void setRxBufferSize(size_t size) { mock::hardwareCall(); mock::uartRxBuffer = size; }
  void onReceiveError(void (*callback)(hardwareSerial_error_t)) {
    mock::hardwareCall();
    mock::uartError = [callback](int error) { callback(hardwareSerial_error_t(error)); };
  }
  void begin(uint32_t baud, uint32_t format, int8_t rx, int8_t tx) {
    mock::hardwareCall();
    if (format != SERIAL_8N1) throw std::runtime_error("UART format changed");
    mock::uartBaud = baud;
    mock::uartRx = rx;
    mock::uartTx = tx;
    ++mock::uartBeginCalls;
  }
  void end() { mock::hardwareCall(); mock::uartRx = -1; mock::uartTx = -1; }
  explicit operator bool() const { return mock::uartValid; }
  int available() { mock::hardwareCall(); return int(mock::uartBytes.size()); }
  int read() {
    mock::hardwareCall();
    if (mock::uartBytes.empty()) return -1;
    const int byte = mock::uartBytes.front();
    mock::uartBytes.pop_front();
    return byte;
  }
};
extern HardwareSerial Serial1;
