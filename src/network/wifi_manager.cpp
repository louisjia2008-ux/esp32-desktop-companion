#include "wifi_manager.h"

#include <WiFi.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <cstddef>
#include <cctype>
#include <cstring>

#include "board_config.h"

namespace {

constexpr char kPreferencesNamespace[] = "devcomp";
constexpr char kConfigurationKey[] = "config_v1";
constexpr char kPairingKeyKey[] = "pair_key";
constexpr uint32_t kConfigurationMagic = 0x31434644U;
constexpr uint16_t kConfigurationVersion = 1;

struct StoredConfiguration {
  uint32_t magic = kConfigurationMagic;
  uint16_t version = kConfigurationVersion;
  uint16_t bridgePort = 8765;
  char ssid[33] = {};
  char password[65] = {};
  char bridgeHost[16] = {};
  uint32_t checksum = 0;
};

uint32_t configurationChecksum(const StoredConfiguration& configuration) {
  const uint8_t* bytes =
      reinterpret_cast<const uint8_t*>(&configuration);
  uint32_t checksum = 2166136261U;
  for (size_t index = 0; index < offsetof(StoredConfiguration, checksum);
       ++index) {
    checksum ^= bytes[index];
    checksum *= 16777619U;
  }
  return checksum;
}

bool hasTerminator(const char* value, size_t capacity) {
  return memchr(value, '\0', capacity) != nullptr;
}

bool validStoredConfiguration(const StoredConfiguration& configuration) {
  return configuration.magic == kConfigurationMagic &&
         configuration.version == kConfigurationVersion &&
         configuration.bridgePort > 0 &&
         hasTerminator(configuration.ssid, sizeof(configuration.ssid)) &&
         hasTerminator(configuration.password,
                       sizeof(configuration.password)) &&
         hasTerminator(configuration.bridgeHost,
                       sizeof(configuration.bridgeHost)) &&
         configuration.checksum == configurationChecksum(configuration);
}

// Accepts the 32 hex digits printed by `bridge.py pair`, ignoring case and
// any spaces, dashes or colons used to group them.
bool parsePairingCode(const String& input, uint8_t* key, size_t keyBytes) {
  size_t digits = 0;
  for (size_t index = 0; index < input.length(); ++index) {
    const char character = input[index];
    if (character == ' ' || character == '-' || character == ':') continue;
    if (!std::isxdigit(static_cast<unsigned char>(character)) ||
        digits >= keyBytes * 2) {
      return false;
    }
    const uint8_t nibble = static_cast<uint8_t>(
        character <= '9' ? character - '0' : (character | 0x20) - 'a' + 10);
    key[digits / 2] = static_cast<uint8_t>(
        digits % 2 == 0 ? nibble << 4 : key[digits / 2] | nibble);
    ++digits;
  }
  return digits == keyBytes * 2;
}

void fillRandomCharacters(char* destination, size_t length,
                          const char* alphabet) {
  const size_t alphabetLength = strlen(alphabet);
  for (size_t index = 0; index < length; ++index) {
    destination[index] = alphabet[esp_random() % alphabetLength];
  }
  destination[length] = '\0';
}

String htmlEscape(const String& source) {
  String result;
  result.reserve(source.length() + 8);
  for (size_t index = 0; index < source.length(); ++index) {
    switch (source[index]) {
      case '&':
        result += F("&amp;");
        break;
      case '<':
        result += F("&lt;");
        break;
      case '>':
        result += F("&gt;");
        break;
      case '\"':
        result += F("&quot;");
        break;
      case '\'':
        result += F("&#39;");
        break;
      default:
        result += source[index];
        break;
    }
  }
  return result;
}

bool validWifiPassword(const String& password) {
  if (password.isEmpty()) {
    return true;
  }
  if (password.length() >= 8 && password.length() <= 63) {
    for (size_t index = 0; index < password.length(); ++index) {
      const uint8_t value = static_cast<uint8_t>(password[index]);
      if (value < 32 || value > 126) {
        return false;
      }
    }
    return true;
  }
  if (password.length() != 64) {
    return false;
  }
  for (size_t index = 0; index < password.length(); ++index) {
    if (!std::isxdigit(static_cast<unsigned char>(password[index]))) {
      return false;
    }
  }
  return true;
}

bool supportedPersonalAuth(wifi_auth_mode_t authMode) {
  return authMode == WIFI_AUTH_OPEN || authMode == WIFI_AUTH_WPA2_PSK ||
         authMode == WIFI_AUTH_WPA_WPA2_PSK ||
         authMode == WIFI_AUTH_WPA3_PSK ||
         authMode == WIFI_AUTH_WPA2_WPA3_PSK;
}

bool parsePort(const String& source, uint16_t& port) {
  if (source.isEmpty()) {
    return false;
  }
  uint32_t value = 0;
  for (size_t index = 0; index < source.length(); ++index) {
    const char digit = source[index];
    if (digit < '0' || digit > '9') {
      return false;
    }
    value = value * 10U + static_cast<uint32_t>(digit - '0');
    if (value > 65535U) {
      return false;
    }
  }
  if (value == 0) {
    return false;
  }
  port = static_cast<uint16_t>(value);
  return true;
}

bool parseNetworkIndex(const String& source, int16_t scanCount,
                       uint8_t& networkIndex) {
  if (source.isEmpty() || scanCount <= 0) {
    return false;
  }
  uint32_t value = 0;
  for (size_t index = 0; index < source.length(); ++index) {
    const char digit = source[index];
    if (digit < '0' || digit > '9') {
      return false;
    }
    value = value * 10U + static_cast<uint32_t>(digit - '0');
    if (value >= 255U) {
      return false;
    }
  }
  if (value >= static_cast<uint32_t>(scanCount)) {
    return false;
  }
  networkIndex = static_cast<uint8_t>(value);
  return true;
}

const char* signalLabel(int32_t rssi) {
  if (rssi >= -60) {
    return "Strong";
  }
  if (rssi >= -72) {
    return "Good";
  }
  return "Weak";
}

}  // namespace

