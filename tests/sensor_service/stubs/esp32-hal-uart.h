#pragma once
#include "mock_runtime.h"
inline int8_t uart_get_RxPin(uint8_t controller) {
  mock::hardwareCall();
  if (controller != 1) throw std::runtime_error("unexpected UART controller");
  return mock::uartRx;
}
inline int8_t uart_get_TxPin(uint8_t controller) {
  mock::hardwareCall();
  if (controller != 1) throw std::runtime_error("unexpected UART controller");
  return mock::uartTx;
}
