#pragma once

#include <Arduino.h>

#include <array>

#include "ui/ui_manager.h"
#include "hub/hub_model.h"

namespace protocol {

constexpr uint8_t kVersion = 1;
constexpr size_t kMaximumPacketBytes = 2048;

enum class IncomingType : uint8_t {
  HUB_SNAPSHOT,
  HUB_RESULT,
  STATUS,
  NOTIFICATION,
  ACTION_REQUIRED,
  ACTION_ACK,
  DISMISS,
  VIDEO_WALLPAPER,
  WALLPAPER_SYNC,
  HOST_STATUS,
  AGENT_STATUS,
  AUTH_CHALLENGE,
  CLOCK,
  DISPLAY_SETTINGS,
  OTA_OFFER,
  OTA_CONTROL,
};

struct IncomingMessage {
  IncomingType type = IncomingType::NOTIFICATION;
  char state[25] = {};
  char title[49] = {};
  char message[129] = {};
  char requestId[49] = {};
  char wallpaperId[17] = {};
  char wallpaperName[49] = {};
  uint16_t videoWidth = 0;
  uint16_t videoHeight = 0;
  uint8_t videoFps = 0;
  uint32_t videoFrameCount = 0;
  char transferId[17] = {};
  char syncOperation[9] = {};
  uint32_t cacheBytes = 0;
  uint32_t cacheCrc = 0;
  HostStatusSnapshot hostStatus{};
  AgentStatusSnapshot agentStatus{};
  std::array<UiActionOption, kMaxEventActions> actions{};
  size_t actionCount = 0;
  CompanionEvent event{};
  char actionId[33] = {};
  char nonce[33] = {};
  uint32_t clockEpoch = 0;
  int32_t clockOffsetS = 0;
  DisplaySettings displaySettings{};
  char otaVersion[25] = {};
  char otaBuild[40] = {};
  char otaSha[65] = {};
  char otaMac[65] = {};
  bool ackAccepted = false;
  hub::Page hubPage{};
  hub::Result hubResult{};
};

bool parseIncoming(const uint8_t* payload, size_t length,
                   IncomingMessage& message, const char*& rejectionReason);

String makeHello(const char* firmwareVersion, bool sdAvailable = false,
                 bool claudePaired = false, const char* otaNonce = nullptr);
String makeAuthResponse(const char* macHex);
String makeHeartbeat(uint32_t uptimeMs);
String makeAction(const char* requestId, const char* actionId);
String makeWallpaperStatus(const char* wallpaperId, const char* state,
                           uint32_t sequence);

}  // namespace protocol