#if defined(SENSOR_DUAL_PROBE) || defined(SENSOR_SERVICE_ENABLED)
WifiManager::EventCounts WifiManager::eventCounts() const {
  portENTER_CRITICAL(&eventMux_);
  const EventCounts snapshot = eventCounts_;
  portEXIT_CRITICAL(&eventMux_);
  return snapshot;
}
#endif

void WifiManager::begin(uint32_t nowMs) {
  pinMode(board::kBootButton, INPUT_PULLUP);
#if defined(SENSOR_DUAL_PROBE) || defined(SENSOR_SERVICE_ENABLED)
  WiFi.onEvent(
      [this](WiFiEvent_t, WiFiEventInfo_t) {
        portENTER_CRITICAL(&eventMux_);
        ++eventCounts_.connections;
        // Duplicate CONNECTED notifications cannot inflate reconnections.
        if (everEventConnected_ && !eventConnected_) ++eventCounts_.reconnections;
        eventConnected_ = true;
        everEventConnected_ = true;
        portEXIT_CRITICAL(&eventMux_);
      },
      WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);
#endif
  WiFi.onEvent(
      [this](WiFiEvent_t, WiFiEventInfo_t info) {
#if defined(SENSOR_DUAL_PROBE) || defined(SENSOR_SERVICE_ENABLED)
        portENTER_CRITICAL(&eventMux_);
        ++eventCounts_.disconnections;
        eventConnected_ = false;
        portEXIT_CRITICAL(&eventMux_);
#endif
        pendingDisconnectReason_ = info.wifi_sta_disconnected.reason;
        disconnectReasonPending_ = true;
      },
      WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  preferencesReady_ = preferences_.begin(kPreferencesNamespace, false);
  if (!preferencesReady_) {
    Serial.println("Wi-Fi preferences unavailable");
  }
  loadConfiguration();
  loadPairingKey();
  configureSetupServer();

  if (hasCredentials_) {
    firstConnectionStartedMs_ = nowMs;
    beginStationAttempt(nowMs);
  } else {
    enterSetupMode();
  }
}

void WifiManager::loop(uint32_t nowMs) {
  // Observe existing manager state without extra radio queries or callbacks.
  const bool connectionObserved = connected();
  if (connectionObserved != observedConnected_) {
    observedConnected_ = connectionObserved;
    if (connectionObserved) ++observedConnections_;
    else ++observedDisconnections_;
  }
  if (disconnectReasonPending_) {
    const uint8_t reason = pendingDisconnectReason_;
    disconnectReasonPending_ = false;
    if (reason != WIFI_REASON_ASSOC_LEAVE) {
      lastDisconnectReason_ = reason;
    }
    Serial.printf("Wi-Fi station disconnected reason=%u (%s)\n",
                  static_cast<unsigned>(reason),
                  WiFi.disconnectReasonName(
                      static_cast<wifi_err_reason_t>(reason)));
  }
  pollResetButton(nowMs);
  if (restartPending_) {
    if (static_cast<int32_t>(nowMs - restartAtMs_) >= 0) {
      ESP.restart();
    }
    return;
  }

  if (state_ == State::SETUP) {
    if (networkScanRunning_) {
      const int16_t scanCount = WiFi.scanComplete();
      if (scanCount >= 0) {
        Serial.printf(
            "Wi-Fi 2.4 GHz network scan complete count=%d elapsed_ms=%lu\n",
            static_cast<int>(scanCount),
            static_cast<unsigned long>(millis() - networkScanStartedMs_));
        networkScanRunning_ = false;
      } else if (scanCount == WIFI_SCAN_FAILED) {
        // scanComplete() can report its deadline before the IDF scan stops.
        // Stop the radio scan explicitly before another connection attempt.
        esp_wifi_scan_stop();
        Serial.printf("Wi-Fi 2.4 GHz network scan failed elapsed_ms=%lu\n",
                      static_cast<unsigned long>(millis() -
                                                 networkScanStartedMs_));
        networkScanRunning_ = false;
      }
    }
    if (networkScanRequested_ && !networkScanRunning_ &&
        !setupStationAttemptRunning_) {
      startNetworkScan();
    }
    setupServer_.handleClient();
    if (manualSetup_ && static_cast<int32_t>(nowMs - manualSetupUntilMs_) >= 0) {
      manualSetup_ = false;
      Serial.println("Manual setup window ended; reconnecting saved Wi-Fi");
    }
    if (!restartPending_ && !manualSetup_) {
      loopSetupConnection(nowMs);
    }
    return;
  }

  const wl_status_t wifiStatus = WiFi.status();
  if (state_ == State::CONNECTED) {
    if (wifiStatus != WL_CONNECTED) {
      Serial.println("Wi-Fi disconnected; retry scheduled");
      scheduleRetry(nowMs);
    }
    return;
  }

  if (wifiStatus == WL_CONNECTED) {
    markConnected(false);
    return;
  }

  if (state_ == State::CONNECTING &&
      nowMs - attemptStartedMs_ >= kStationAttemptMs) {
    if (!everConnected_ &&
        nowMs - firstConnectionStartedMs_ >= kInitialSetupTimeoutMs) {
      Serial.println("Wi-Fi startup connection timed out; entering setup mode");
      initialConnectionFailed_ = true;
      enterSetupMode();
    } else {
      scheduleRetry(nowMs);
    }
  } else if (state_ == State::RETRY_WAIT &&
             static_cast<int32_t>(nowMs - nextRetryMs_) >= 0) {
    beginStationAttempt(nowMs);
  }
}

const char* WifiManager::lastConnectionFailure() const {
  switch (lastDisconnectReason_) {
    case WIFI_REASON_NO_AP_FOUND:
      return "NETWORK NOT FOUND (2.4 GHz only)";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "AUTHENTICATION FAILED";
    case WIFI_REASON_ASSOC_FAIL:
      return "WI-FI ASSOCIATION FAILED";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "WI-FI SIGNAL LOST";
    default:
      return lastDisconnectReason_ != 0 || initialConnectionFailed_
                 ? "WI-FI CONNECTION TIMED OUT"
                 : "";
  }
}

const char* WifiManager::lastConnectionFailureChinese() const {
  switch (lastDisconnectReason_) {
    case WIFI_REASON_NO_AP_FOUND:
      return "未找到该网络：请选择列表中的 2.4 GHz Wi-Fi。";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "认证失败：请重新输入 Wi-Fi 密码。";
    case WIFI_REASON_ASSOC_FAIL:
      return "Wi-Fi 关联失败：请检查路由器兼容模式。";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "Wi-Fi 信号丢失：请靠近路由器。";
    default:
      return lastDisconnectReason_ != 0 || initialConnectionFailed_
                 ? "Wi-Fi 连接超时：请重新选择网络。"
                 : "";
  }
}

String WifiManager::localIp() const {
  if (!connected()) {
    return String();
  }
  return WiFi.localIP().toString();
}

void WifiManager::loadConfiguration() {
  if (!preferencesReady_) {
    hasCredentials_ = false;
    return;
  }
  const size_t storedLength = preferences_.getBytesLength(kConfigurationKey);
  if (storedLength > 0) {
    StoredConfiguration configuration{};
    if (storedLength != sizeof(configuration) ||
        preferences_.getBytes(kConfigurationKey, &configuration,
                              sizeof(configuration)) !=
            sizeof(configuration) ||
        !validStoredConfiguration(configuration)) {
      Serial.println(
          "Stored Wi-Fi configuration blob rejected: invalid version or checksum");
      hasCredentials_ = false;
      return;
    }
    ssid_ = configuration.ssid;
    password_ = configuration.password;
    bridgeHost_ = configuration.bridgeHost;
    bridgePort_ = configuration.bridgePort;
  } else {
    // One-time compatibility path for credentials written by firmware 0.2.0
    // and earlier. The next successful save writes one atomic blob.
    ssid_ = preferences_.isKey("ssid") ? preferences_.getString("ssid") : "";
    password_ =
        preferences_.isKey("pass") ? preferences_.getString("pass") : "";
    bridgeHost_ = preferences_.isKey("bridge_host")
                      ? preferences_.getString("bridge_host")
                      : "";
    const uint32_t storedPort = preferences_.isKey("bridge_port")
                                    ? preferences_.getUInt("bridge_port")
                                    : 8765;
    bridgePort_ = storedPort > 0 && storedPort <= 65535
                      ? static_cast<uint16_t>(storedPort)
                      : 8765;
  }
  IPAddress storedBridgeAddress;
  if (bridgeHost_.isEmpty() ||
      !storedBridgeAddress.fromString(bridgeHost_.c_str())) {
    Serial.println("Stored Bridge host rejected: expected IPv4 address");
    bridgeHost_.clear();
  }
  hasCredentials_ = !ssid_.isEmpty() && !bridgeHost_.isEmpty();
}

void WifiManager::loadPairingKey() {
  hasPairingKey_ = preferencesReady_ &&
      preferences_.getBytesLength(kPairingKeyKey) == kPairingKeyBytes &&
      preferences_.getBytes(kPairingKeyKey, pairingKey_, kPairingKeyBytes) ==
          kPairingKeyBytes;
  Serial.printf("Bridge pairing key: %s\n", hasPairingKey_ ? "stored" : "none");
}

void WifiManager::beginStationAttempt(uint32_t nowMs) {
  WiFi.scanDelete();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.begin(ssid_.c_str(), password_.c_str());
  state_ = State::CONNECTING;
  attemptStartedMs_ = nowMs;
  Serial.printf("Wi-Fi connection attempt %u\n",
                static_cast<unsigned>(retryCount_ + 1));
}

void WifiManager::markConnected(bool closeSetup) {
  if (closeSetup) {
    setupServer_.stop();
    WiFi.softAPdisconnect(true);
    if ((WiFi.getMode() & WIFI_AP) != 0) {
      WiFi.mode(WIFI_STA);
    }
    setupStationAttemptRunning_ = false;
    networkScanRequested_ = false;
    WiFi.scanDelete();
    Serial.println((WiFi.getMode() & WIFI_AP) != 0
                       ? "Saved Wi-Fi recovered; setup hotspot shutdown failed"
                       : "Saved Wi-Fi recovered; setup hotspot closed");
  }
  state_ = State::CONNECTED;
  everConnected_ = true;
  initialConnectionFailed_ = false;
  lastDisconnectReason_ = 0;
  retryCount_ = 0;
  Serial.printf("Wi-Fi connected IP=%s gateway=%s bssid=%s channel=%ld rssi=%d\n",
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(), WiFi.BSSIDstr().c_str(),
                static_cast<long>(WiFi.channel()), WiFi.RSSI());
}

void WifiManager::scheduleSetupRetry(uint32_t nowMs) {
  setupStationAttemptRunning_ = false;
  nextRetryMs_ = nowMs + kSetupRetryDelayMs;
}

void WifiManager::loopSetupConnection(uint32_t nowMs) {
  if (!hasCredentials_) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    markConnected(true);
    return;
  }
  if (setupStationAttemptRunning_) {
    if (nowMs - attemptStartedMs_ >= kStationAttemptMs) {
      // GOT_IP is delivered asynchronously. Recheck at the timeout boundary
      // before cancelling an attempt that may just have succeeded.
      if (WiFi.status() == WL_CONNECTED) {
        markConnected(true);
        return;
      }
      // Stop this station attempt without erasing credentials or turning off
      // the setup AP. A scan and a connection attempt must not overlap.
      WiFi.disconnect(false, false);
      scheduleSetupRetry(nowMs);
      Serial.println("Saved Wi-Fi still unavailable; setup retry scheduled");
    }
    return;
  }
  // Keep the AP available even while a phone is attached. Requiring the
  // phone to leave would turn background recovery into another stuck state.
  // handleClient() has already completed the current response before this.
  if (networkScanRunning_ ||
      static_cast<int32_t>(nowMs - nextRetryMs_) < 0) {
    return;
  }
  WiFi.setAutoReconnect(false);
  WiFi.begin(ssid_.c_str(), password_.c_str());
  attemptStartedMs_ = nowMs;
  setupStationAttemptRunning_ = true;
  Serial.println("Setup mode: retrying saved Wi-Fi with hotspot available");
}

