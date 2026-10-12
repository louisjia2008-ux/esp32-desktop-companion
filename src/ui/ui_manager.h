#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include <array>

#include "backlight_policy.h"
#include "glass_clock.h"
#include "touch_driver.h"
#include "wallpaper.h"
#include "event_system.h"
#include "hub/hub_ui.h"
#if defined(SENSOR_SERVICE_ENABLED) && SENSOR_SERVICE_ENABLED
#include "sensors/sensor_service.h"
#include "sensors/air_history.h"
#endif

enum class UiState : uint8_t {
  WALLPAPER,
  HOST_STATUS,
  MAC_CONTROL,
  INFO_CARDS,
  TASKS,
  USAGE,
  NOTIFICATION,
  ACTION_REQUIRED,
  SUCCESS,
  ERROR,
  WARNING,
  WIFI_CONNECTING,
  SETUP,
  // Appended to preserve the existing state values used by diagnostics.
  ENVIRONMENT,
};

enum class ConnectionIndicator : uint8_t {
  NO_WIFI,
  WIFI_ONLY,
  BRIDGE_CONNECTED,
};

enum class AgentState : uint8_t { Idle, Working, Waiting, Done, Stopped };
enum class AgentSource : uint8_t { Claude, Codex };

// Latest paired agent session summary from the Bridge (agent_status).
struct AgentSession {
  AgentSource source = AgentSource::Claude;
  char id[9] = {};
  char project[33] = {};
  char tool[33] = {};
  AgentState state = AgentState::Idle;
  uint32_t elapsedS = 0;
  uint32_t ageS = 0;
  uint8_t pending = 0;
};

struct AgentStatusSnapshot {
  static constexpr size_t kMaximumSessions = 4;
  std::array<AgentSession, kMaximumSessions> sessions{};
  size_t count = 0;
};

struct HostStatusSnapshot {
  bool hasCpuPercent = false;
  bool hasMemoryPercent = false;
  bool hasMemoryUsedMb = false;
  bool hasMemoryTotalMb = false;
  bool hasUptimeSeconds = false;
  uint8_t cpuPercent = 0;
  uint8_t memoryPercent = 0;
  uint32_t memoryUsedMb = 0;
  uint32_t memoryTotalMb = 0;
  uint32_t uptimeSeconds = 0;
};

class UiManager {
 public:
  struct ButtonRect { int16_t x, y, width, height; };
  void begin(uint32_t wallpaperSeed);

