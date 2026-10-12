#include "websocket_client.h"
#include "hub/hub_protocol.h"

#include <mbedtls/md.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include "firmware_version.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <ArduinoJson.h>
#include "storage/sd_card.h"

namespace {

constexpr char kFirmwareVersion[] = DEVCOMPANION_VERSION;

int controlPriority(const protocol::IncomingMessage& message) {
  switch (message.type) {
    case protocol::IncomingType::ACTION_ACK: return 6;
    case protocol::IncomingType::DISMISS: return 5;
    case protocol::IncomingType::ACTION_REQUIRED: return 4;
    case protocol::IncomingType::NOTIFICATION: return int(message.event.type);
    case protocol::IncomingType::STATUS:
      return strcmp(message.state, "error") == 0 ? 3 : 1;
    default: return 0;
  }
}

uint16_t readLittle16(const uint8_t* value) {
  return static_cast<uint16_t>(value[0]) |
         (static_cast<uint16_t>(value[1]) << 8U);
}

uint32_t readLittle32(const uint8_t* value) {
  return static_cast<uint32_t>(value[0]) |
         (static_cast<uint32_t>(value[1]) << 8U) |
         (static_cast<uint32_t>(value[2]) << 16U) |
         (static_cast<uint32_t>(value[3]) << 24U);
}

uint64_t readLittle64(const uint8_t* value) {
  uint64_t result = 0;
  for (uint8_t index = 0; index < 8; ++index) {
    result |= static_cast<uint64_t>(value[index]) << (index * 8U);
  }
  return result;
}

uint64_t parseWallpaperKey(const char* value) {
  uint64_t result = 0;
  for (size_t index = 0; index < 16; ++index) {
    const char character = value[index];
    const uint8_t digit = character <= '9'
                              ? static_cast<uint8_t>(character - '0')
                              : static_cast<uint8_t>(character - 'a' + 10);
    result = (result << 4U) | digit;
  }
  return result;
}

uint32_t reconnectDelay(uint8_t attempt) {
  const uint8_t shift = attempt > 5 ? 5 : attempt;
  const uint32_t delayMs = 1000UL << shift;
  return delayMs > 30000UL ? 30000UL : delayMs;
}

}  // namespace

