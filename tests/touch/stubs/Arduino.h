#pragma once
#include "../../wifi_manager/stubs/Arduino.h"

constexpr int INPUT = 1;
inline uint32_t micros() { return millis() * 1000U; }

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(address) (*reinterpret_cast<const uint8_t*>(address))
#endif