void WifiManager::scheduleRetry(uint32_t nowMs) {
  if (retryCount_ < 10) {
    ++retryCount_;
  }
  state_ = State::RETRY_WAIT;
  nextRetryMs_ = nowMs + retryDelayMs();
}

uint32_t WifiManager::retryDelayMs() const {
  const uint8_t shift = retryCount_ > 5 ? 5 : retryCount_;
  const uint32_t delayMs = 1000UL << shift;
  return delayMs > kMaximumRetryDelayMs ? kMaximumRetryDelayMs : delayMs;
}

void WifiManager::enterSetupMode() {
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_AP_STA);
  const uint64_t chipId = ESP.getEfuseMac();
  snprintf(accessPointName_, sizeof(accessPointName_), "DevCompanion-%04X",
           static_cast<unsigned>(chipId & 0xFFFFU));
  generateSetupSecrets();
  if (!WiFi.softAP(accessPointName_, accessPointPassword_)) {
    Serial.println("Wi-Fi setup AP failed to start");
  } else {
    Serial.printf("Wi-Fi setup mode AP=%s IP=%s\n", accessPointName_,
                  WiFi.softAPIP().toString().c_str());
  }
  setupServer_.begin();
  state_ = State::SETUP;
  scheduleSetupRetry(millis());
  startNetworkScan();
}

