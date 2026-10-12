#pragma once

#include <Arduino.h>

namespace board {

// The controller remains configured with its verified native portrait panel
// geometry. Phase 1 presents the entire product in rotation-1 landscape.
constexpr int16_t kPanelWidth = 320;
constexpr int16_t kPanelHeight = 480;
constexpr int16_t kDisplayWidth = 480;
constexpr int16_t kDisplayHeight = 320;
constexpr uint8_t kDisplayRotation = 1;

// AXS15231B QSPI display bus.
constexpr int8_t kLcdCs = 45;
constexpr int8_t kLcdClock = 47;
constexpr int8_t kLcdData0 = 21;
constexpr int8_t kLcdData1 = 48;
constexpr int8_t kLcdData2 = 40;
constexpr int8_t kLcdData3 = 39;
constexpr int8_t kLcdTearingEffect = 38;
constexpr int8_t kLcdBacklight = 1;
constexpr int8_t kLcdReset = -1;

// AXS15231B capacitive touch interface.
constexpr int8_t kTouchSda = 4;
constexpr int8_t kTouchScl = 8;
constexpr int8_t kTouchInterrupt = 3;
constexpr uint8_t kTouchAddress = 0x3B;
constexpr uint32_t kTouchI2cFrequency = 400000;

// Manufacturer DEMO_PIC/pincfg.h: dedicated 1-bit SD_MMC bus. Do not
// enable 4-bit mode; the other data lines are used by the QSPI display.
constexpr int8_t kSdClock = 12;
constexpr int8_t kSdCommand = 11;
constexpr int8_t kSdData0 = 13;

// The board's BOOT button is wired active-low on GPIO0. A five-second hold is
// required before firmware settings are erased.
constexpr int8_t kBootButton = 0;

}  // namespace board
