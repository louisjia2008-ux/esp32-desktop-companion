#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <sys/time.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include "firmware_version.h"

#include <cstring>

#include "board_config.h"
#include "display/display_manager.h"
#include "network/websocket_client.h"
#include "network/wifi_manager.h"
#include "protocol/protocol.h"
#include "logging/runtime_log.h"
#include "sensors/sensor_service.h"
#include "sensors/sensor_policy.h"
#include "sensors/radar_range.h"
#include "storage/air_history.h"
#include "storage/sensor_settings_codec.h"
#include "sensors/radar_diagnostics.h"
#include "sensors/scd41_diagnostics.h"
#include "sensors/diagnostic_log.h"
#include "storage/sd_card.h"
#include "storage/sd_wallpaper.h"
#include "touch/touch_sampler.h"
#include "touch/touch_transform.h"
#include "touch_driver.h"
#include "ui/backlight_policy.h"
#include "ui/ui_manager.h"

// The WebSocket receive path nests several library frames before the
// protocol parser runs; 16 KiB leaves headroom over the default 8 KiB.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// Defer Arduino's automatic OTA confirmation until the real peripherals and
// network have passed the startup checks below.
extern "C" bool verifyRollbackLater() { return true; }

namespace {

constexpr uint32_t kSerialBaud = 115200;
constexpr uint32_t kSerialStartupDelayMs = 1200;
// A measured full-frame QSPI flush takes about 56 ms. A 60 ms cadence avoids
// back-to-back flushes and leaves time for touch and network service.
constexpr uint32_t kFrameIntervalMs = 60;
constexpr uint32_t kModalFrameIntervalMs = 100;
constexpr uint32_t kMetricsIntervalMs = 10000;

Axs15231bTouch touchDevice;
touch::Sampler touchSampler;
WifiManager wifi;
CompanionWebSocketClient bridge;
UiManager ui;
storage::SdWallpaper sdWallpaper;
storage::OtaReceiver ota;
uint32_t bootReadyAtMs = 0;
bool bootChecked = false;
bool localVideoActive = false;

bool touchReady = false;
bool wifiUiInitialized = false;
bool wifiUiActive = false;
WifiManager::State displayedWifiState = WifiManager::State::SETUP;
bool bridgeWasConnected = false;
uint32_t lastFrameMs = 0;
uint32_t metricsWindowStartedMs = 0;
uint32_t frameCount = 0;
uint64_t accumulatedFrameUs = 0;
uint64_t accumulatedClockUs = 0;
char activeWallpaperId[17] = {};
bool wallpaperPlayingReported = false;
uint32_t lastWallpaperDecodeErrorMs = 0;
uint32_t decodedVideoFrameCount = 0;
uint64_t accumulatedVideoDecodeUs = 0;
uint32_t maxTouchDispatchLagMs = 0;
uint32_t forcedTouchReleases = 0;
// Clock (set from the Bridge) and screen brightness.
Preferences displayPreferences;
DisplaySettings displaySettings;
BacklightFader backlightFader;
bool clockSynced = false;
int32_t clockOffsetS = 0;
constexpr char kDisplaySettingsKey[] = "disp_v1";
constexpr char kClockVisibleKey[] = "clock_vis";
RuntimeLog runtimeLog;
RTC_DATA_ATTR uint32_t bootCounterMagic = 0;
RTC_DATA_ATTR uint32_t bootCounter = 0;
#if defined(SENSOR_SERVICE_ENABLED)
Preferences sensorPreferences;
bool sensorPreferencesReady = false;
sensors::SensorSettings sensorSettings;
sensors::SensorSnapshot sensorSnapshot;
sensors::PresencePolicy presencePolicy;
sensors::PresencePolicyState presenceState;
sensors::Co2ReminderPolicy co2Policy;
sensors::Co2PolicyState co2State;
storage::AirHistory airHistory;
uint32_t reminderSequence = 0;
uint32_t minFreePsram = UINT32_MAX;

void loadSensorPreferences() {
  sensorPreferencesReady = sensorPreferences.begin("sensors", false);
  uint8_t encoded[sensors::kSensorSettingsBytes] = {};
  if (sensorPreferencesReady &&
      sensorPreferences.getBytesLength("cfg_v1") == sizeof(encoded) &&
      sensorPreferences.getBytes("cfg_v1", encoded, sizeof(encoded)) == sizeof(encoded)) {
    sensors::decodeSensorSettings(encoded, sizeof(encoded), sensorSettings);
  }
  ui.setSensorOptions(sensorSettings.presenceWakeEnabled, sensorSettings.co2ReminderEnabled);
  runtimeLog.printf("SENSOR_SETTINGS wake=%u reminder=%u storage=%s\n",
                    unsigned(sensorSettings.presenceWakeEnabled),
                    unsigned(sensorSettings.co2ReminderEnabled),
                    sensorPreferencesReady ? "READY" : "UNAVAILABLE");
}

void updateLocalSensors(uint32_t nowMs) {
  sensorSnapshot = sensors::readSensorSnapshot(nowMs);
  nowMs = millis();  // A worker can publish between the old clock and the copy.
  // The main loop is the sole owner of UI, policies, and settings persistence.
  if (ui.takeSensorOptionsChange()) {
    sensorSettings.presenceWakeEnabled = ui.presenceWakeEnabled();
    sensorSettings.co2ReminderEnabled = ui.co2ReminderEnabled();
    uint8_t encoded[sensors::kSensorSettingsBytes];
    sensors::encodeSensorSettings(sensorSettings, encoded);
    const bool saved = sensorPreferencesReady &&
        sensorPreferences.putBytes("cfg_v1", encoded, sizeof(encoded)) == sizeof(encoded);
    runtimeLog.printf("SENSOR_SETTINGS wake=%u reminder=%u saved=%u\n",
                      unsigned(sensorSettings.presenceWakeEnabled),
                      unsigned(sensorSettings.co2ReminderEnabled), unsigned(saved));
  }
  const bool radarFresh = sensors::radarFresh(sensorSnapshot.radar, nowMs);
  const auto& radar = sensorSnapshot.radar.sample;
  const auto range = sensors::radarRange(radar);
  presenceState = presencePolicy.update(nowMs, sensorSettings.presenceWakeEnabled,
      radarFresh, radar.presenceKnown, range.nearPresence,
      nowMs - ui.lastInteractionMs());
  sensors::Co2PolicyInputs inputs;
  inputs.nowMs = nowMs;
  inputs.hasSample = sensorSnapshot.air.hasSample;
  inputs.fresh = sensors::airFresh(sensorSnapshot.air, nowMs);
  inputs.sampleAtMs = sensorSnapshot.air.sampledAtMs;
  inputs.co2Ppm = sensorSnapshot.air.co2Ppm;
  inputs.presenceFresh = radarFresh;
  inputs.presenceKnown = radar.presenceKnown;
  inputs.presence = radar.presence;
  inputs.reminderEnabled = sensorSettings.co2ReminderEnabled;
  inputs.canShowReminder = !ota.busy() && !wifi.manualSetupActive() &&
      !ui.overlayActive() && ui.events().queueDepth() == 0 &&
      ui.state() != UiState::SETUP && ui.state() != UiState::WIFI_CONNECTING;
  co2State = co2Policy.update(inputs);
  if (co2State.shouldRemind) {
    CompanionEvent event;
    event.type = EventType::Info;
    snprintf(event.id, sizeof(event.id), "co2-%lu-%lu", (unsigned long)bootCounter,
             (unsigned long)++reminderSequence);
    strlcpy(event.source, "sensors", sizeof(event.source));
    strlcpy(event.title, "CO2 偏高，请通风", sizeof(event.title));
    snprintf(event.message, sizeof(event.message),
             "当前 %u ppm，已持续偏高 5 分钟。建议开窗或改善通风。",
             unsigned(sensorSnapshot.air.co2Ppm));
    event.autoDismissMs = 12000;
    if (ui.receiveEvent(event, nowMs)) {
      co2Policy.markReminderShown(nowMs);
      runtimeLog.printf("LOCAL_CO2_REMINDER shown=1 ppm=%u duration_ms=12000 uptime_ms=%lu\n",
                        unsigned(sensorSnapshot.air.co2Ppm), (unsigned long)nowMs);
    }
  }
  ui.updateSensorSnapshot(sensorSnapshot, nowMs, co2State.highEpisode);
  uint8_t historyDay = 0;
  if (ui.takeAirHistoryRequest(historyDay)) airHistory.requestDay(historyDay);
  sensors::HistoryClock historyClock;
  historyClock.valid = clockSynced;
  historyClock.utcEpochSeconds = clockSynced ? static_cast<uint32_t>(time(nullptr)) : 0;
  historyClock.utcOffsetSeconds = clockOffsetS;
  // SD remains a main-loop resource. Sensor workers keep sampling during OTA,
  // wallpaper uploads and history loads; optional history I/O yields to them.
  airHistory.poll(nowMs, sensorSnapshot, historyClock,
                  !ota.busy() && !sdWallpaper.storageBusy());
  static auto oldHistoryState = sensors::HistoryState::Starting;
  if (oldHistoryState != airHistory.view().state) {
    runtimeLog.printf("AIR_HISTORY_STATE state=%s day=%ld uptime_ms=%lu\n",
        sensors::historyStateName(airHistory.view().state),
        (long)airHistory.view().dayOrdinal, (unsigned long)nowMs);
    oldHistoryState = airHistory.view().state;
  }
  const uint32_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (freePsram < minFreePsram) minFreePsram = freePsram;
  static sensors::Health oldRadar = sensors::Health::Offline, oldAir = sensors::Health::Offline;
  if (oldRadar != sensorSnapshot.radar.health || oldAir != sensorSnapshot.air.health) {
    runtimeLog.printf("SENSOR_STATE radar=%s air=%s radar_error=%ld air_error=%ld uptime_ms=%lu\n",
        sensors::healthName(sensorSnapshot.radar.health), sensors::healthName(sensorSnapshot.air.health),
        (long)sensorSnapshot.radar.lastError, (long)sensorSnapshot.air.lastError, (unsigned long)nowMs);
    oldRadar = sensorSnapshot.radar.health; oldAir = sensorSnapshot.air.health;
  }
}
#endif
#if defined(TOUCH_TRACE)
volatile uint32_t touchInterruptEdges = 0;
void IRAM_ATTR countTouchInterrupt() { ++touchInterruptEdges; }
#endif
uint32_t modalFrameCount = 0;
uint32_t baseFrameCount = 0;
uint32_t renderedEventRevision = 0;
uint32_t maximumMainLoopMs = 0;

void printSizeMiB(const char* label, uint32_t bytes) {
  runtimeLog.printf("%s: %lu bytes (%.2f MB)\n", label,
                static_cast<unsigned long>(bytes),
                static_cast<double>(bytes) / (1024.0 * 1024.0));
}

bool testPsram() {
  constexpr size_t kTestBytes = 4096;
  uint8_t* memory = static_cast<uint8_t*>(
      heap_caps_malloc(kTestBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (memory == nullptr) {
    return false;
  }

  for (size_t index = 0; index < kTestBytes; ++index) {
    memory[index] = static_cast<uint8_t>((index * 37U) ^ 0xA5U);
  }
  bool valid = true;
  for (size_t index = 0; index < kTestBytes; ++index) {
    if (memory[index] != static_cast<uint8_t>((index * 37U) ^ 0xA5U)) {
      valid = false;
      break;
    }
  }
  heap_caps_free(memory);
  return valid;
}

bool runSerialAndMemoryTest() {
  runtimeLog.println();
  runtimeLog.println("================================");
  runtimeLog.println("ESP32-S3 BOOT OK");
  runtimeLog.println("DEVCOMPANION_FW=" DEVCOMPANION_VERSION);
  runtimeLog.println("DEVCOMPANION_BUILD=" __DATE__ " " __TIME__);
  if (bootCounterMagic != 0x45565431U) {
    bootCounterMagic = 0x45565431U;
    bootCounter = 0;
  }
  ++bootCounter;
  runtimeLog.printf("BOOT_METRICS boot_count=%lu reset_reason=%d\n",
                (unsigned long)bootCounter, int(esp_reset_reason()));
  runtimeLog.printf("Chip: %s, revision %u, CPU %u MHz\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getCpuFreqMHz());
  printSizeMiB("Flash", ESP.getFlashChipSize());

  const bool psramDetected = psramFound();
  runtimeLog.printf("PSRAM detected: %s\n", psramDetected ? "YES" : "NO");
  printSizeMiB("PSRAM", ESP.getPsramSize());

  const bool psramPassed = psramDetected && testPsram();
  runtimeLog.printf("PSRAM TEST: %s\n", psramPassed ? "PASS" : "FAIL");
  return psramPassed;
}

bool scanTouchI2c() {
  bool foundExpectedAddress = false;
  runtimeLog.println("I2C scan: START");
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    const uint8_t error = Wire.endTransmission();
    if (error == 0) {
      runtimeLog.printf("I2C device found at 0x%02X\n", address);
      if (address == board::kTouchAddress) {
        foundExpectedAddress = true;
      }
    }
  }

  if (foundExpectedAddress) {
    runtimeLog.println("Touch I2C device found at 0x3B");
  } else {
    runtimeLog.println("Touch I2C device NOT found at 0x3B");
  }
  return foundExpectedAddress;
}

void failClosed(const char* reason) {
  runtimeLog.printf("FATAL: %s\n", reason);
  runtimeLog.println(
      "Startup stopped; reset after correcting the hardware/configuration.");
  while (true) {
    delay(1000);
  }
}

void handleIncomingMessage(const protocol::IncomingMessage& message,
                           uint32_t nowMs) {
  switch (message.type) {
    case protocol::IncomingType::HUB_SNAPSHOT:
      ui.hubModel().accept(message.hubPage,nowMs);break;
    case protocol::IncomingType::HUB_RESULT:
      ui.hubResult(message.hubResult,nowMs);break;
    case protocol::IncomingType::STATUS:
      if (ui.showStatus(message.state, message.title, message.message,
                        nowMs)) {
        runtimeLog.printf("UI state message accepted: status=%s\n",
                      message.state);
      } else {
        runtimeLog.printf("UI state message ignored during unresolved action: status=%s\n",
                      message.state);
      }
      break;
    case protocol::IncomingType::NOTIFICATION:
      ui.receiveEvent(message.event, nowMs);
      break;
    case protocol::IncomingType::ACTION_REQUIRED:
      ui.receiveEvent(message.event, nowMs);
      break;
    case protocol::IncomingType::ACTION_ACK:
      ui.acknowledgeAction(message.requestId, message.actionId,
                           message.ackAccepted, millis());
      break;
    case protocol::IncomingType::DISMISS:
      ui.dismiss(message.requestId);
      runtimeLog.printf("UI state message accepted: dismiss request_id=%s\n",
                    message.requestId);
      break;
    case protocol::IncomingType::VIDEO_WALLPAPER:
      if (sdWallpaper.ready()) break;
      strlcpy(activeWallpaperId, message.wallpaperId,
              sizeof(activeWallpaperId));
      wallpaperPlayingReported = false;
      lastWallpaperDecodeErrorMs = 0;
      if (ui.selectVideoWallpaper(message.wallpaperId)) {
        bridge.sendWallpaperStatus(message.wallpaperId, "selected", 0);
        runtimeLog.printf(
            "Video wallpaper selected: id=%s frames=%lu fps=%u\n",
            message.wallpaperId,
            static_cast<unsigned long>(message.videoFrameCount),
            static_cast<unsigned>(message.videoFps));
      } else {
        bridge.sendWallpaperStatus(message.wallpaperId, "decode_error", 0);
        runtimeLog.println("Video wallpaper unavailable: framebuffer allocation");
      }
      break;
    case protocol::IncomingType::WALLPAPER_SYNC:
      if (ota.busy()) break;
      if (strcmp(message.syncOperation, "begin") == 0)
        sdWallpaper.beginUpload(message.transferId, message.wallpaperId,
                                message.cacheBytes, message.cacheCrc, nowMs);
      else if (strcmp(message.syncOperation, "commit") == 0)
        sdWallpaper.finishUpload(message.transferId);
      else if (strcmp(message.syncOperation, "abort") == 0)
        sdWallpaper.abortUpload(message.transferId);
      else if (strcmp(message.syncOperation, "clear") == 0)
        sdWallpaper.clear(message.transferId, message.wallpaperId);
      break;
    case protocol::IncomingType::HOST_STATUS:
      ui.updateHostStatus(message.hostStatus, nowMs);
      break;
    case protocol::IncomingType::AGENT_STATUS:
      ui.updateAgentStatus(message.agentStatus, nowMs);
      break;
    case protocol::IncomingType::AUTH_CHALLENGE:
      break;  // answered inside the WebSocket client before queueing
    case protocol::IncomingType::OTA_OFFER: {
      if (ui.overlayActive() || !ota.offer(message.transferId, message.otaVersion,
          message.cacheBytes, message.otaSha, nowMs)) {
        storage::OtaAck rejected;
        strlcpy(rejected.transferId, message.transferId, sizeof(rejected.transferId));
        rejected.state = "error"; rejected.reason = "device_busy";
        bridge.sendOtaAck(rejected); break;
      }
      CompanionEvent offer;
      offer.type = EventType::ActionRequired;
      snprintf(offer.id, sizeof(offer.id), "ota-%s", ota.id());
      strlcpy(offer.source, "Bridge", sizeof(offer.source));
      strlcpy(offer.title, "更新固件?", sizeof(offer.title));
      snprintf(offer.message, sizeof(offer.message), "%s -> %s  %.2f MB\n请保持供电,安装后自动重启",
          DEVCOMPANION_VERSION, ota.version(), ota.bytes()/1048576.0);
      offer.actionCount = 2;
      strlcpy(offer.actions[0].id,"install",sizeof(offer.actions[0].id));
      strlcpy(offer.actions[0].label,"安装",sizeof(offer.actions[0].label));
      offer.actions[0].holdMs = 1500;
      strlcpy(offer.actions[1].id,"cancel",sizeof(offer.actions[1].id));
      strlcpy(offer.actions[1].label,"取消",sizeof(offer.actions[1].label));
      if (!ui.receiveEvent(offer,nowMs)) ota.cancel("device_busy");
      break;
    }
    case protocol::IncomingType::OTA_CONTROL:
      if (strcmp(message.syncOperation,"commit") == 0) ota.commit(message.transferId,nowMs);
      else ota.abort(message.transferId);
      break;
    case protocol::IncomingType::CLOCK: {
      const timeval now{static_cast<time_t>(message.clockEpoch), 0};
      settimeofday(&now, nullptr);
      clockOffsetS = message.clockOffsetS;
      if (!clockSynced) runtimeLog.printf("CLOCK_SYNCED offset_s=%ld\n", (long)clockOffsetS);
      clockSynced = true;
      break;
    }
    case protocol::IncomingType::DISPLAY_SETTINGS:
      if (message.displaySettings != displaySettings) {
        displaySettings = message.displaySettings;
        // Written only on change, so a reconnect never wears the flash.
        displayPreferences.putBytes(kDisplaySettingsKey, &displaySettings,
                                    sizeof(displaySettings));
        runtimeLog.printf("DISPLAY_SETTINGS night=%u-%u day=%u night=%u idle=%us/%u\n",
                      displaySettings.nightStartMin, displaySettings.nightEndMin,
                      displaySettings.dayLevel, displaySettings.nightLevel,
                      displaySettings.idleDimS, displaySettings.idleLevel);
      }
      break;
  }
}

ConnectionIndicator currentConnectionIndicator() {
  if (!wifi.connected()) {
    return ConnectionIndicator::NO_WIFI;
  }
  return bridge.connected() ? ConnectionIndicator::BRIDGE_CONNECTED
                            : ConnectionIndicator::WIFI_ONLY;
}

void loadDisplayPreferences() {
  if (!displayPreferences.begin("display", false)) {
    runtimeLog.println("Display preferences unavailable; using defaults");
    return;
  }
  DisplaySettings stored;
  if (displayPreferences.getBytesLength(kDisplaySettingsKey) == sizeof(stored) &&
      displayPreferences.getBytes(kDisplaySettingsKey, &stored, sizeof(stored)) == sizeof(stored) &&
      validDisplaySettings(stored)) {
    displaySettings = stored;
  }
  ui.setClockVisible(displayPreferences.getUChar(kClockVisibleKey, 1) != 0);
}

void updateClockAndBacklight(uint32_t nowMs) {
  struct tm local {};
  if (clockSynced) {
    const time_t shifted = time(nullptr) + clockOffsetS;
    gmtime_r(&shifted, &local);
  }
  ui.setLocalTime(clockSynced, local);

  BacklightInputs inputs;
  inputs.timeValid = clockSynced;
  inputs.minuteOfDay = static_cast<uint16_t>(local.tm_hour * 60 + local.tm_min);
  inputs.sinceInteractionMs = nowMs - ui.lastInteractionMs();
  inputs.attention = ota.active() || ui.overlayActive() || ui.state() == UiState::SETUP ||
                     ui.state() == UiState::WIFI_CONNECTING;
#if defined(SENSOR_SERVICE_ENABLED)
  inputs.activityOverride = presenceState.overrideIdle;
  inputs.overrideActive = presenceState.awake;
#endif
  const bool night = clockSynced && isNight(displaySettings, inputs.minuteOfDay);
  static bool previousNight = night;
  const bool nightChanged = night != previousNight;
  previousNight = night;
  const uint8_t level =
      backlightFader.step(targetBacklight(displaySettings, inputs), nowMs, nightChanged ? 2000 : 300);
  display::setBacklightLevel(level);
  const uint8_t awake = night ? displaySettings.nightWakeLevel : displaySettings.dayLevel;
  ui.setScreenDimmed(level + 5 < awake);

  if (ui.takeClockVisibilityChange()) {
    displayPreferences.putUChar(kClockVisibleKey, ui.clockVisible() ? 1 : 0);
  }
}

void renderFrame(uint32_t nowMs) {
  const uint32_t interval = ui.overlayActive() || ota.active() ? kModalFrameIntervalMs : kFrameIntervalMs;
  const bool eventChanged = renderedEventRevision != ui.events().revision();
  if (!eventChanged && nowMs - lastFrameMs < interval) {
    return;
  }
  lastFrameMs = nowMs;
  renderedEventRevision = ui.events().revision();
  const uint32_t startedUs = micros();
  Arduino_Canvas& frameBuffer = display::canvas();
  ui.render(frameBuffer, nowMs);
  accumulatedClockUs += ui.clockRenderUs();
  frameBuffer.flush();
  const uint32_t elapsedUs = micros() - startedUs;
  accumulatedFrameUs += elapsedUs;
  ++frameCount;
  if (ui.overlayActive()) ++modalFrameCount;
  else ++baseFrameCount;
}

void reportMetrics(uint32_t nowMs) {
  if (nowMs - metricsWindowStartedMs < kMetricsIntervalMs || frameCount == 0) {
    return;
  }
  const uint32_t windowMs = nowMs - metricsWindowStartedMs;
  const double averageFrameMs =
      static_cast<double>(accumulatedFrameUs) / frameCount / 1000.0;
  const double framesPerSecond =
      static_cast<double>(frameCount) * 1000.0 / windowMs;
  const double averageDecodeMs =
      decodedVideoFrameCount == 0
          ? 0.0
          : static_cast<double>(accumulatedVideoDecodeUs) /
                decodedVideoFrameCount / 1000.0;
  const char* wallpaperMode =
      activeWallpaperId[0] == '\0'
          ? "builtin"
          : (ui.videoWallpaperFresh(nowMs) ? "video" : "fallback");
  runtimeLog.printf(
      "METRICS avg_frame_ms=%.2f fps=%.1f free_heap=%lu free_psram=%lu "
      "wifi=%s websocket=%s wallpaper=%s decoded_frames=%lu "
      "avg_decode_ms=%.2f clock_ms=%.2f backlight=%u "
      "window_ms=%lu frame_count=%lu\n",
      averageFrameMs, framesPerSecond,
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(
          heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
      wifi.connected() ? "connected" : "disconnected",
      bridge.connected() ? "connected" : "disconnected", wallpaperMode,
      static_cast<unsigned long>(decodedVideoFrameCount), averageDecodeMs,
      static_cast<double>(accumulatedClockUs) / frameCount / 1000.0,
      backlightFader.level(), static_cast<unsigned long>(windowMs),
      static_cast<unsigned long>(frameCount));
  accumulatedClockUs = 0;
#if defined(SENSOR_DUAL_PROBE) || defined(SENSOR_SERVICE_ENABLED)
  const auto wifiEvents = wifi.eventCounts();
  const auto wsEvents = bridge.eventCounts();
  runtimeLog.printf("NETWORK_DIAG WIFI_CONNECTION_EVENTS=%lu WIFI_DISCONNECTION_EVENTS=%lu WIFI_RECONNECTIONS=%lu WS_CONNECTION_EVENTS=%lu WS_DISCONNECTION_EVENTS=%lu WS_RECONNECTIONS=%lu\n",
                (unsigned long)wifiEvents.connections,
                (unsigned long)wifiEvents.disconnections,
                (unsigned long)wifiEvents.reconnections,
                (unsigned long)wsEvents.connections,
                (unsigned long)wsEvents.disconnections,
                (unsigned long)wsEvents.reconnections);
  runtimeLog.printf("NETWORK_DIAG WIFI_OBSERVED_CONNECTIONS=%lu WIFI_OBSERVED_DISCONNECTIONS=%lu WIFI_OBSERVED_RECONNECTIONS=%lu WS_OBSERVED_CONNECTIONS=%lu WS_OBSERVED_DISCONNECTIONS=%lu WS_OBSERVED_RECONNECTIONS=%lu\n",
                (unsigned long)wifi.observedConnectionCount(),
                (unsigned long)wifi.observedDisconnectionCount(),
                (unsigned long)wifi.observedReconnectionCount(),
                (unsigned long)bridge.observedConnectionCount(),
                (unsigned long)bridge.observedDisconnectionCount(),
                (unsigned long)bridge.observedReconnectionCount());
  const auto transport = bridge.transportDiagnostics();
  runtimeLog.printf("WS_TRANSPORT stage=%s tcp_attempts=%lu tcp_failures=%lu upgrades=%lu upgrade_timeouts=%lu protocol_failures=%lu fresh_restarts=%lu heartbeat_resets=%lu connections=%lu disconnects=%lu last_attempt_ms=%lu max_attempt_ms=%lu max_poll_ms=%lu max_loop_ms=%lu last_error=%ld log_drops=%lu uptime_ms=%lu\n",
      network::RecoveringWebSocketsClient::stageName(transport.stage),
      (unsigned long)transport.tcpAttempts, (unsigned long)transport.tcpFailures,
      (unsigned long)transport.upgradeAttempts, (unsigned long)transport.upgradeTimeouts,
      (unsigned long)transport.protocolFailures, (unsigned long)transport.freshRestarts,
      (unsigned long)transport.heartbeatResets, (unsigned long)transport.connections,
      (unsigned long)transport.disconnects, (unsigned long)transport.lastTcpMs,
      (unsigned long)transport.maxTcpMs, (unsigned long)transport.maxPollMs,
      (unsigned long)maximumMainLoopMs,
      (long)transport.lastError, (unsigned long)bridge.transportLogDrops(), (unsigned long)nowMs);
#endif
#if defined(SENSOR_SERVICE_ENABLED)
  const auto& r = sensorSnapshot.radar;
  const auto& a = sensorSnapshot.air;
  const auto range = sensors::radarRange(r.sample);
  const bool rangeFresh = sensors::radarFresh(r, nowMs) && r.sample.presenceKnown;
  runtimeLog.printf("RADAR_RANGE fresh=%u room_presence=%u moving_valid=%u still_valid=%u nearest_cm=%u near=%u wake_limit_cm=%u uptime_ms=%lu\n",
      unsigned(rangeFresh), unsigned(rangeFresh && r.sample.presence),
      unsigned(rangeFresh && range.movingValid), unsigned(rangeFresh && range.stillValid),
      unsigned(rangeFresh && range.hasDistance ? range.nearestDistanceCm : 0),
      unsigned(rangeFresh && range.nearPresence), unsigned(sensors::kWakeRangeCm),
      (unsigned long)nowMs);
  runtimeLog.printf("SENSOR_RADAR health=%s presence=%s age_ms=%lu frames=%lu invalid=%lu unsupported=%lu uart_errors=%lu moving_cm=%u still_cm=%u moving_energy=%u still_energy=%u stack_free=%lu\n",
      sensors::healthName(r.health), sensors::radarFresh(r, nowMs) && r.sample.presenceKnown ? (r.sample.presence ? "YES" : "NO") : "UNKNOWN",
      (unsigned long)(r.hasSample ? sensors::sampleAgeMs(nowMs, r.sample.receivedAtMs) : UINT32_MAX),
      (unsigned long)r.counters.validFrames, (unsigned long)r.counters.invalidFrames,
      (unsigned long)r.counters.unsupportedFrames, (unsigned long)r.uartErrors,
      unsigned(r.sample.movingDistanceCm), unsigned(r.sample.stillDistanceCm),
      unsigned(r.sample.movingEnergy), unsigned(r.sample.stillEnergy), (unsigned long)r.workerStackFree);
  runtimeLog.printf("SENSOR_AIR health=%s age_ms=%lu sample_at_ms=%lu co2_ppm=%u temperature_c=%.2f humidity_rh=%.2f samples=%lu errors=%lu nack=%lu crc=%lu short=%lu invalid=%lu recoveries=%lu init_failures=%lu stack_free=%lu\n",
      sensors::healthName(a.health),
      (unsigned long)(a.hasSample ? sensors::sampleAgeMs(nowMs, a.sampledAtMs) : UINT32_MAX),
      (unsigned long)a.sampledAtMs, unsigned(a.co2Ppm), double(a.temperatureC), double(a.humidityRh),
      (unsigned long)a.validSamples, (unsigned long)a.errors, (unsigned long)a.nackErrors,
      (unsigned long)a.crcErrors, (unsigned long)a.shortReads, (unsigned long)a.invalidSamples,
      (unsigned long)a.recoveryAttempts, (unsigned long)a.initializationFailures, (unsigned long)a.workerStackFree);
  runtimeLog.printf("SENSOR_METRICS uptime_ms=%lu min_heap=%lu largest_heap=%lu min_psram=%lu largest_psram=%lu log_drops=%lu wake_override=%u awake=%u absent_ms=%lu high_episode=%u high_ms=%lu page=%u\n",
      (unsigned long)nowMs, (unsigned long)ESP.getMinFreeHeap(),
      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned long)minFreePsram, (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
      (unsigned long)runtimeLog.droppedLines(), unsigned(presenceState.overrideIdle), unsigned(presenceState.awake),
      (unsigned long)presenceState.continuousAbsentMs, unsigned(co2State.highEpisode),
      (unsigned long)co2State.highForMs, unsigned(ui.baseState()));
  const auto& history = airHistory.counters();
  const auto& historyView = airHistory.view();
  runtimeLog.printf("AIR_HISTORY state=%s day=%ld ago=%u minutes=%lu samples=%lu recorded=%lu written=%lu dropped=%lu io_errors=%lu crc_errors=%lu invalid=%lu torn=%lu protected=%lu clock_jumps=%lu max_io_ms=%lu error=%ld uptime_ms=%lu\n",
      sensors::historyStateName(historyView.state), (long)historyView.dayOrdinal,
      unsigned(historyView.daysAgo), (unsigned long)historyView.validMinutes,
      (unsigned long)history.acceptedSamples, (unsigned long)history.recordedMinutes,
      (unsigned long)history.writtenMinutes, (unsigned long)history.droppedMinutes,
      (unsigned long)history.ioErrors, (unsigned long)history.crcErrors,
      (unsigned long)history.invalidRecords, (unsigned long)history.tornTails,
      (unsigned long)history.protectedFiles, (unsigned long)history.clockDiscontinuities,
      (unsigned long)history.maxIoMs, (long)history.lastError, (unsigned long)nowMs);
#endif
  const EventStatistics& events = ui.events().statistics();
  sdWallpaper.reportMetrics();
  runtimeLog.printf(
      "EVENT_METRICS queue=%u history=%u overlay=%u attempts=%lu accepted=%lu "
      "retries=%lu duplicate_events=%lu overflow_drops=%lu ack_timeouts=%lu "
      "touch_dispatch_max_ms=%lu base_frames=%lu modal_frames=%lu "
      "uptime_ms=%lu boot_count=%lu loop_stack_free=%lu\n",
      unsigned(ui.events().queueDepth()), unsigned(ui.events().historyCount()),
      unsigned(ui.overlayActive()), (unsigned long)events.actionAttempts,
      (unsigned long)events.accepted, (unsigned long)events.retries,
      (unsigned long)events.duplicateEvents, (unsigned long)events.overflowDrops,
      (unsigned long)events.ackTimeouts, (unsigned long)maxTouchDispatchLagMs,
      (unsigned long)baseFrameCount, (unsigned long)modalFrameCount,
      (unsigned long)nowMs, (unsigned long)bootCounter,
      (unsigned long)uxTaskGetStackHighWaterMark(nullptr));
  const touch::SamplerStatistics touchStatistics = touchSampler.statistics();
  runtimeLog.printf(
      "TOUCH_METRICS samples=%lu down_samples=%lu idle_reports=%lu "
      "read_errors=%lu forced_releases=%lu queue_overflows=%lu "
      "max_gap_ms=%lu\n",
      static_cast<unsigned long>(touchStatistics.samples),
      static_cast<unsigned long>(touchStatistics.downSamples),
      static_cast<unsigned long>(touchStatistics.idleReports),
      static_cast<unsigned long>(touchStatistics.readErrors),
      static_cast<unsigned long>(forcedTouchReleases),
      static_cast<unsigned long>(touchStatistics.queueOverflows),
      static_cast<unsigned long>(touchStatistics.maximumGapMs));
  if (touchStatistics.readErrors != 0) {
    runtimeLog.printf(
        "TOUCH_ERRORS command=%lu response=%lu count=%lu coordinates=%lu "
        "last_count=%u last_event=%u raw_x=%u raw_y=%u\n",
        static_cast<unsigned long>(touchStatistics.commandErrors),
        static_cast<unsigned long>(touchStatistics.responseErrors),
        static_cast<unsigned long>(touchStatistics.pointCountErrors),
        static_cast<unsigned long>(touchStatistics.coordinateErrors),
        touchStatistics.lastError.pointCount, touchStatistics.lastError.event,
        touchStatistics.lastError.rawX, touchStatistics.lastError.rawY);
  }
  metricsWindowStartedMs = nowMs;
  frameCount = 0;
  accumulatedFrameUs = 0;
  decodedVideoFrameCount = 0;
  accumulatedVideoDecodeUs = 0;
  baseFrameCount = 0;
  modalFrameCount = 0;
}

void serviceTouch() {
  if (!touchReady) {
    return;
  }
  touch::Sample sample;
  while (touchSampler.pop(sample)) {
    if (sample.gap) {
      ui.resetTouchAfterGap(sample.sampledAtMs);
      runtimeLog.println("Touch sample queue overflow; waiting for a fresh release");
    }
#if defined(TOUCH_TRACE)
    // Diagnostic build only (PLATFORMIO_BUILD_FLAGS=-DTOUCH_TRACE): log each
    // change of the raw controller report together with the UI release gate.
    {
      static TouchReadResult lastResult = TouchReadResult::ERROR;
      static TouchReadDiagnostics last{};
      static bool lastBlocked = false;
      static bool lastPhysical = false;
      const TouchReadDiagnostics& d = sample.diagnostics;
      const bool blocked = ui.touchReleaseBarrierActive();
      const bool physical = ui.physicalTouchDown();
      if (sample.result != lastResult || d.pointCount != last.pointCount ||
          d.event != last.event || d.rawX != last.rawX || d.rawY != last.rawY ||
          d.interruptLow != last.interruptLow ||
          blocked != lastBlocked || physical != lastPhysical) {
        runtimeLog.printf(
            "TOUCH_TRACE t=%lu res=%c count=%u event=%u x=%u y=%u "
            "int_low=%d int_edges=%lu raw=%02X%02X%02X%02X%02X%02X%02X%02X "
            "blocked=%d physical=%d state=%u\n",
            static_cast<unsigned long>(sample.sampledAtMs),
            sample.result == TouchReadResult::DOWN ? 'D'
                : sample.result == TouchReadResult::UP ? 'U' : 'E',
            d.pointCount, d.event, d.rawX, d.rawY, d.interruptLow,
            static_cast<unsigned long>(touchInterruptEdges),
            d.raw[0], d.raw[1], d.raw[2], d.raw[3], d.raw[4], d.raw[5],
            d.raw[6], d.raw[7], blocked, physical,
            static_cast<unsigned>(ui.state()));
        lastResult = sample.result;
        last = d;
        lastBlocked = blocked;
        lastPhysical = physical;
      }
    }
#endif
    if (sample.result == TouchReadResult::ERROR) {
      // An I2C error is not evidence that the finger was released, but a
      // contact with no real report for several seconds must not keep the
      // UI's release barrier locked.
      if (ui.releaseStaleContact(sample.sampledAtMs)) {
        ++forcedTouchReleases;
        runtimeLog.printf("TOUCH_BARRIER_FORCED count=%lu\n",
                      static_cast<unsigned long>(forcedTouchReleases));
      }
      continue;
    }
    const bool rawTouchDown = sample.result == TouchReadResult::DOWN;
    TouchPoint displayPoint{};
    if (rawTouchDown &&
        !touch::toDisplayCoordinates(sample.point, displayPoint)) {
      continue;
    }
    UiActionEvent action;
    if (ui.handleTouch(rawTouchDown, displayPoint, sample.sampledAtMs, action) &&
        action.valid) {
      char otaId[24]; snprintf(otaId, sizeof(otaId), "ota-%s", ota.id());
      if (strncmp(action.requestId,"ota-",4) == 0) {
        const bool matches = ota.offered() && strcmp(action.requestId,otaId) == 0;
        ui.acknowledgeAction(action.requestId,action.actionId,matches,millis());
        if (matches) {
          if (strcmp(action.actionId,"install") == 0) {
            sdWallpaper.abortUpload(); ota.approve(millis());
          } else ota.cancel();
        }
        continue;
      }
      const uint32_t lag = millis() - sample.sampledAtMs;
      if (lag > maxTouchDispatchLagMs) maxTouchDispatchLagMs = lag;
      if (bridge.sendAction(action.requestId, action.actionId)) {
        runtimeLog.printf("ACTION_SENT request_id=%s action_id=%s\n",
                      action.requestId, action.actionId);
      } else {
        ui.actionSendFailed(millis());
      }
    }
  }
}

bool localUiAvailable() {
#if defined(SENSOR_SERVICE_ENABLED)
  return true;
#else
  return sdWallpaper.ready();
#endif
}

void updateWifiUi(uint32_t nowMs) {
  const WifiManager::State currentState = wifi.state();
  if (wifiUiInitialized && currentState == displayedWifiState) {
    return;
  }

  if (localUiAvailable() && wifi.hasSavedNetwork() && !wifi.manualSetupActive() &&
      currentState != WifiManager::State::CONNECTED) {
    ui.dismiss("");
    wifiUiActive = false;
    displayedWifiState = currentState;
    wifiUiInitialized = true;
    return;
  }

  switch (currentState) {
    case WifiManager::State::SETUP:
      ui.showSetup(wifi.setupAccessPointName(),
                   wifi.setupAccessPointPassword(),
                   wifi.lastConnectionFailure());
      wifiUiActive = true;
      break;
    case WifiManager::State::CONNECTING:
      ui.showWifiConnecting(wifi.connectionAttemptNumber(), false,
                            wifi.lastConnectionFailure());
      wifiUiActive = true;
      break;
    case WifiManager::State::RETRY_WAIT:
      ui.showWifiConnecting(wifi.connectionAttemptNumber(), true,
                            wifi.lastConnectionFailure());
      wifiUiActive = true;
      break;
    case WifiManager::State::CONNECTED:
      ui.showWifiConnected(wifi.localIp().c_str(), wifi.bridgeHost().c_str(),
                           wifi.bridgePort(), nowMs);
      wifiUiActive = true;
      break;
  }
  displayedWifiState = currentState;
  wifiUiInitialized = true;
}

}  // namespace

void setup() {
#if defined(SENSOR_SERVICE_ENABLED)
  // HWCDC defaults to 256 bytes, smaller than one metrics line. Allocation
  // failure only reduces logging capacity; neither sensor depends on it.
  Serial.setTxBufferSize(4096);
  Serial.setTxTimeoutMs(1);
#endif
  Serial.begin(kSerialBaud);
  delay(kSerialStartupDelayMs);

  if (!runSerialAndMemoryTest()) {
    failClosed("PSRAM validation failed");
  }
  if (!display::begin()) {
    failClosed("LCD initialization failed");
  }

  const bool touchAck = touchDevice.begin();
  const bool touchScanFound = scanTouchI2c();
  touchReady = touchAck && touchScanFound;
  runtimeLog.printf("Touch init: %s (SDA=%d SCL=%d INT=%d address=0x%02X)\n",
                touchReady ? "OK" : "FAIL", board::kTouchSda,
                board::kTouchScl, board::kTouchInterrupt,
                board::kTouchAddress);

  sdWallpaper.begin();

  const uint32_t nowMs = millis();
  ui.begin(static_cast<uint32_t>(ESP.getEfuseMac()));
  ui.setHubSession(esp_random(),esp_random());
  loadDisplayPreferences();
#if defined(SENSOR_SERVICE_ENABLED)
  loadSensorPreferences();
  airHistory.begin();
  ui.setAirHistoryView(&airHistory.view());
#endif
  bridge.begin();
  bridge.setHubModel(&ui.hubModel());
  bridge.setSdWallpaper(&sdWallpaper);
  bridge.setOtaReceiver(&ota);
  wifi.begin(nowMs);
  if (wifi.hasPairingKey()) bridge.setPairingKey(wifi.pairingKey());
  if (touchReady) {
#if defined(TOUCH_TRACE)
    attachInterrupt(board::kTouchInterrupt, countTouchInterrupt, FALLING);
#endif
    touchReady = touchSampler.begin(touchDevice);
    runtimeLog.printf("Touch sampler: %s (period_ms=%lu core=0 queue=%u)\n",
                  touchReady ? "OK" : "FAIL",
                  static_cast<unsigned long>(touch::Sampler::kPeriodMs),
                  static_cast<unsigned>(touch::Sampler::kQueueCapacity));
  }
  metricsWindowStartedMs = nowMs;
  runtimeLog.println("Ambient UI init: OK");
  runtimeLog.println("WALLPAPER_RENDER_PIPELINE: READY");
  bootReadyAtMs = millis();
#if defined(SENSOR_SERVICE_ENABLED)
  sensors::beginSensors();
#else
#if defined(SENSOR_DUAL_PROBE)
  if (sensors::beginDiagnosticLog()) {
#endif
  sensors::beginRadarDiagnostics();
  sensors::beginScd41Diagnostics();
#if defined(SENSOR_DUAL_PROBE)
  } else {
    runtimeLog.println("SENSORS: INIT_FAILED LOG_MUTEX_ALLOCATION");
  }
#endif
#endif
  runtimeLog.println("================================");
}

void loop() {
  const uint32_t nowMs = millis();
  static bool havePreviousLoop = false;
  static uint32_t previousLoopMs = 0;
  if (havePreviousLoop && nowMs - previousLoopMs > maximumMainLoopMs)
    maximumMainLoopMs = nowMs - previousLoopMs;
  previousLoopMs = nowMs;
  havePreviousLoop = true;
  wifi.loop(nowMs);
  updateWifiUi(nowMs);

  bridge.loop(wifi.connected(), wifi.bridgeHost(), wifi.bridgePort(), nowMs);
  ui.setConnectionIndicator(currentConnectionIndicator());
  ui.setHubOnline(bridge.hubReady(),nowMs);
  if (wifiUiActive && !wifi.manualSetupActive() &&
      ((wifi.connected() && bridge.connected()) ||
       (localUiAvailable() && wifi.hasSavedNetwork()))) {
    ui.dismiss("");
    wifiUiActive = false;
  }

  const bool bridgeIsConnected = bridge.connected();
  if (!bridgeWasConnected && bridgeIsConnected && sdWallpaper.ready())
    wallpaperPlayingReported = false;
  if (bridgeWasConnected && !bridgeIsConnected) {
    ui.invalidateHostStatus();
    ui.clearAgentStatus();
    if (ui.state() == UiState::ACTION_REQUIRED) {
      ui.showConnectionLost(nowMs);
    }
  }
  bridgeWasConnected = bridgeIsConnected;
  serviceTouch();
  if(ui.takeSetupExitRequest()){
    wifi.closeDeviceSettings(millis());
    wifiUiActive=false;displayedWifiState=wifi.state();wifiUiInitialized=true;
  }

  static protocol::IncomingMessage incoming;  // Keep the receive model off the loop stack
  while (bridge.popMessage(incoming)) {
    handleIncomingMessage(incoming, millis());
  }

  hub::Result hubResult;
  while(bridge.popHubResult(hubResult))ui.hubResult(hubResult,millis());
  hub::Action hubAction;
  if(!ota.busy() && ui.takeHubAction(hubAction)) {
    if(strcmp(hubAction.action,"device_settings")==0)wifi.openDeviceSettings(millis());
    else if(!bridge.sendHubAction(hubAction))ui.hubSendFailed();
    else runtimeLog.printf("HUB_ACTION_SENT action=%s value=%u\n",hubAction.action,unsigned(hubAction.value));
  }

  ota.tick(millis());
  storage::OtaAck otaAck;
  if (ota.popAck(otaAck)) {
    bridge.sendOtaAck(otaAck);
    if (!ota.busy()) {
      char requestId[24]; snprintf(requestId,sizeof(requestId),"ota-%s",otaAck.transferId);
      ui.dismiss(requestId);
    }
    runtimeLog.printf("OTA state=%s offset=%lu reason=%s\n",otaAck.state,(unsigned long)otaAck.offset,otaAck.reason);
  }
  ui.setOtaProgress(ota.active(),ota.percent(),ota.state() == storage::OtaReceiver::State::Complete);
#if defined(SENSOR_SERVICE_ENABLED)
  updateLocalSensors(millis());
#endif
  if (ota.rebootDue(millis())) ESP.restart();
  if (!bootChecked && millis()-bootReadyAtMs >= 30000 && touchReady && wifi.connected()) {
    esp_ota_img_states_t state;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running,&state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY ||
        esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      bootChecked = true;
      runtimeLog.println("BOOT_HEALTH peripherals=true wifi=true stable_ms=30000");
      bridge.sendOtaBoot();
    }
  }
  if (!bootChecked && millis()-bootReadyAtMs >= 180000) {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(),&state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) esp_ota_mark_app_invalid_rollback_and_reboot();
  }
  if (ota.active()) {
    updateClockAndBacklight(millis()); renderFrame(millis()); delay(1); return;
  }

  sdWallpaper.tick(millis());
  if (sdWallpaper.candidateAvailable() && !ui.overlayActive()) {
    storage::SdWallpaperFrame candidate;
    const bool decoded = sdWallpaper.readCandidate(candidate) &&
        ui.selectVideoWallpaper(candidate.wallpaperId) &&
        ui.decodeVideoWallpaperFrame(candidate.wallpaperId, candidate.jpeg,
                                     candidate.length, millis());
    if (decoded && sdWallpaper.acceptCandidate(millis())) {
      strlcpy(activeWallpaperId, sdWallpaper.activeId(), sizeof(activeWallpaperId));
      localVideoActive = true;
      wallpaperPlayingReported = false;
    } else {
      if (!decoded) sdWallpaper.rejectCandidate();
      if (sdWallpaper.ready()) ui.selectVideoWallpaper(sdWallpaper.activeId());
    }
  }
  if (sdWallpaper.ready()) {
    if (!localVideoActive || strcmp(activeWallpaperId, sdWallpaper.activeId()) != 0) {
      strlcpy(activeWallpaperId, sdWallpaper.activeId(), sizeof(activeWallpaperId));
      ui.selectVideoWallpaper(activeWallpaperId);
      localVideoActive = true;
      wallpaperPlayingReported = false;
    }
    storage::SdWallpaperFrame frame;
    if (sdWallpaper.nextFrame(millis(), ui.overlayActive(), frame)) {
      const uint32_t startedUs = micros();
      if (ui.decodeVideoWallpaperFrame(frame.wallpaperId, frame.jpeg, frame.length, millis())) {
        ++decodedVideoFrameCount;
        accumulatedVideoDecodeUs += micros() - startedUs;
        if (!wallpaperPlayingReported) {
          bridge.sendWallpaperStatus(frame.wallpaperId, "playing", frame.sequence);
          wallpaperPlayingReported = true;
          runtimeLog.printf("Video wallpaper playing: id=%s source=sd sequence=%lu\n",
                        frame.wallpaperId, (unsigned long)frame.sequence);
        }
      } else if (millis() - lastWallpaperDecodeErrorMs >= 5000) {
        bridge.sendWallpaperStatus(frame.wallpaperId, "decode_error", frame.sequence);
        lastWallpaperDecodeErrorMs = millis();
        runtimeLog.println("SD_CACHE playback_error reason=jpeg_decode");
      }
    }
  } else if (localVideoActive) {
    localVideoActive = false;
    activeWallpaperId[0] = '\0';
    ui.clearVideoWallpaper();
  }
  storage::CacheAck cacheAck;
  if (sdWallpaper.popAck(cacheAck)) bridge.sendCacheAck(cacheAck);

  VideoWallpaperFrame videoFrame;
  if (!sdWallpaper.ready() && !ui.overlayActive() && bridge.popVideoFrame(videoFrame)) {
    const uint32_t decodeStartedUs = micros();
    const bool decoded =
        strcmp(activeWallpaperId, videoFrame.wallpaperId) == 0 &&
        ui.decodeVideoWallpaperFrame(videoFrame.wallpaperId, videoFrame.jpeg,
                                     videoFrame.jpegLength, nowMs);
    const uint32_t decodeElapsedUs = micros() - decodeStartedUs;
    if (decoded) {
      ++decodedVideoFrameCount;
      accumulatedVideoDecodeUs += decodeElapsedUs;
      if (!wallpaperPlayingReported) {
        bridge.sendWallpaperStatus(videoFrame.wallpaperId, "playing",
                                   videoFrame.sequence);
        wallpaperPlayingReported = true;
        runtimeLog.printf(
            "Video wallpaper playing: id=%s first_sequence=%lu decode_ms=%.2f\n",
            videoFrame.wallpaperId,
            static_cast<unsigned long>(videoFrame.sequence),
            static_cast<double>(decodeElapsedUs) / 1000.0);
      }
    } else if (lastWallpaperDecodeErrorMs == 0 ||
               nowMs - lastWallpaperDecodeErrorMs >= 5000) {
      bridge.sendWallpaperStatus(videoFrame.wallpaperId, "decode_error",
                                 videoFrame.sequence);
      wallpaperPlayingReported = false;
      lastWallpaperDecodeErrorMs = nowMs;
      runtimeLog.printf("Video wallpaper decode failed: id=%s sequence=%lu\n",
                    videoFrame.wallpaperId,
                    static_cast<unsigned long>(videoFrame.sequence));
    }
  }

  AgentStatusSnapshot agentStatus;
  if (bridge.popAgentStatus(agentStatus)) {
    ui.updateAgentStatus(agentStatus, millis());
  }

  HostStatusSnapshot hostStatus;
  if (bridge.popHostStatus(hostStatus)) {
    ui.updateHostStatus(hostStatus, nowMs);
    runtimeLog.printf(
        "HOST_STATUS_ACCEPTED cpu=%d memory=%d uptime_s=%ld\n",
        hostStatus.hasCpuPercent ? static_cast<int>(hostStatus.cpuPercent) : -1,
        hostStatus.hasMemoryPercent
            ? static_cast<int>(hostStatus.memoryPercent)
            : -1,
        hostStatus.hasUptimeSeconds
            ? static_cast<long>(hostStatus.uptimeSeconds)
            : -1L);
  }

  serviceTouch();
  ui.tick(millis());
#if defined(SENSOR_SERVICE_ENABLED)
  updateLocalSensors(millis());
#endif
  updateClockAndBacklight(millis());
  // Local SD decoding stamps frames later than the start of this loop.
  // Rendering with the old timestamp would underflow the freshness check.
  renderFrame(millis());
  serviceTouch();
  reportMetrics(millis());
  delay(1);
}