void WifiManager::generateSetupSecrets() {
  fillRandomCharacters(accessPointPassword_, 12,
                       "ABCDEFGHJKLMNPQRSTUVWXYZ23456789");
  fillRandomCharacters(setupCsrfToken_, 32, "0123456789abcdef");
}

void WifiManager::configureSetupServer() {
  if (setupServerConfigured_) {
    return;
  }
  setupServer_.on("/", HTTP_GET, [this]() { handleSetupPage(); });
  setupServer_.on("/scan", HTTP_GET, [this]() { handleNetworkScan(); });
  setupServer_.on("/save", HTTP_POST, [this]() { handleSave(); });
  setupServer_.onNotFound([this]() {
    setupServer_.sendHeader("Location", "/", true);
    setupServer_.send(302, "text/plain", "");
  });
  setupServerConfigured_ = true;
}

void WifiManager::startNetworkScan() {
  if (networkScanRunning_) {
    return;
  }
  if (setupStationAttemptRunning_) {
    networkScanRequested_ = true;
    Serial.println("Wi-Fi network scan queued until station attempt ends");
    return;
  }
  networkScanRequested_ = false;
  WiFi.scanDelete();
  networkScanStartedMs_ = millis();
  Serial.println("Wi-Fi 2.4 GHz network scan started");
  // A complete scan on this board takes about eight seconds in a busy radio
  // environment. A 750 ms per-channel ceiling gives the async core a 15 s
  // completion window while the UI and setup server remain responsive.
  const int16_t result = WiFi.scanNetworks(true, false, false, 750);
  if (result == WIFI_SCAN_RUNNING) {
    networkScanRunning_ = true;
  } else if (result >= 0) {
    Serial.printf("Wi-Fi 2.4 GHz network scan complete count=%d elapsed_ms=0\n",
                  static_cast<int>(result));
  } else {
    Serial.printf("Wi-Fi 2.4 GHz network scan could not start result=%d\n",
                  static_cast<int>(result));
  }
}

void WifiManager::sendSetupHtml(const String& page) {
  setupServer_.sendHeader("Cache-Control", "no-store");
  setupServer_.sendHeader("X-Content-Type-Options", "nosniff");
  setupServer_.sendHeader(
      "Content-Security-Policy",
      "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; "
      "base-uri 'none'; frame-ancestors 'none'");
  setupServer_.send(200, "text/html; charset=utf-8", page);
}