void CompanionWebSocketClient::begin() {
  if (callbackConfigured_) {
    return;
  }
  client_.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
    handleEvent(type, payload, length);
  });
  pendingJpeg_ = static_cast<uint8_t*>(heap_caps_malloc(
      kMaximumJpegBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (pendingJpeg_ == nullptr) {
    transportLog_.println("Video wallpaper buffer allocation failed");
  } else {
    transportLog_.printf("Video wallpaper compressed buffer: %u bytes internal RAM\n",
                  static_cast<unsigned>(kMaximumJpegBytes));
  }
  callbackConfigured_ = true;
}

void CompanionWebSocketClient::loop(bool wifiAvailable, const String& host,
                                    uint16_t port, uint32_t nowMs) {
  // Existing transport state is owned by this loop; no new network work.
  if (connected_ != observedConnected_) {
    observedConnected_ = connected_;
    if (connected_) ++observedConnections_;
    else ++observedDisconnections_;
  }
  if (!wifiAvailable || host.isEmpty() || port == 0) {
    if (transportActive_) {
      stopTransport();
    }
    return;
  }

  if (!transportActive_ || host != host_ || port != port_) {
    if (transportActive_) {
      stopTransport();
    }
    startTransport(host, port);
  }

  const bool wasConnected = connected_;
  const uint32_t transportStartedMs = millis();
  client_.loop();
  if (wasConnected && !connected_) {
    transportLog_.printf("WebSocket disconnect transport_loop_ms=%lu\n",
                  static_cast<unsigned long>(millis() - transportStartedMs));
  }
  nowMs = millis();  // Callbacks and transport I/O may advance the clock.
  if (connected_ && nowMs - lastHeartbeatMs_ >= kHeartbeatIntervalMs) {
    String heartbeat = protocol::makeHeartbeat(nowMs);
    if (client_.sendTXT(heartbeat)) {
      lastHeartbeatMs_ = nowMs;
    }
  }
}

bool CompanionWebSocketClient::popHubResult(hub::Result& result) {
  if(!hubResultPending_)return false;result=pendingHubResult_;hubResultPending_=false;return true;
}
bool CompanionWebSocketClient::sendHubAction(const hub::Action& action) {
  if(!connected_||!hasPairingKey_)return false;String packet=hub::encodeAction(action);return client_.sendTXT(packet);
}

bool CompanionWebSocketClient::popMessage(
    protocol::IncomingMessage& message) {
  if (queueCount_ > 0) {
    // ACK/cancellation cannot wait behind a saturated event burst.
    size_t next = 0;
    for (size_t i = 1; i < queueCount_; ++i) {
      if (controlPriority(queue_[i]) > controlPriority(queue_[next])) next = i;
    }
    message = queue_[next];
    for (size_t i = next + 1; i < queueCount_; ++i) queue_[i - 1] = queue_[i];
    --queueCount_;
    return true;
  }
  if (!videoControlPending_) {
    return false;
  }
  message = pendingVideoControl_;
  videoControlPending_ = false;
  return true;
}

void CompanionWebSocketClient::setPairingKey(const uint8_t* key) {
  memcpy(pairingKey_, key, sizeof(pairingKey_));
  hasPairingKey_ = true;
}

void CompanionWebSocketClient::answerAuthChallenge(const char* nonce) {
  if (!hasPairingKey_) {
    transportLog_.println("Bridge pairing challenge ignored: no pairing key stored");
    return;
  }
  // HMAC-SHA256(pairing key, 32 ASCII nonce characters); the key itself
  // never leaves the device.
  uint8_t mac[32];
  const mbedtls_md_info_t* sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (sha256 == nullptr ||
      mbedtls_md_hmac(sha256, pairingKey_, sizeof(pairingKey_),
                      reinterpret_cast<const unsigned char*>(nonce), strlen(nonce),
                      mac) != 0) {
    transportLog_.println("Bridge pairing challenge failed: HMAC unavailable");
    return;
  }
  char hex[65];
  for (size_t i = 0; i < sizeof(mac); ++i) snprintf(hex + i * 2, 3, "%02x", mac[i]);
  String response = protocol::makeAuthResponse(hex);
  client_.sendTXT(response);
  transportLog_.println("Bridge pairing challenge answered");
}

bool CompanionWebSocketClient::popAgentStatus(AgentStatusSnapshot& status) {
  if (!agentStatusPending_) return false;
  status = pendingAgentStatus_;
  agentStatusPending_ = false;
  return true;
}

bool CompanionWebSocketClient::popHostStatus(HostStatusSnapshot& status) {
  if (!hostStatusPending_) {
    return false;
  }
  status = pendingHostStatus_;
  hostStatusPending_ = false;
  return true;
}

bool CompanionWebSocketClient::popVideoFrame(VideoWallpaperFrame& frame) {
  if (!videoFramePending_ || pendingJpeg_ == nullptr) {
    return false;
  }
  frame.jpeg = pendingJpeg_;
  frame.jpegLength = pendingJpegLength_;
  frame.sequence = pendingSequence_;
  strlcpy(frame.wallpaperId, expectedWallpaperId_, sizeof(frame.wallpaperId));
  videoFramePending_ = false;
  return true;
}

bool CompanionWebSocketClient::sendAction(const char* requestId,
                                          const char* actionId) {
  if (!connected_) {
    return false;
  }
  String action = protocol::makeAction(requestId, actionId);
  return client_.sendTXT(action);
}

bool CompanionWebSocketClient::sendWallpaperStatus(const char* wallpaperId,
                                                    const char* state,
                                                    uint32_t sequence) {
  if (!connected_) {
    return false;
  }
  String status = protocol::makeWallpaperStatus(wallpaperId, state, sequence);
  return client_.sendTXT(status);
}

bool CompanionWebSocketClient::sendCacheAck(const storage::CacheAck& ack) {
  if (!connected_) return false;
  StaticJsonDocument<384> document;
  document["v"] = 1;
  document["type"] = "wallpaper_sync_ack";
  document["transfer_id"] = ack.transferId;
  document["wallpaper_id"] = ack.wallpaperId;
  document["state"] = ack.state;
  document["offset"] = ack.offset;
  document["bytes"] = ack.bytes;
  document["reason"] = ack.reason;
  document["sd_capacity_bytes"] = storage::sdCardCapacityBytes();
  String output;
  serializeJson(document, output);
  return client_.sendTXT(output);
}

void CompanionWebSocketClient::startTransport(const String& host, uint16_t port) {
  host_ = host;
  port_ = port;
  reconnectAttempt_ = 0;
  connected_ = false;
  client_.begin(host_.c_str(), port_, "/");
  client_.setReconnectInterval(reconnectDelay(reconnectAttempt_));
  client_.enableHeartbeat(10000, 3000, 2);
  transportActive_ = true;
  transportLog_.printf("WebSocket connecting host=%s port=%u\n", host_.c_str(),
                static_cast<unsigned>(port_));
}

void CompanionWebSocketClient::stopTransport() {
  client_.disconnect();
  transportActive_ = false;
  connected_ = false;
  host_.clear();
  port_ = 0;
  reconnectAttempt_ = 0;
  queueHead_ = 0;
  queueTail_ = 0;
  queueCount_ = 0;
  videoStreamSelected_ = false;
  videoFramePending_ = false;
  videoControlPending_ = false;
  hostStatusPending_ = false;
  agentStatusPending_ = false;
  hasAcceptedSequence_ = false;
  expectedWallpaperId_[0] = '\0';
  expectedWallpaperKey_ = 0;
}

void CompanionWebSocketClient::handleEvent(WStype_t type, uint8_t* payload,
                                           size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
#if defined(SENSOR_DUAL_PROBE) || defined(SENSOR_SERVICE_ENABLED)
      ++eventCounts_.connections;
      if (everEventConnected_ && !eventConnected_) ++eventCounts_.reconnections;
      eventConnected_ = true;
      everEventConnected_ = true;
#endif
      connected_ = true;
      hubReady_ = false;
      reconnectAttempt_ = 0;
      client_.setReconnectInterval(reconnectDelay(reconnectAttempt_));
      uint8_t nonce[16]; esp_fill_random(nonce, sizeof(nonce));
      for (size_t i=0; i<sizeof(nonce); ++i) snprintf(otaNonce_ + i*2, 3, "%02x", nonce[i]);
      String hello = protocol::makeHello(kFirmwareVersion,
          sdWallpaper_ && sdWallpaper_->available(), hasPairingKey_, ota_ ? otaNonce_ : nullptr);
      client_.sendTXT(hello);
      lastHeartbeatMs_ = millis();
      transportLog_.println("WebSocket connected");
      break;
    }
    case WStype_DISCONNECTED:
#if defined(SENSOR_DUAL_PROBE) || defined(SENSOR_SERVICE_ENABLED)
      ++eventCounts_.disconnections;
      eventConnected_ = false;
#endif
      hubReady_ = false;
      if(hubModel_)hubModel_->disconnected();
      if (ota_) ota_->disconnected();
      if (sdWallpaper_) sdWallpaper_->abortUpload();
      if (connected_) {
        transportLog_.println("WebSocket disconnected");
      }
      connected_ = false;
      if (reconnectAttempt_ < 10) {
        ++reconnectAttempt_;
      }
      client_.setReconnectInterval(reconnectDelay(reconnectAttempt_));
      queueHead_ = 0;
      queueTail_ = 0;
      queueCount_ = 0;
      videoStreamSelected_ = false;
      videoFramePending_ = false;
      videoControlPending_ = false;
      hostStatusPending_ = false;
      agentStatusPending_ = false;
      hasAcceptedSequence_ = false;
      expectedWallpaperId_[0] = '\0';
      expectedWallpaperKey_ = 0;
      break;
    case WStype_TEXT: {
      protocol::IncomingMessage& message = receiveScratch_;
      const char* rejectionReason = nullptr;
      if (!protocol::parseIncoming(payload, length, message,
                                   rejectionReason)) {
        transportLog_.printf("WebSocket message rejected: %s\n", rejectionReason);
        break;
      }
      if (message.type == protocol::IncomingType::HUB_SNAPSHOT) {
        if(hasPairingKey_&&hubModel_){
          if(hubModel_->accept(message.hubPage,millis()))transportLog_.printf("HUB_SNAPSHOT section=%u revision=%lu items=%u\n",unsigned(message.hubPage.section),(unsigned long)message.hubPage.revision,unsigned(message.hubPage.total));
          hubReady_=true;
        }
        break;
      }
      if (message.type == protocol::IncomingType::HUB_RESULT) {
        if(hasPairingKey_){pendingHubResult_=message.hubResult;hubResultPending_=true;}
        break;
      }
      if (message.type == protocol::IncomingType::HOST_STATUS) {
        // Telemetry is latest-only and never competes with actionable control
        // messages for the bounded FIFO.
        pendingHostStatus_ = message.hostStatus;
        hostStatusPending_ = true;
        break;
      }
      if (message.type == protocol::IncomingType::AUTH_CHALLENGE) {
        answerAuthChallenge(message.nonce);
        sendOtaBoot();
        break;
      }
      if (message.type == protocol::IncomingType::OTA_OFFER && !verifyOtaOffer(message)) {
        transportLog_.println("OTA offer rejected: authentication"); break;
      }
      if (message.type == protocol::IncomingType::AGENT_STATUS) {
        pendingAgentStatus_ = message.agentStatus;
        agentStatusPending_ = true;
        break;
      }
      if (message.type == protocol::IncomingType::VIDEO_WALLPAPER) {
        // Selection controls are latest-only. They cannot evict actions or
        // notifications from the regular FIFO, while configuring the binary
        // receiver immediately for the frames that follow on this socket.
        pendingVideoControl_ = message;
        videoControlPending_ = true;
        configureVideoStream(message);
        break;
      }
      enqueue(message);
      break;
    }
    case WStype_BIN:
      if (length >= 4 && memcmp(payload, "DWO1", 4) == 0 && ota_)
        ota_->acceptChunk(payload, length, millis());
      else if (ota_ && ota_->active()) break;
      else if (length >= 4 && memcmp(payload, "DWS1", 4) == 0 && sdWallpaper_)
        sdWallpaper_->acceptChunk(payload, length, millis());
      else acceptVideoFrame(payload, length);
      break;
    case WStype_ERROR:
      transportLog_.println("WebSocket transport error");
      break;
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
      transportLog_.println("WebSocket message rejected: fragmented frame");
      break;
    case WStype_PING:
    case WStype_PONG:
      break;
  }
}

