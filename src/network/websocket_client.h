#pragma once

#include <Arduino.h>
#include "recovering_websocket_client.h"
#include "logging/runtime_log.h"

#include <array>

#include "protocol/protocol.h"
#include "storage/sd_wallpaper.h"
#include "storage/ota_receiver.h"

struct VideoWallpaperFrame {
  const uint8_t* jpeg = nullptr;
  size_t jpegLength = 0;
  uint32_t sequence = 0;
  char wallpaperId[17] = {};
};

class CompanionWebSocketClient {
 public:
  void begin();
  void setHubModel(hub::Model* model) { hubModel_=model; }
  bool popHubResult(hub::Result& result);
  bool sendHubAction(const hub::Action& action);
  void setSdWallpaper(storage::SdWallpaper* store) { sdWallpaper_ = store; }
  void setOtaReceiver(storage::OtaReceiver* receiver) { ota_ = receiver; }
  bool sendOtaAck(const storage::OtaAck& ack);
  void sendOtaBoot();
  // Enables the claude_companion capability and answers auth challenges.
  void setPairingKey(const uint8_t* key);
  bool sendCacheAck(const storage::CacheAck& ack);
  void loop(bool wifiAvailable, const String& host, uint16_t port,
            uint32_t nowMs);

  bool connected() const { return connected_; }
  network::RecoveringWebSocketsClient::Diagnostics transportDiagnostics() const {
    return client_.diagnostics();
  }
  uint32_t transportLogDrops() const { return transportLog_.droppedLines(); }
  // Main-loop observations, since object construction. Deliberate transport
  // stops count too; changes occurring entirely between observations may be
  // missed. These are not counts of every callback or reconnect attempt.
  uint32_t observedConnectionCount() const { return observedConnections_; }
  uint32_t observedDisconnectionCount() const { return observedDisconnections_; }
  uint32_t observedReconnectionCount() const {
    return observedConnections_ > 0 ? observedConnections_ - 1 : 0;
  }
#if defined(SENSOR_DUAL_PROBE) || defined(SENSOR_SERVICE_ENABLED)
  // Delivered transport callbacks, including failed-attempt disconnects.
  // Owned by the existing loop task; call getters from that same task.
  struct EventCounts {
    uint32_t connections = 0;
    uint32_t disconnections = 0;
    uint32_t reconnections = 0;
  };
  EventCounts eventCounts() const { return eventCounts_; }
  uint32_t eventConnectionCount() const { return eventCounts_.connections; }
  uint32_t eventDisconnectionCount() const { return eventCounts_.disconnections; }
  uint32_t eventReconnectionCount() const { return eventCounts_.reconnections; }
#endif
  bool hubReady() const { return connected_ && hubReady_; }
  bool popMessage(protocol::IncomingMessage& message);
  bool popHostStatus(HostStatusSnapshot& status);
  bool popAgentStatus(AgentStatusSnapshot& status);
  bool popVideoFrame(VideoWallpaperFrame& frame);
  bool sendAction(const char* requestId, const char* actionId);
  bool sendWallpaperStatus(const char* wallpaperId, const char* state,
                           uint32_t sequence);

 private:
  static constexpr size_t kQueueCapacity = 8;
  static constexpr uint32_t kHeartbeatIntervalMs = 10000;
  static constexpr uint32_t kMaximumReconnectMs = 30000;
  static constexpr size_t kVideoFrameHeaderBytes = 28;
  static constexpr size_t kMaximumJpegBytes = 14 * 1024;

  void startTransport(const String& host, uint16_t port);
  void stopTransport();
  void handleEvent(WStype_t type, uint8_t* payload, size_t length);
  void answerAuthChallenge(const char* nonce);
  bool verifyOtaOffer(const protocol::IncomingMessage& message) const;
  bool enqueue(const protocol::IncomingMessage& message);
  void configureVideoStream(const protocol::IncomingMessage& message);
  void acceptVideoFrame(const uint8_t* payload, size_t length);
  void rejectVideoFrame(const char* reason);

  hub::Model* hubModel_=nullptr;
  hub::Result pendingHubResult_{};
  bool hubResultPending_=false;
  bool hubReady_=false;
  network::RecoveringWebSocketsClient client_;
  RuntimeLog transportLog_;
  storage::SdWallpaper* sdWallpaper_ = nullptr;
  storage::OtaReceiver* ota_ = nullptr;
  char otaNonce_[33] = {};
  String host_;
  uint16_t port_ = 0;
  bool callbackConfigured_ = false;
  bool transportActive_ = false;
  bool connected_ = false;
  bool observedConnected_ = false;
  uint32_t observedConnections_ = 0;
  uint32_t observedDisconnections_ = 0;
#if defined(SENSOR_DUAL_PROBE) || defined(SENSOR_SERVICE_ENABLED)
  EventCounts eventCounts_{};
  bool eventConnected_ = false;
  bool everEventConnected_ = false;
#endif
  uint8_t reconnectAttempt_ = 0;
  uint32_t lastHeartbeatMs_ = 0;
  std::array<protocol::IncomingMessage, kQueueCapacity> queue_{};
  // Parse target for the receive callback; too large for the loop stack.
  protocol::IncomingMessage receiveScratch_{};
  size_t queueHead_ = 0;
  size_t queueTail_ = 0;
  size_t queueCount_ = 0;
  HostStatusSnapshot pendingHostStatus_{};
  bool hostStatusPending_ = false;
  uint8_t pairingKey_[16] = {};
  bool hasPairingKey_ = false;
  AgentStatusSnapshot pendingAgentStatus_{};
  bool agentStatusPending_ = false;
  protocol::IncomingMessage pendingVideoControl_{};
  bool videoControlPending_ = false;

  uint8_t* pendingJpeg_ = nullptr;
  size_t pendingJpegLength_ = 0;
  uint32_t pendingSequence_ = 0;
  uint32_t lastAcceptedSequence_ = 0;
  uint32_t lastBinaryRejectMs_ = 0;
  uint64_t expectedWallpaperKey_ = 0;
  char expectedWallpaperId_[17] = {};
  bool videoFramePending_ = false;
  bool videoStreamSelected_ = false;
  bool hasAcceptedSequence_ = false;
};