void WifiManager::handleSetupPage() {
  const int16_t scanCount = WiFi.scanComplete();
  const bool scanPending = networkScanRunning_ || networkScanRequested_ ||
                           scanCount == WIFI_SCAN_RUNNING;
  String page;
  page.reserve(12000);
  page += F("<!doctype html><html><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  if (scanPending) {
    page += F("<meta http-equiv='refresh' content='2'>");
  }
  page += F("<title>Dev Companion Setup</title><style>");
  page += F("*{box-sizing:border-box}body{font-family:-apple-system,BlinkMacSystemFont,system-ui;background:#07101f;color:#eef;margin:0;padding:20px}");
  page += F("main{max-width:480px;margin:auto}h1{margin:8px 0}.lead{color:#b9c6dc;line-height:1.5}.notice,.error{padding:13px 14px;border-radius:12px;margin:16px 0}.notice{background:#10253b;border:1px solid #285173}.error{background:#36191d;border:1px solid #a84d59;color:#ffd9dd}.networks{border:0;padding:0;margin:22px 0 0;min-width:0}.networks legend{font-weight:700;margin-bottom:8px}.network{display:flex;align-items:center;gap:12px;margin:8px 0;padding:12px;border:1px solid #354968;border-radius:12px;background:#101b30}.network input{width:20px;height:20px;margin:0;flex:0 0 20px}.network span{min-width:0}.network strong{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.network small{color:#9fb0c9}.field{display:block;margin-top:18px}.field input{width:100%;padding:13px;margin-top:6px;border-radius:10px;border:1px solid #526;background:#101b30;color:#fff;font-size:16px}.actions{display:flex;gap:10px;align-items:center;margin-top:10px}.refresh{color:#75d8ff}.submit{width:100%;margin-top:24px;padding:14px;border:0;border-radius:10px;background:#5dd1ff;color:#06101c;font-weight:800;font-size:16px}</style></head><body><main>");
  page += F("<h1>Dev Companion</h1><p class='lead'>Select a Wi-Fi network the ESP32 can actually see, then configure the Mac Bridge.<br>请选择 ESP32 实际可见的 Wi-Fi，再配置 Mac Bridge。</p>");
  page += F("<div class='notice'><strong>2.4 GHz only / 仅支持 2.4 GHz</strong><br>WPA2 or WPA2/WPA3 Personal is recommended. Enterprise and captive-portal networks are unsupported.</div>");
  if (hasCredentials_) {
    page += F("<div class='notice'>Saved settings are still on this device. After a failed attempt it waits 30 seconds before retrying the saved Wi-Fi; this hotspot closes only after Wi-Fi connects.<br>原配置仍保存在设备里，每次失败后等待 30 秒再试原网络；连接成功后配置热点才会关闭。</div>");
  }
  const char* failure = lastConnectionFailure();
  if (failure[0] != '\0') {
    page += F("<div class='error'><strong>Last attempt failed / 上次连接失败</strong><br>");
    page += failure;
    page += F("<br>");
    page += lastConnectionFailureChinese();
    page += F("</div>");
  }
  if (scanPending) {
    page += networkScanRequested_
                ? F("<div class='notice'><strong>Scan queued / 扫描已排队</strong><br>Waiting for the current connection attempt to finish. / 当前连接尝试结束后开始扫描。<br>")
                : F("<div class='notice'><strong>Scanning 2.4 GHz networks… / 正在扫描…</strong><br>");
    page += F("This page refreshes when the list is ready. No settings have been submitted.</div></main></body></html>");
    sendSetupHtml(page);
    return;
  }
  page += F("<form method='post' action='/save'><fieldset class='networks'><legend>Visible networks / 可见网络</legend>");
  if (scanCount > 0) {
    String shownSsids[24];
    size_t shownCount = 0;
    bool savedNetworkSelected = false;
    for (int16_t index = 0; index < scanCount && shownCount < 24; ++index) {
      const String candidate = WiFi.SSID(static_cast<uint8_t>(index));
      if (candidate.isEmpty()) {
        continue;
      }
      bool duplicate = false;
      for (size_t shown = 0; shown < shownCount; ++shown) {
        if (shownSsids[shown] == candidate) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) {
        continue;
      }
      shownSsids[shownCount++] = candidate;
      bool hasOpenAccessPoint = false;
      bool hasProtectedAccessPoint = false;
      bool hasUnsupportedAccessPoint = false;
      for (int16_t match = 0; match < scanCount && match < 255; ++match) {
        if (WiFi.SSID(static_cast<uint8_t>(match)) != candidate) {
          continue;
        }
        const wifi_auth_mode_t authMode =
            WiFi.encryptionType(static_cast<uint8_t>(match));
        if (!supportedPersonalAuth(authMode)) {
          hasUnsupportedAccessPoint = true;
        } else if (authMode == WIFI_AUTH_OPEN) {
          hasOpenAccessPoint = true;
        } else {
          hasProtectedAccessPoint = true;
        }
      }
      const String escapedSsid = htmlEscape(candidate);
      page += F("<label class='network'><input type='radio' name='network_choice' value='");
      page += String(index);
      page += F("'");
      if (!hasOpenAccessPoint && !hasProtectedAccessPoint) {
        page += F(" disabled");
      } else if (candidate == ssid_) {
        page += F(" checked");
        savedNetworkSelected = true;
      }
      page += F("><span><strong>");
      page += escapedSsid;
      page += F("</strong><small>");
      const int32_t rssi = WiFi.RSSI(static_cast<uint8_t>(index));
      page += signalLabel(rssi);
      page += F(" · ");
      page += String(rssi);
      page += F(" dBm · ");
      if (!hasOpenAccessPoint && !hasProtectedAccessPoint) {
        page += F("Unsupported security");
      } else if (hasOpenAccessPoint && hasProtectedAccessPoint) {
        page += F("Mixed security - password recommended");
      } else if (hasProtectedAccessPoint) {
        page += F("Password protected");
      } else {
        page += F("Open");
      }
      if (hasUnsupportedAccessPoint &&
          (hasOpenAccessPoint || hasProtectedAccessPoint)) {
        page += F(" · some incompatible access points");
      }
      page += F("</small></span></label>");
    }
    if (shownCount == 0) {
      page += F("<p class='error'>No named 2.4 GHz networks found.</p>");
    }
    page += F("<label class='network'><input type='radio' name='network_choice' value='manual'");
    if (!savedNetworkSelected) {
      page += F(" checked");
    }
    page += F("><span><strong>Hidden/manual network / 隐藏或手动输入</strong><small>Select this before using the manual SSID field below.</small></span></label>");
  } else {
    page += F("<p class='error'>No 2.4 GHz networks found. Move closer to the router and rescan.<br>未找到 2.4 GHz 网络，请靠近路由器后重试。</p>");
    page += F("<input type='hidden' name='network_choice' value='manual'>");
  }
  page += F("<div class='actions'><a class='refresh' href='/scan'>Rescan / 重新扫描</a></div></fieldset>");
  page += F("<label class='field'>Hidden/manual SSID / 隐藏或手动输入<input name='ssid' maxlength='32' value='");
  page += htmlEscape(ssid_);
  page += F("' autocapitalize='none' autocorrect='off' spellcheck='false'></label>");
  const bool canKeepPassword = !ssid_.isEmpty() && !password_.isEmpty() &&
                               validWifiPassword(password_);
  if (canKeepPassword) {
    page += F("<fieldset class='networks'><legend>Wi-Fi password / Wi-Fi 密码</legend><label class='network'><input type='radio' name='password_action' value='keep' checked><span><strong>Use saved password / 使用已保存密码</strong><small>Only for the same saved Wi-Fi / 仅适用于原来保存的同一 Wi-Fi</small></span></label><label class='network'><input type='radio' name='password_action' value='replace'><span><strong>Use a new password / 使用新密码</strong><small>Select this when changing Wi-Fi / 更换 Wi-Fi 时请选择此项</small></span></label></fieldset>");
  } else {
    page += F("<input type='hidden' name='password_action' value='replace'>");
  }
  page += F("<label class='field'>New Wi-Fi password / 新 Wi-Fi 密码<input name='new_password' type='password' maxlength='64' autocomplete='new-password' autocapitalize='none' autocorrect='off' spellcheck='false' placeholder='Leave empty when using the saved password / 保留原密码时留空'></label>");
  page += F("<small class='lead'>A new password must contain 8–63 printable characters or a 64-digit hexadecimal PSK. Leave it blank for an open network.<br>新密码为 8–63 个可打印字符或 64 位十六进制 PSK；开放网络不需密码。</small>");
  page += F("<label class='field'>Mac Bridge IPv4<input name='host' maxlength='15' required value='");
  page += htmlEscape(bridgeHost_);
  page += F("' inputmode='decimal' placeholder='192.168.1.50'></label><small class='lead'>Use the Mac's active LAN IPv4; never use 127.0.0.1 or 192.168.4.1.</small>");
  page += F("<label class='field'>Mac Bridge port<input name='port' type='number' min='1' max='65535' value='");
  page += String(bridgePort_);
  page += F("'></label><label class='field'>Bridge pairing code / Bridge 配对码<input name='pairing' maxlength='64' autocomplete='off' autocapitalize='none' autocorrect='off' spellcheck='false' placeholder='");
  page += hasPairingKey_ ? F("Saved - leave empty to keep / 已保存，留空保持不变")
                         : F("32 hex characters / 32 位十六进制");
  page += F("'></label><small class='lead'>Only for the Claude Code companion: run <code>bridge.py pair</code> on the Mac and paste the code. It is stored on this device and never sent over the network.<br>仅用于 Claude Code 联动：在 Mac 上运行 <code>bridge.py pair</code> 并粘贴配对码。配对码只保存在本设备，不会通过网络发送。</small>");
  page += F("<input type='hidden' name='token' value='");
  page += setupCsrfToken_;
  page += F("'><button class='submit' type='submit'>Save and test Wi-Fi / 保存并测试 Wi-Fi</button></form></main></body></html>");
  sendSetupHtml(page);
}