  void setConnectionIndicator(ConnectionIndicator indicator);
  void showSetup(const char* accessPointName, const char* accessPointPassword,
                 const char* failureMessage);
  bool takeSetupExitRequest() { const bool requested=setupExitRequested_;setupExitRequested_=false;return requested; }
  void showWifiConnecting(uint8_t attemptNumber, bool waitingForRetry,
                          const char* failureMessage);
  void showWifiConnected(const char* localIp, const char* bridgeHost,
                         uint16_t bridgePort, uint32_t nowMs);
  bool showNotification(const char* title, const char* message,
                        uint32_t nowMs);
  bool showStatus(const char* state, const char* title, const char* message,
                  uint32_t nowMs);
  bool showActionRequired(const char* requestId, const char* title,
                          const char* message,
                          const UiActionOption* actions, size_t actionCount);
  void dismiss(const char* requestId);
  void showConnectionLost(uint32_t nowMs);
  bool selectVideoWallpaper(const char* wallpaperId);
  void clearVideoWallpaper() { wallpaper_.clearVideo(); }
  bool decodeVideoWallpaperFrame(const char* wallpaperId,
                                 const uint8_t* jpeg, size_t jpegLength,
                                 uint32_t nowMs);
  bool videoWallpaperFresh(uint32_t nowMs) const;
  void updateHostStatus(const HostStatusSnapshot& status, uint32_t nowMs);
  void invalidateHostStatus();
#if defined(SENSOR_SERVICE_ENABLED) && SENSOR_SERVICE_ENABLED
  void updateSensorSnapshot(const sensors::SensorSnapshot& snapshot,
                            uint32_t nowMs, bool co2High = false);
  // The history worker owns this stable view and its bucket buffer. Rendering
  // only reads it; day changes are requests consumed by main, never SD access.
  void setAirHistoryView(const sensors::AirHistoryView* view) { airHistory_ = view; }
  bool takeAirHistoryRequest(uint8_t& daysAgo) {
    if (!historyRequestPending_) return false;
    daysAgo = historyDaysAgo_;
    historyRequestPending_ = false;
    return true;
  }
  void setSensorOptions(bool presenceWake, bool co2Reminder) {
    presenceWakeEnabled_ = presenceWake;
    co2ReminderEnabled_ = co2Reminder;
  }
  bool presenceWakeEnabled() const { return presenceWakeEnabled_; }
  bool co2ReminderEnabled() const { return co2ReminderEnabled_; }
  bool takeSensorOptionsChange() {
    const bool changed = sensorOptionsChanged_;
    sensorOptionsChanged_ = false;
    return changed;
  }
#endif
  // Lock-screen clock over the wallpaper. Local time comes from main (the
  // Bridge sets the RTC); without a synced clock nothing is drawn.
  void setLocalTime(bool valid, const struct tm& local);
  bool clockVisible() const { return clockVisible_; }
  void setClockVisible(bool visible) { clockVisible_ = visible; }
  // True once after a tap changed the clock visibility (main persists it).
  bool takeClockVisibilityChange() {
    const bool changed = clockVisibilityChanged_;
    clockVisibilityChanged_ = false;
    return changed;
  }
  // Brightness integration: last physical touch, and whether the screen is
  // currently dimmed (the first gesture on a dimmed screen only wakes it).
  uint32_t lastInteractionMs() const { return lastInteractionMs_; }
  void setScreenDimmed(bool dimmed) { screenDimmed_ = dimmed; }
  // Agent sessions shown as a rotating HUD over the live wallpaper.
  void updateAgentStatus(const AgentStatusSnapshot& status, uint32_t nowMs);
  void clearAgentStatus() { agentStatusReceived_ = false; }
  // Index of the session the HUD shows, or -1 when it is hidden.
  int8_t visibleAgentSession(uint32_t nowMs) const;
  size_t visibleAgentCount(uint32_t nowMs) const;
  bool receiveEvent(const CompanionEvent& event, uint32_t nowMs);
  bool acknowledgeAction(const char* requestId, const char* actionId,
                         bool accepted, uint32_t nowMs);
  void actionSendFailed(uint32_t nowMs);
  bool overlayActive() const { return events_.active(); }
  const EventSystem& events() const { return events_; }
  UiState baseState() const { return state_; }
  ButtonRect eventButtonRect(size_t index) const;

  hub::Model& hubModel() { return hub_.model; }
  void setHubSession(uint32_t first,uint32_t second) { hub_.setSession(first,second); }
  void setHubOnline(bool ready, uint32_t nowMs) { hub_.setOnline(ready,nowMs); }
  bool takeHubAction(hub::Action& action) { return hub_.takeAction(action); }
  void hubResult(const hub::Result& result, uint32_t nowMs) { hub_.result(result,nowMs); }
  void hubSendFailed() { hub_.sendFailed(); }
  void tick(uint32_t nowMs);
  bool handleTouch(bool rawTouchDown, const TouchPoint& point,
                   uint32_t nowMs, UiActionEvent& event);
  void resetTouchAfterGap(uint32_t nowMs);
  // Called for samples that carry no usable contact data. Ends a contact
  // whose last real report is older than kStaleContactMs; never dispatches.
  bool releaseStaleContact(uint32_t nowMs);
  bool touchReleaseBarrierActive() const { return blockedUntilRelease_; }
  bool physicalTouchDown() const { return physicalTouchDown_; }
  void render(Arduino_Canvas& gfx, uint32_t nowMs);
  uint32_t clockRenderUs() const { return clockRenderUs_; }
  void setOtaProgress(bool active, uint8_t percent, bool rebooting) {
    if (active != otaProgressActive_) armTouchReleaseBarrier();
    otaProgressActive_ = active; otaPercent_ = percent; otaRebooting_ = rebooting;
  }

  UiState state() const;

 private:
  hub::Ui hub_;
  bool ambientControlConsumed_ = false;
  bool setupExitRequested_ = false;
  uint32_t clockRenderUs_ = 0;
  bool otaProgressActive_ = false, otaRebooting_ = false;
  uint8_t otaPercent_ = 0;
  static constexpr size_t kMaximumActions = kMaxEventActions;

