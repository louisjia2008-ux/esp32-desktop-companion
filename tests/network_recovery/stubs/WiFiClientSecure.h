#pragma once
#include "WiFi.h"
class WiFiClientSecure : public WiFiClient {
 public:
  void setCACert(const char*) {}
  void setCACertBundle(const uint8_t*) {}
  void setInsecure() {}
  bool verify(const char*, const char*) { return true; }
};