void WifiManager::handleNetworkScan() {
  startNetworkScan();
  setupServer_.sendHeader("Location", "/", true);
  setupServer_.sendHeader("Cache-Control", "no-store");
  setupServer_.send(303, "text/plain; charset=utf-8", "Refresh network list...");
}

void WifiManager::handleSave() {
  if (setupServer_.arg("token") != setupCsrfToken_) {
    setupServer_.send(403, "text/plain; charset=utf-8",
                      "Setup session expired. Reload the page and try again. / "
                      "配置会话已失效，请刷新页面后重试。");
    return;
  }
  const String networkChoice = setupServer_.arg("network_choice");
  const String manualSsid = setupServer_.arg("ssid");
  const int16_t scanCount = WiFi.scanComplete();
  uint8_t selectedNetworkIndex = 0;
  const bool manualNetwork =
      networkChoice.isEmpty() || networkChoice == "manual";
  const bool selectedNetworkFound =
      !manualNetwork &&
      parseNetworkIndex(networkChoice, scanCount, selectedNetworkIndex);
  if (!manualNetwork && !selectedNetworkFound) {
    setupServer_.send(400, "text/plain; charset=utf-8",
                      "The selected scan result is no longer available. "
                      "Rescan and try again. / 所选扫描结果已失效，请重新扫描。");
    return;
  }
  const String newSsid = selectedNetworkFound
                             ? WiFi.SSID(selectedNetworkIndex)
                             : manualSsid;
  uint8_t newPairingKey[kPairingKeyBytes] = {};
  const String pairingInput = setupServer_.arg("pairing");
  const bool replacePairingKey = !pairingInput.isEmpty();
  if (replacePairingKey &&
      !parsePairingCode(pairingInput, newPairingKey, kPairingKeyBytes)) {
    setupServer_.send(400, "text/plain; charset=utf-8",
                      "The pairing code must be the 32 hexadecimal characters "
                      "printed by bridge.py pair. / 配对码必须是 bridge.py pair "
                      "显示的 32 位十六进制字符。");
    return;
  }
  const String passwordAction = setupServer_.arg("password_action");
  const String passwordInput = setupServer_.arg("new_password");
  String newPassword;
  const String newHost = setupServer_.arg("host");
  uint16_t parsedPort = 0;
  bool selectedNetworkProtected = false;
  bool selectedNetworkSupported = !selectedNetworkFound;
  if (selectedNetworkFound && scanCount > 0) {
    for (int16_t index = 0; index < scanCount && index < 255; ++index) {
      if (WiFi.SSID(static_cast<uint8_t>(index)) == newSsid) {
        const wifi_auth_mode_t authMode =
            WiFi.encryptionType(static_cast<uint8_t>(index));
        if (supportedPersonalAuth(authMode)) {
          selectedNetworkSupported = true;
          selectedNetworkProtected =
              selectedNetworkProtected || authMode != WIFI_AUTH_OPEN;
        }
      }
    }
  }
  if (!selectedNetworkSupported) {
    setupServer_.send(400, "text/plain; charset=utf-8",
                      "The selected network uses unsupported security. Use "
                      "WPA2 or WPA2/WPA3 Personal. / 所选网络的安全类型不受支持。");
    return;
  }
  const bool canKeepPassword = !ssid_.isEmpty() && !password_.isEmpty() &&
                               validWifiPassword(password_);
  if (passwordAction == "keep") {
    if (!canKeepPassword || newSsid != ssid_ || !passwordInput.isEmpty() ||
        (selectedNetworkFound && !selectedNetworkProtected)) {
      setupServer_.send(
          400, "text/plain; charset=utf-8",
          "The saved password is only for the same protected Wi-Fi. To change "
          "networks or enter a password, select Use a new password. / "
          "原密码仅用于原来保存的同一 Wi-Fi。更换网络或输入密码时，请选择使用新密码。");
      return;
    }
    newPassword = password_;
  } else if (passwordAction == "replace") {
    newPassword = passwordInput;
    if (manualNetwork && newSsid == ssid_ && canKeepPassword &&
        newPassword.isEmpty()) {
      setupServer_.send(
          400, "text/plain; charset=utf-8",
          "This Wi-Fi already has a saved password. Select Use saved password "
          "or enter its new password. For an open network select its scan "
          "result explicitly. / 该网络已有保存的密码。请选择使用已保存密码，"
          "或输入新密码；改用开放网络时请明确选择扫描结果。");
      return;
    }
  } else {
    setupServer_.send(
        400, "text/plain; charset=utf-8",
        "Select a password option. Reload this page if it is outdated. / "
        "请选择密码选项；页面过期时请刷新后重试。");
    return;
  }
  if (selectedNetworkFound && !selectedNetworkProtected &&
      !newPassword.isEmpty()) {
    setupServer_.send(400, "text/plain; charset=utf-8",
                      "The selected network is open; leave the password blank. / "
                      "所选网络是开放网络，密码请留空。");
    return;
  }
  IPAddress parsedBridgeAddress;
  const bool validBridgeHost =
      !newHost.isEmpty() && parsedBridgeAddress.fromString(newHost.c_str());
  if (newSsid.isEmpty() || newSsid.length() > 32 ||
      !validWifiPassword(newPassword) || !validBridgeHost ||
      !parsePort(setupServer_.arg("port"), parsedPort)) {
    setupServer_.send(400, "text/plain; charset=utf-8",
                      "Invalid configuration. Select or enter a Wi-Fi name; "
                      "use an empty password only for open Wi-Fi, an 8-63 "
                      "character password, or a 64-digit hexadecimal PSK. "
                      "Bridge host is required and must be an IPv4 address. / "
                      "必须填写 Mac Bridge 的 IPv4 地址。");
    return;
  }
  if (selectedNetworkFound && selectedNetworkProtected &&
      newPassword.isEmpty()) {
    setupServer_.send(
        400, "text/plain; charset=utf-8",
        "The selected network is password protected. Enter its Wi-Fi password.");
    return;
  }
  if (!preferencesReady_) {
    setupServer_.send(500, "text/plain; charset=utf-8",
                      "Device storage is unavailable; settings were not saved.");
    return;
  }

  StoredConfiguration configuration;
  memset(&configuration, 0, sizeof(configuration));
  configuration.magic = kConfigurationMagic;
  configuration.version = kConfigurationVersion;
  configuration.bridgePort = parsedPort;
  strlcpy(configuration.ssid, newSsid.c_str(), sizeof(configuration.ssid));
  strlcpy(configuration.password, newPassword.c_str(),
          sizeof(configuration.password));
  strlcpy(configuration.bridgeHost, newHost.c_str(),
          sizeof(configuration.bridgeHost));
  configuration.checksum = configurationChecksum(configuration);

  StoredConfiguration verifiedConfiguration{};
  const bool configurationStored =
      preferences_.putBytes(kConfigurationKey, &configuration,
                            sizeof(configuration)) == sizeof(configuration) &&
      preferences_.getBytes(kConfigurationKey, &verifiedConfiguration,
                            sizeof(verifiedConfiguration)) ==
          sizeof(verifiedConfiguration) &&
      memcmp(&configuration, &verifiedConfiguration,
             sizeof(configuration)) == 0 &&
      validStoredConfiguration(verifiedConfiguration);
  if (!configurationStored) {
    setupServer_.send(500, "text/plain; charset=utf-8",
                      "Device storage verification failed; the device will not "
                      "restart. / 设备存储校验失败，设备不会重启。");
    return;
  }
  if (replacePairingKey) {
    uint8_t verifiedKey[kPairingKeyBytes] = {};
    const bool keyStored =
        preferences_.putBytes(kPairingKeyKey, newPairingKey, kPairingKeyBytes) ==
            kPairingKeyBytes &&
        preferences_.getBytes(kPairingKeyKey, verifiedKey, kPairingKeyBytes) ==
            kPairingKeyBytes &&
        memcmp(newPairingKey, verifiedKey, kPairingKeyBytes) == 0;
    if (!keyStored) {
      setupServer_.send(500, "text/plain; charset=utf-8",
                        "Wi-Fi settings were saved, but the pairing code could "
                        "not be verified; the device will not restart. / Wi-Fi "
                        "设置已保存，但配对码校验失败，设备不会重启。");
      return;
    }
    Serial.println("Bridge pairing key stored and verified");
  }
  String savedPage =
                    F("<!doctype html><meta charset='utf-8'><meta name='viewport' "
                    "content='width=device-width,initial-scale=1'><title>Testing"
                    "</title><style>body{font-family:system-ui;background:#07101f;"
                    "color:#eef;padding:28px;line-height:1.5}main{max-width:440px;"
                    "margin:auto}.pending{padding:12px;border-radius:10px;"
                    "background:#3b2e12;border:1px solid #8d712a}</style><main>"
                    "<h1>Settings saved to device<br>设置已保存到设备</h1>"
                    "<p class='pending'><strong>Not connected yet / 尚未联网</strong>"
                    "</p><p>The device is restarting and testing the selected "
                    "2.4 GHz network. Its setup hotspot will disappear, so Safari "
                    "may switch to cellular or home Wi-Fi and 192.168.4.1 may stop "
                    "opening; that is normal during the test.</p><p>设备正在重启并测试"
                    "所选 2.4 GHz 网络。此时配置热点会消失，Safari 可能切回蜂窝"
                    "网络或家庭 Wi-Fi，192.168.4.1 暂时无法打开属于正常现象。"
                    "</p><p>The screen will say WI-FI CONNECTING until it has a "
                    "real IP. If SETUP MODE returns after about 40 seconds, "
                    "rejoin the DevCompanion hotspot and reopen 192.168.4.1 to "
                    "read the failure reason.</p><p>屏幕在真正获得 IP 前会显示 "
                    "WI-FI CONNECTING。如果约 40 秒后重新出现 SETUP MODE，"
                    "请重新连接 DevCompanion 热点并打开 192.168.4.1 查看原因。"
                    "</p></main>");
  sendSetupHtml(savedPage);
  Serial.println("Wi-Fi and bridge settings persisted and verified; restarting");
  restartAtMs_ = millis() + 750;
  restartPending_ = true;
}