bool CompanionWebSocketClient::enqueue(
    const protocol::IncomingMessage& message) {
  if (queueCount_ == kQueueCapacity) {
    size_t victim = kQueueCapacity;
    int lowest = controlPriority(message);
    for (size_t i = 0; i < queueCount_; ++i) {
      if (controlPriority(queue_[i]) < lowest) {
        lowest = controlPriority(queue_[i]); victim = i;
      }
    }
    if (victim == kQueueCapacity) {
      transportLog_.println("WebSocket message rejected: receive queue full");
      return false;
    }
    transportLog_.println("WebSocket receive queue evicted lower priority control");
    for (size_t i = victim + 1; i < queueCount_; ++i) queue_[i - 1] = queue_[i];
    --queueCount_;
  }
  queue_[queueCount_++] = message;
  return true;
}

void CompanionWebSocketClient::configureVideoStream(
    const protocol::IncomingMessage& message) {
  strlcpy(expectedWallpaperId_, message.wallpaperId,
          sizeof(expectedWallpaperId_));
  expectedWallpaperKey_ = parseWallpaperKey(expectedWallpaperId_);
  videoStreamSelected_ = true;
  videoFramePending_ = false;
  hasAcceptedSequence_ = false;
}

void CompanionWebSocketClient::acceptVideoFrame(const uint8_t* payload,
                                                size_t length) {
  if (!videoStreamSelected_ || pendingJpeg_ == nullptr) {
    rejectVideoFrame("no active video control");
    return;
  }
  if (payload == nullptr || length < kVideoFrameHeaderBytes + 4) {
    rejectVideoFrame("packet too short");
    return;
  }
  if (memcmp(payload, "DWP1", 4) != 0 || payload[4] != 1 ||
      payload[5] != 0 || readLittle16(payload + 6) != kVideoFrameHeaderBytes) {
    rejectVideoFrame("header identity");
    return;
  }
  if (readLittle16(payload + 8) != 320 ||
      readLittle16(payload + 10) != 480) {
    rejectVideoFrame("dimensions");
    return;
  }
  const uint32_t sequence = readLittle32(payload + 12);
  const uint32_t jpegLength = readLittle32(payload + 16);
  const uint64_t wallpaperKey = readLittle64(payload + 20);
  if (wallpaperKey != expectedWallpaperKey_) {
    rejectVideoFrame("wallpaper id");
    return;
  }
  if (jpegLength <= 4 || jpegLength > kMaximumJpegBytes ||
      length != kVideoFrameHeaderBytes + jpegLength) {
    rejectVideoFrame("JPEG length");
    return;
  }
  const uint8_t* jpeg = payload + kVideoFrameHeaderBytes;
  if (jpeg[0] != 0xFF || jpeg[1] != 0xD8 ||
      jpeg[jpegLength - 2] != 0xFF || jpeg[jpegLength - 1] != 0xD9) {
    rejectVideoFrame("JPEG markers");
    return;
  }
  if (hasAcceptedSequence_ &&
      static_cast<int32_t>(sequence - lastAcceptedSequence_) <= 0) {
    return;
  }
  memcpy(pendingJpeg_, jpeg, jpegLength);
  pendingJpegLength_ = jpegLength;
  pendingSequence_ = sequence;
  lastAcceptedSequence_ = sequence;
  hasAcceptedSequence_ = true;
  videoFramePending_ = true;
}