  void setWallpaper();
  void armTouchReleaseBarrier();
  void armTouchReleaseBarrierAt(uint32_t nowMs, bool releaseConfirmed);
  void clearExpiry();
  void setExpiry(uint32_t nowMs, uint32_t durationMs);
  void clearTouchLatch(bool resetActionState = true);
  void clearAmbientTouchLatch();
  int8_t buttonAt(const TouchPoint& point) const;
  void drawConnectionIndicator(Arduino_Canvas& gfx) const;
  void drawOverlay(Arduino_Canvas& gfx) const;
  void drawModal(Arduino_Canvas& gfx) const;
  void drawSetup(Arduino_Canvas& gfx) const;
  void drawHostStatus(Arduino_Canvas& gfx, uint32_t nowMs) const;
#if defined(SENSOR_SERVICE_ENABLED) && SENSOR_SERVICE_ENABLED
  void drawEnvironment(Arduino_Canvas& gfx, uint32_t nowMs) const;
  void drawEnvironmentLive(Arduino_Canvas& gfx, uint32_t nowMs) const;
  void drawEnvironmentRadar(Arduino_Canvas& gfx, uint32_t nowMs) const;
  void drawEnvironmentHistory(Arduino_Canvas& gfx) const;
  void activateSensorButton(int8_t button, uint32_t nowMs);
  int8_t sensorButtonAt(const TouchPoint& point) const;
  enum class EnvironmentView : uint8_t { Live, History, Radar };
  EnvironmentView environmentView_ = EnvironmentView::Live;
  const sensors::AirHistoryView* airHistory_ = nullptr;
  uint8_t historyDaysAgo_ = 0;
  uint8_t historyMetric_ = 0;
  bool historyRequestPending_ = false;
  uint16_t farthestRadarReportCm_ = 0;
  sensors::SensorSnapshot sensorSnapshot_{};
  bool co2High_ = false;
  bool presenceWakeEnabled_ = true;
  bool co2ReminderEnabled_ = true;
  bool sensorOptionsChanged_ = false;
#endif
  void drawEvent(Arduino_Canvas& gfx, uint32_t nowMs) const;
  void drawAgentHud(Arduino_Canvas& gfx, uint32_t nowMs) const;
  void updateAgentRotationPause(uint32_t nowMs);
  void drawPageDots(Arduino_Canvas& gfx) const;
  void handleAmbientGesture(uint32_t nowMs);
  void showPage(int8_t direction, uint32_t nowMs);
  uint16_t holdDuration(int8_t button) const;
  void syncEvent(uint32_t nowMs);
  size_t eventButtonCount() const;

  Wallpaper wallpaper_;
  EventSystem events_;
  uint32_t eventRevision_ = 0;
  uint32_t overlayWallpaperMs_ = 0;
  uint32_t legacyEventCounter_ = 0;
  UiState state_ = UiState::WALLPAPER;
  ConnectionIndicator indicator_ = ConnectionIndicator::NO_WIFI;
  char title_[49] = {};
  char message_[129] = {};
  char requestId_[49] = {};
  char setupAccessPoint_[33] = {};
  char setupAccessPointPassword_[13] = {};
  char setupFailure_[81] = {};
  std::array<UiActionOption, kMaximumActions> actions_{};
  size_t actionCount_ = 0;
  uint32_t expiresAtMs_ = 0;
  bool expiryActive_ = false;

  AgentStatusSnapshot agentStatus_{};
  uint32_t agentStatusReceivedMs_ = 0;
  bool agentStatusReceived_ = false;
  uint32_t agentRotationEpochMs_ = 0;
  uint32_t agentRotationElapsedMs_ = 0;
  uint8_t agentRotationIndex_ = 0;
  bool agentRotationPaused_ = false;
  // A hold-to-confirm button released early shows a hint until this time.
  uint32_t holdHintUntilMs_ = 0;

  HostStatusSnapshot hostStatus_{};
  uint32_t hostStatusUpdatedMs_ = 0;
  bool hostStatusReceived_ = false;

  bool logicalTouchDown_ = false;
  bool actionDispatched_ = false;
  bool sending_ = false;
  int8_t pressedButton_ = -1;
  uint32_t touchStartedMs_ = 0;
  uint32_t lastRawTouchMs_ = 0;
  bool ambientTouchDown_ = false;
  bool ambientToggleConsumed_ = false;
  uint32_t ambientTouchStartedMs_ = 0;
  uint32_t ambientLastRawTouchMs_ = 0;
  TouchPoint ambientStart_{};
  TouchPoint ambientLast_{};
  bool ambientWakeOnly_ = false;
  bool screenDimmed_ = false;
  uint32_t lastInteractionMs_ = 0;
  uifont::GlassClock glassClock_;
  struct tm localTime_ {};
  bool localTimeValid_ = false;
  bool clockVisible_ = true;
  bool clockVisibilityChanged_ = false;
  bool physicalTouchDown_ = false;
  uint32_t physicalLastRawTouchMs_ = 0;
  bool blockedUntilRelease_ = true;
  uint32_t touchSamplesNotBeforeMs_ = 0;
};