void WifiManager::openDeviceSettings(uint32_t nowMs) {
  manualSetup_=true;
  manualSetupUntilMs_=nowMs+kManualSetupWindowMs;
  enterSetupMode(); // existing setup path preserves saved credentials
}

void WifiManager::closeDeviceSettings(uint32_t nowMs) {
  if (state_ != State::SETUP || restartPending_) return;
  manualSetup_=false;manualSetupUntilMs_=0;
  setupServer_.stop();
  if(networkScanRunning_)esp_wifi_scan_stop();
  networkScanRunning_=false;networkScanRequested_=false;setupStationAttemptRunning_=false;
  WiFi.scanDelete();WiFi.softAPdisconnect(true);WiFi.mode(WIFI_STA);
  Serial.println("Device settings closed; saved configuration kept");
  if(!hasCredentials_)return;
  if(WiFi.status()==WL_CONNECTED){markConnected(false);return;}
  retryCount_=0;firstConnectionStartedMs_=nowMs;
  beginStationAttempt(nowMs);
}

void WifiManager::pollResetButton(uint32_t nowMs) {
  if (digitalRead(board::kBootButton) == LOW) {
    if (resetButtonDownSinceMs_ == 0) {
      resetButtonDownSinceMs_ = nowMs;
    } else if (!resetHandled_ && nowMs - resetButtonDownSinceMs_ >= kResetHoldMs) {
      resetHandled_ = true;
      clearSettingsAndRestart(nowMs);
    }
  } else {
    // Released after 1.5-5 s: open the setup page without erasing anything.
    if (resetButtonDownSinceMs_ != 0 && !resetHandled_ && !restartPending_ &&
        nowMs - resetButtonDownSinceMs_ >= kManualSetupHoldMs &&
        state_ != State::SETUP) {
      Serial.println("Manual setup requested; saved settings are kept");
      manualSetup_ = true;
      manualSetupUntilMs_ = nowMs + kManualSetupWindowMs;
      enterSetupMode();
    }
    resetButtonDownSinceMs_ = 0;
    resetHandled_ = false;
  }
}

void WifiManager::clearSettingsAndRestart(uint32_t nowMs) {
  Serial.println("Wi-Fi settings reset requested");
  preferences_.clear();
  WiFi.disconnect(true, true);
  restartAtMs_ = nowMs + 500;
  restartPending_ = true;
}
