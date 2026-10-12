#pragma once

#include <WebSocketsClient.h>
#include <stdint.h>

namespace network {

// Plain LAN WebSocket adapter for the pinned WebSockets 2.6.1 / ESP32 2.0.17.
// The upstream TCP connection and frame parser remain in use. Only the HTTP
// upgrade is read incrementally, without Stream::readStringUntil().
class RecoveringWebSocketsClient : public WebSocketsClient {
 public:
  enum class Stage : uint8_t { Waiting, TcpConnect, HttpUpgrade, Connected };
  struct Diagnostics {
    Stage stage = Stage::Waiting;
    uint32_t stageStartedMs = 0;
    uint32_t tcpAttempts = 0;
    uint32_t tcpFailures = 0;
    uint32_t upgradeAttempts = 0;
    uint32_t upgradeTimeouts = 0;
    uint32_t protocolFailures = 0;
    uint32_t freshRestarts = 0;
    uint32_t heartbeatResets = 0;
    uint32_t connections = 0;
    uint32_t disconnects = 0;
    uint32_t lastTcpMs = 0;
    uint32_t maxTcpMs = 0;
    uint32_t maxPollMs = 0;
    int32_t lastError = 0;
  };

  static constexpr uint32_t kUpgradeTimeoutMs = 5000;
  static constexpr uint32_t kFreshRestartMs = 90000;
  static constexpr size_t kHeaderBytesPerPoll = 512;
  static constexpr size_t kMaxHeaderLineBytes = 512;
  static constexpr size_t kMaxHeaderBytes = 4096;

  RecoveringWebSocketsClient() = default;
  ~RecoveringWebSocketsClient() override;
  void begin(const char* host, uint16_t port, const char* url = "/",
             const char* protocol = "arduino");
  void loop();
  void disconnect();

  Diagnostics diagnostics() const { return diagnostics_; }
  static const char* stageName(Stage stage);

 protected:
  void clientDisconnect(WSclient_t* client) override;
  void runCbEvent(WStype_t type, uint8_t* payload, size_t length) override;

 private:
  enum Error : int32_t {
    NoError = 0,
    TcpConnectFailed = -1,
    UpgradeTimedOut = -2,
    HeaderTooLarge = -3,
    UpgradeRejected = -4,
    FreshTransportRestart = -5,
  };

  void setStage(Stage stage, uint32_t nowMs);
  void resetHeartbeat(uint32_t nowMs);
  void startUpgrade();
  void pollUpgrade();
  void restartTransport(uint32_t nowMs);
  void finishPoll(uint32_t startedMs);

  Diagnostics diagnostics_;
  bool started_ = false;
  uint32_t unconnectedSinceMs_ = 0;
  uint32_t headerStartedMs_ = 0;
  size_t lineBytes_ = 0;
  size_t headerBytes_ = 0;
  char line_[kMaxHeaderLineBytes + 1] = {};
  String savedHost_;
  String savedUrl_;
  String savedProtocol_;
};

}  // namespace network