void CompanionWebSocketClient::rejectVideoFrame(const char* reason) {
  const uint32_t nowMs = millis();
  if (lastBinaryRejectMs_ == 0 || nowMs - lastBinaryRejectMs_ >= 2000) {
    transportLog_.printf("WebSocket video frame rejected: %s\n", reason);
    lastBinaryRejectMs_ = nowMs;
  }
}

bool CompanionWebSocketClient::verifyOtaOffer(const protocol::IncomingMessage& m) const {
  if (!hasPairingKey_ || !otaNonce_[0]) return false;
  char canonical[256];
  const int size = snprintf(canonical, sizeof(canonical), "ota-v1\n%s\n%s\n%lu\n%s\n%s\n%s",
      otaNonce_, m.transferId, (unsigned long)m.cacheBytes, m.otaSha, m.otaVersion, m.otaBuild);
  if (size <= 0 || size >= int(sizeof(canonical))) return false;
  uint8_t digest[32]; char hex[65];
  if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), pairingKey_, sizeof(pairingKey_),
      reinterpret_cast<const uint8_t*>(canonical), size, digest) != 0) return false;
  for (size_t i=0;i<32;++i) snprintf(hex+i*2,3,"%02x",digest[i]);
  uint8_t difference = 0;
  for (size_t i=0;i<64;++i) difference |= hex[i] ^ m.otaMac[i];
  return difference == 0;
}

