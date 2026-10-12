#pragma once

#include <Arduino.h>

#include <array>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

enum wl_status_t : uint8_t {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6,
};

enum wifi_mode_t : uint8_t {
  WIFI_MODE_NULL = 0,
  WIFI_STA = 1,
  WIFI_AP = 2,
  WIFI_AP_STA = 3,
};

enum wifi_auth_mode_t : uint8_t {
  WIFI_AUTH_OPEN = 0,
  WIFI_AUTH_WEP = 1,
  WIFI_AUTH_WPA_PSK = 2,
  WIFI_AUTH_WPA2_PSK = 3,
  WIFI_AUTH_WPA_WPA2_PSK = 4,
  WIFI_AUTH_WPA2_ENTERPRISE = 5,
  WIFI_AUTH_WPA3_PSK = 6,
  WIFI_AUTH_WPA2_WPA3_PSK = 7,
};

using wifi_err_reason_t = uint8_t;

constexpr wifi_err_reason_t WIFI_REASON_UNSPECIFIED = 1;
constexpr wifi_err_reason_t WIFI_REASON_AUTH_FAIL = 2;
constexpr wifi_err_reason_t WIFI_REASON_ASSOC_FAIL = 3;
constexpr wifi_err_reason_t WIFI_REASON_BEACON_TIMEOUT = 4;
constexpr wifi_err_reason_t WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15;
constexpr wifi_err_reason_t WIFI_REASON_HANDSHAKE_TIMEOUT = 204;
constexpr wifi_err_reason_t WIFI_REASON_NO_AP_FOUND = 201;
constexpr wifi_err_reason_t WIFI_REASON_ASSOC_LEAVE = 8;

constexpr int16_t WIFI_SCAN_RUNNING = -1;
constexpr int16_t WIFI_SCAN_FAILED = -2;

enum class WiFiEvent_t : uint8_t {
  ARDUINO_EVENT_WIFI_STA_DISCONNECTED,
};

struct WiFiEventInfo_t {
  struct {
    uint8_t reason = 0;
  } wifi_sta_disconnected;
};

class IPAddress {
 public:
  IPAddress() = default;
  explicit IPAddress(const std::string& value) : value_(value) {}

  bool fromString(const char* source) {
    if (source == nullptr) {
      return false;
    }
    std::array<unsigned, 4> octets{};
    char trailing = '\0';
    if (std::sscanf(source, "%u.%u.%u.%u%c", &octets[0], &octets[1],
                    &octets[2], &octets[3], &trailing) != 4) {
      return false;
    }
    for (const unsigned octet : octets) {
      if (octet > 255) {
        return false;
      }
    }
    std::ostringstream stream;
    stream << octets[0] << '.' << octets[1] << '.' << octets[2] << '.'
           << octets[3];
    value_ = stream.str();
    return true;
  }

  String toString() const { return String(value_); }

 private:
  std::string value_ = "0.0.0.0";
};

namespace fake_wifi {

struct Network {
  std::string ssid;
  int32_t rssi = -65;
  wifi_auth_mode_t auth = WIFI_AUTH_WPA2_PSK;
};

struct BeginCall {
  std::string ssid;
  std::string password;
  uint32_t atMs = 0;
};

struct DisconnectCall {
  bool wifiOff = false;
  bool eraseAccessPoint = false;
  uint32_t atMs = 0;
};

inline wl_status_t stationStatus = WL_DISCONNECTED;
inline wifi_mode_t modeValue = WIFI_MODE_NULL;
inline bool autoReconnect = true;
inline bool softAPActive = false;
inline uint8_t softAPStations = 0;
inline bool softAPStartSucceeds = true;
inline std::string softAPSsid;
inline std::string softAPPassword;
inline std::string stationIp = "192.168.1.88";
inline std::string softAPIp = "192.168.4.1";
inline std::vector<Network> networks;
inline int16_t scanCompleteResult = WIFI_SCAN_FAILED;
inline int16_t scanStartResult = WIFI_SCAN_RUNNING;
inline size_t scanNetworksCalls = 0;
inline size_t scanDeleteCalls = 0;
inline size_t softAPDisconnectCalls = 0;
inline size_t setAutoReconnectCalls = 0;
inline std::vector<BeginCall> beginCalls;
inline std::vector<DisconnectCall> disconnectCalls;
inline std::vector<wl_status_t> scriptedStatuses;
inline size_t statusCalls = 0;
inline std::function<void(WiFiEvent_t, WiFiEventInfo_t)> eventHandler;

inline void reset() {
  stationStatus = WL_DISCONNECTED;
  modeValue = WIFI_MODE_NULL;
  autoReconnect = true;
  softAPActive = false;
  softAPStations = 0;
  softAPStartSucceeds = true;
  softAPSsid.clear();
  softAPPassword.clear();
  stationIp = "192.168.1.88";
  softAPIp = "192.168.4.1";
  networks.clear();
  scanCompleteResult = WIFI_SCAN_FAILED;
  scanStartResult = WIFI_SCAN_RUNNING;
  scanNetworksCalls = 0;
  scanDeleteCalls = 0;
  softAPDisconnectCalls = 0;
  setAutoReconnectCalls = 0;
  beginCalls.clear();
  disconnectCalls.clear();
  scriptedStatuses.clear();
  statusCalls = 0;
  eventHandler = nullptr;
}

inline void completeScan(std::vector<Network> results) {
  networks = std::move(results);
  scanCompleteResult = static_cast<int16_t>(networks.size());
}

inline void emitDisconnect(uint8_t reason) {
  if (eventHandler) {
    WiFiEventInfo_t info{};
    info.wifi_sta_disconnected.reason = reason;
    eventHandler(WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED, info);
  }
}

}  // namespace fake_wifi

