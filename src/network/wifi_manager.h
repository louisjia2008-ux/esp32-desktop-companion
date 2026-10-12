#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>

class WifiManager {
 public:
  enum class State : uint8_t {
    CONNECTING,
    RETRY_WAIT,
    CONNECTED,
    SETUP,
  };

  void begin(uint32_t nowMs);
  void loop(uint32_t nowMs);
  void openDeviceSettings(uint32_t nowMs);
  void closeDeviceSettings(uint32_t nowMs);

  bool connected() const { return state_ == State::CONNECTED; }
  // Main-loop observations of connected(), since object construction. These
  // include deliberate disconnects and can miss changes between loop calls;
  // they are not counts of every radio event or connection attempt.
  uint32_t observedConnectionCount() const { return observedConnections_; }
  uint32_t observedDisconnectionCount() const { return observedDisconnections_; }
  uint32_t observedReconnectionCount() const {
    return observedConnections_ > 0 ? observedConnections_ - 1 : 0;
  }
#if defined(SENSOR_DUAL_PROBE) || defined(SENSOR_SERVICE_ENABLED)
  // Delivered STA association/disassociation notifications, not IP readiness.
  // Disconnects include failed associations and deliberate disconnects.
  struct EventCounts {
    uint32_t connections = 0;
    uint32_t disconnections = 0;
    uint32_t reconnections = 0;
  };
  EventCounts eventCounts() const;
  uint32_t eventConnectionCount() const { return eventCounts().connections; }
  uint32_t eventDisconnectionCount() const { return eventCounts().disconnections; }
  uint32_t eventReconnectionCount() const { return eventCounts().reconnections; }
#endif
  bool hasSavedNetwork() const { return hasCredentials_; }
  bool setupMode() const { return state_ == State::SETUP; }
  State state() const { return state_; }
  uint8_t connectionAttemptNumber() const { return retryCount_ + 1; }
  const char* lastConnectionFailure() const;
  const char* lastConnectionFailureChinese() const;
  const char* setupAccessPointName() const { return accessPointName_; }
  const char* setupAccessPointPassword() const {
    return accessPointPassword_;
  }
  // 16-byte key shared with the Mac Bridge (bridge.py pair). Present only
  // after the user entered it on the setup page; never sent over the LAN.
  static constexpr size_t kPairingKeyBytes = 16;
  bool hasPairingKey() const { return hasPairingKey_; }
  const uint8_t* pairingKey() const { return pairingKey_; }
  // Setup opened with BOOT held 1.5-5 s: saved settings stay, and the
  // hotspot is not closed by a background reconnect for a few minutes.
  bool manualSetupActive() const { return state_ == State::SETUP && manualSetup_; }
  const String& bridgeHost() const { return bridgeHost_; }
  uint16_t bridgePort() const { return bridgePort_; }
  String localIp() const;

 private:
  static constexpr uint32_t kStationAttemptMs = 10000;
  static constexpr uint32_t kInitialSetupTimeoutMs = 30000;
  static constexpr uint32_t kMaximumRetryDelayMs = 30000;
  static constexpr uint32_t kSetupRetryDelayMs = 30000;
  static constexpr uint32_t kManualSetupHoldMs = 1500;
  static constexpr uint32_t kResetHoldMs = 5000;
  static constexpr uint32_t kManualSetupWindowMs = 300000;

  void loadConfiguration();
  void loadPairingKey();
  void beginStationAttempt(uint32_t nowMs);
  void scheduleRetry(uint32_t nowMs);
  void loopSetupConnection(uint32_t nowMs);
  void scheduleSetupRetry(uint32_t nowMs);
  void markConnected(bool closeSetup);
  void enterSetupMode();
  void generateSetupSecrets();
  void configureSetupServer();
  void startNetworkScan();
  void sendSetupHtml(const String& page);
  void handleSetupPage();
  void handleNetworkScan();
  void handleSave();
  void pollResetButton(uint32_t nowMs);
  void clearSettingsAndRestart(uint32_t nowMs);
  uint32_t retryDelayMs() const;

  Preferences preferences_;
  WebServer setupServer_{80};
  State state_ = State::SETUP;
  bool observedConnected_ = false;
  uint32_t observedConnections_ = 0;
  uint32_t observedDisconnections_ = 0;
#if defined(SENSOR_DUAL_PROBE) || defined(SENSOR_SERVICE_ENABLED)
  mutable portMUX_TYPE eventMux_ = portMUX_INITIALIZER_UNLOCKED;
  EventCounts eventCounts_{};
  bool eventConnected_ = false;
  bool everEventConnected_ = false;
#endif
  String ssid_;
  String password_;
  String bridgeHost_;
  uint16_t bridgePort_ = 8765;
  char accessPointName_[33] = {};
  char accessPointPassword_[13] = {};
  char setupCsrfToken_[33] = {};
  bool preferencesReady_ = false;
  bool hasCredentials_ = false;
  bool everConnected_ = false;
  bool initialConnectionFailed_ = false;
  bool setupServerConfigured_ = false;
  bool networkScanRunning_ = false;
  bool networkScanRequested_ = false;
  bool setupStationAttemptRunning_ = false;
  uint32_t networkScanStartedMs_ = 0;
  uint8_t retryCount_ = 0;
  uint32_t firstConnectionStartedMs_ = 0;
  uint32_t attemptStartedMs_ = 0;
  uint32_t nextRetryMs_ = 0;
  uint32_t resetButtonDownSinceMs_ = 0;
  bool resetHandled_ = false;
  uint8_t pairingKey_[kPairingKeyBytes] = {};
  bool hasPairingKey_ = false;
  bool manualSetup_ = false;
  uint32_t manualSetupUntilMs_ = 0;
  uint32_t restartAtMs_ = 0;
  bool restartPending_ = false;
  volatile uint8_t pendingDisconnectReason_ = 0;
  volatile bool disconnectReasonPending_ = false;
  uint8_t lastDisconnectReason_ = 0;
};