bool CompanionWebSocketClient::sendOtaAck(const storage::OtaAck& ack) {
  if (!connected_) return false;
  StaticJsonDocument<320> doc;
  doc["v"]=1; doc["type"]="ota_ack"; doc["transfer_id"]=ack.transferId;
  doc["state"]=ack.state; doc["offset"]=ack.offset; doc["reason"]=ack.reason;
  String message; serializeJson(doc,message); return client_.sendTXT(message);
}

void CompanionWebSocketClient::sendOtaBoot() {
  if (!connected_ || !ota_ || !hasPairingKey_) return;
  const esp_partition_t* running = esp_ota_get_running_partition();
  uint8_t digest[32]; char hex[65];
  if (!running || esp_partition_get_sha256(running, digest) != ESP_OK) return;
  for (size_t i=0;i<32;++i) snprintf(hex+i*2,3,"%02x",digest[i]);
  esp_ota_img_states_t state;
  const esp_err_t result = esp_ota_get_state_partition(running, &state);
  const char* bootState = result == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY ? "pending" :
                         result == ESP_OK && state == ESP_OTA_IMG_VALID ? "valid" : "unknown";
  StaticJsonDocument<320> doc;
  doc["v"]=1; doc["type"]="ota_boot"; doc["firmware"]=kFirmwareVersion;
  doc["state"]=bootState; doc["image_sha256"]=hex;
  String message; serializeJson(doc,message); client_.sendTXT(message);
}
