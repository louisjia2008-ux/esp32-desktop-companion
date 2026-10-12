#pragma once

#include <cstddef>

namespace fake_esp_wifi {

inline size_t scanStopCalls = 0;

inline void reset() { scanStopCalls = 0; }

}  // namespace fake_esp_wifi

inline int esp_wifi_scan_stop() {
  ++fake_esp_wifi::scanStopCalls;
  return 0;
}