class WiFiClass {
 public:
  template <typename Callback>
  void onEvent(Callback callback, WiFiEvent_t event) {
    (void)event;
    fake_wifi::eventHandler = callback;
  }

  void mode(wifi_mode_t value) { fake_wifi::modeValue = value; }

  wifi_mode_t getMode() const { return fake_wifi::modeValue; }

  void setAutoReconnect(bool enabled) {
    fake_wifi::autoReconnect = enabled;
    ++fake_wifi::setAutoReconnectCalls;
  }

  wl_status_t status() const {
    ++fake_wifi::statusCalls;
    if (!fake_wifi::scriptedStatuses.empty()) {
      fake_wifi::stationStatus = fake_wifi::scriptedStatuses.front();
      fake_wifi::scriptedStatuses.erase(fake_wifi::scriptedStatuses.begin());
    }
    return fake_wifi::stationStatus;
  }

  void begin(const char* ssid, const char* password) {
    fake_wifi::beginCalls.push_back(
        {ssid == nullptr ? "" : ssid, password == nullptr ? "" : password,
         millis()});
  }

  bool disconnect(bool wifiOff = false, bool eraseAccessPoint = false) {
    fake_wifi::disconnectCalls.push_back(
        {wifiOff, eraseAccessPoint, millis()});
    fake_wifi::stationStatus = WL_DISCONNECTED;
    if (wifiOff) {
      fake_wifi::modeValue = WIFI_MODE_NULL;
    }
    return true;
  }

  const char* disconnectReasonName(wifi_err_reason_t reason) const {
    (void)reason;
    return "fake reason";
  }

  IPAddress localIP() const { return IPAddress(fake_wifi::stationIp); }
  // Connection diagnostics logged on connect; values are not asserted.
  IPAddress gatewayIP() const { return IPAddress("192.168.4.254"); }
  String BSSIDstr() const { return String("00:00:00:00:00:00"); }
  int32_t channel() const { return 1; }
  int8_t RSSI() const { return -50; }

  bool softAP(const char* ssid, const char* password) {
    fake_wifi::softAPSsid = ssid == nullptr ? "" : ssid;
    fake_wifi::softAPPassword = password == nullptr ? "" : password;
    fake_wifi::softAPActive = fake_wifi::softAPStartSucceeds;
    return fake_wifi::softAPStartSucceeds;
  }

  bool softAPdisconnect(bool wifiOff) {
    (void)wifiOff;
    ++fake_wifi::softAPDisconnectCalls;
    fake_wifi::softAPActive = false;
    fake_wifi::softAPStations = 0;
    return true;
  }

  IPAddress softAPIP() const { return IPAddress(fake_wifi::softAPIp); }

  uint8_t softAPgetStationNum() const { return fake_wifi::softAPStations; }

  int16_t scanNetworks(bool async, bool showHidden, bool passive,
                       uint32_t maxMsPerChannel) {
    (void)async;
    (void)showHidden;
    (void)passive;
    (void)maxMsPerChannel;
    ++fake_wifi::scanNetworksCalls;
    if (fake_wifi::scanStartResult == WIFI_SCAN_RUNNING) {
      fake_wifi::scanCompleteResult = WIFI_SCAN_RUNNING;
    } else if (fake_wifi::scanStartResult >= 0) {
      fake_wifi::scanCompleteResult = fake_wifi::scanStartResult;
    }
    return fake_wifi::scanStartResult;
  }

  int16_t scanComplete() const { return fake_wifi::scanCompleteResult; }

  void scanDelete() {
    ++fake_wifi::scanDeleteCalls;
    fake_wifi::networks.clear();
    fake_wifi::scanCompleteResult = WIFI_SCAN_FAILED;
  }

  String SSID(uint8_t index) const {
    if (index >= fake_wifi::networks.size()) {
      return String();
    }
    return String(fake_wifi::networks[index].ssid);
  }

  int32_t RSSI(uint8_t index) const {
    return index >= fake_wifi::networks.size()
               ? -127
               : fake_wifi::networks[index].rssi;
  }

  wifi_auth_mode_t encryptionType(uint8_t index) const {
    return index >= fake_wifi::networks.size()
               ? WIFI_AUTH_WPA2_ENTERPRISE
               : fake_wifi::networks[index].auth;
  }
};

inline WiFiClass WiFi;
