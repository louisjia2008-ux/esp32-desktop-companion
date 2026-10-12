#include "ui_manager.h"

#include <algorithm>
#include <cstring>

#include "board_config.h"
#include "cjk_font.h"
#include "glass_surface.h"
#include "text_layout.h"
#if defined(SENSOR_SERVICE_ENABLED) && SENSOR_SERVICE_ENABLED
#include "environment_chart.h"
#include "sensors/radar_range.h"
#endif

namespace {

constexpr int16_t kCardX = 20;
constexpr int16_t kCardY = 28;
constexpr int16_t kCardWidth = 440;
constexpr int16_t kCardHeight = 268;
constexpr int16_t kButtonY = 210;
constexpr int16_t kButtonWidth = 128;
constexpr int16_t kButtonHeight = 58;
constexpr int16_t kButtonGap = 10;
constexpr uint32_t kTouchPressDebounceMs = 55;
constexpr uint32_t kTouchReleaseDebounceMs = 80;
// Only read errors for this long cannot keep the UI waiting for a release.
constexpr uint32_t kStaleContactMs = 3000;
constexpr uint32_t kHostDashboardTimeoutMs = 15000;
// Ambient gestures are classified when the finger lifts.
constexpr int16_t kSwipeMinimumPx = 70;
constexpr int16_t kTapSlopPx = 20;
constexpr uint32_t kTapMaximumMs = 600;
// Swipe order of the full-screen pages; the wallpaper is the home page.
constexpr UiState kPages[] = {UiState::WALLPAPER, UiState::HOST_STATUS,
#if defined(SENSOR_SERVICE_ENABLED) && SENSOR_SERVICE_ENABLED
                            UiState::ENVIRONMENT,
#endif
                            UiState::MAC_CONTROL, UiState::INFO_CARDS,
                            UiState::TASKS, UiState::USAGE};
constexpr size_t kPageCount = sizeof(kPages) / sizeof(kPages[0]);

int8_t pageIndex(UiState state) {
  for (size_t i = 0; i < kPageCount; ++i) {
    if (kPages[i] == state) return static_cast<int8_t>(i);
  }
  return -1;
}

// Hub page IDs are a separate protocol inside hub::Ui. Navigation may insert
// local pages, but every Hub call must retain these original IDs.
int8_t hubPage(UiState state) {
  switch (state) {
    case UiState::WALLPAPER: return 0;
    case UiState::HOST_STATUS: return 1;
    case UiState::MAC_CONTROL: return 2;
    case UiState::INFO_CARDS: return 3;
    case UiState::TASKS: return 4;
    case UiState::USAGE: return 5;
    default: return -1;
  }
}

UiState hubDestination(uint8_t destination) {
  switch (destination) {
    case 4: return UiState::TASKS;
    case 5: return UiState::USAGE;
    default: return UiState::WALLPAPER;
  }
}

const char* pageName(UiState state) {
  switch (state) {
    case UiState::WALLPAPER: return "home";
    case UiState::HOST_STATUS: return "mac";
    case UiState::ENVIRONMENT: return "environment";
    case UiState::MAC_CONTROL: return "control";
    case UiState::INFO_CARDS: return "cards";
    case UiState::TASKS: return "tasks";
    case UiState::USAGE: return "usage";
    default: return "unknown";
  }
}
constexpr uint32_t kHostStatusStaleMs = 15000;

void copyBounded(char* destination, size_t capacity, const char* source) {
  if (capacity == 0) {
    return;
  }
  if (source == nullptr) {
    destination[0] = '\0';
    return;
  }
  strlcpy(destination, source, capacity);
}

void drawCenteredText(Arduino_Canvas& gfx, const char* text, int16_t centerX,
                      int16_t baselineY, uint8_t size, uint16_t color,
                      int16_t maximumWidth = board::kDisplayWidth - 24) {
  char visible[64] = {};
  copyBounded(visible, sizeof(visible), text);
  size_t length = strlen(visible);
  while (size > 1 && static_cast<int16_t>(length * 6U * size) > maximumWidth) {
    --size;
  }
  const size_t maximumCharacters =
      static_cast<size_t>(maximumWidth / static_cast<int16_t>(6 * size));
  if (length > maximumCharacters && maximumCharacters >= 3) {
    length = maximumCharacters;
    visible[length - 3] = '.';
    visible[length - 2] = '.';
    visible[length - 1] = '.';
    visible[length] = '\0';
  }
  const int16_t width = static_cast<int16_t>(length * 6U * size);
  gfx.setTextSize(size);
  gfx.setTextColor(color);
  gfx.setCursor(centerX - width / 2, baselineY);
  gfx.print(visible);
}

int16_t buttonStartX(size_t actionCount) {
  const int16_t totalWidth =
      static_cast<int16_t>(actionCount * kButtonWidth +
                           (actionCount > 0 ? actionCount - 1 : 0) *
                               kButtonGap);
  return static_cast<int16_t>((board::kDisplayWidth - totalWidth) / 2);
}

void fillLandscapeRect(Arduino_Canvas& gfx, int16_t x, int16_t y, int16_t width,
                       int16_t height, uint16_t color) {
  if (board::kDisplayRotation != 1 || x < 0 || y < 0 || width <= 0 ||
      height <= 0 || x + width > board::kDisplayWidth ||
      y + height > board::kDisplayHeight) {
    gfx.fillRect(x, y, width, height, color);
    return;
  }

  uint16_t* frameBuffer = gfx.getFramebuffer();
  const int16_t nativeStartX = board::kPanelWidth - y - height;
  for (int16_t logicalX = x; logicalX < x + width; ++logicalX) {
    std::fill_n(frameBuffer +
                    static_cast<size_t>(logicalX) * board::kPanelWidth +
                    nativeStartX,
                height, color);
  }
}

void drawMetricCard(Arduino_Canvas& gfx, int16_t x, const char* label,
                    bool available, uint8_t percent, uint16_t accent) {
  constexpr int16_t kY = 72;
  constexpr int16_t kWidth = 210;
  constexpr int16_t kHeight = 158;
  surface::glass(gfx, x, kY, kWidth, kHeight, 0x0863,132,16);
  gfx.drawRoundRect(x, kY, kWidth, kHeight, 16, accent);
  drawCenteredText(gfx, label, x + kWidth / 2, kY + 20, 2, 0xBDF7,
                   kWidth - 24);

  char value[12] = "--";
  if (available) {
    snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(percent));
  }
  drawCenteredText(gfx, value, x + kWidth / 2, kY + 58, 4,
                   available ? WHITE : 0x8410, kWidth - 24);

  constexpr int16_t kBarInset = 18;
  constexpr int16_t kBarY = kY + 122;
  constexpr int16_t kBarHeight = 12;
  constexpr int16_t kBarWidth = kWidth - 2 * kBarInset;
  fillLandscapeRect(gfx, x + kBarInset, kBarY, kBarWidth, kBarHeight,
                    0x2104);
  if (available && percent > 0) {
    const int16_t filled = static_cast<int16_t>(
        (static_cast<uint32_t>(kBarWidth) * percent) / 100U);
    fillLandscapeRect(gfx, x + kBarInset, kBarY, filled, kBarHeight, accent);
  }
}

// ----- CJK text (16 px Unifont, UTF-8) ------------------------------------
// The built-in 6x8 font stays the default for numbers and ASCII screens;
// every CJK drawing call switches fonts and restores the default afterwards.
class CjkFontScope {
 public:
  explicit CjkFontScope(Arduino_Canvas& gfx) : gfx_(gfx) {
    gfx_.setFont(uifont::cjkFont());
    gfx_.setUTF8Print(true);
    gfx_.setTextWrap(false);
  }
  ~CjkFontScope() {
    gfx_.setFont(static_cast<const GFXfont*>(nullptr));
    gfx_.setUTF8Print(false);
    gfx_.setTextWrap(true);
    gfx_.setTextSize(1);
  }

 private:
  Arduino_Canvas& gfx_;
};

void drawCjkLine(Arduino_Canvas& gfx, const char* text, int16_t x,
                 int16_t top, uint8_t scale, uint16_t color) {
  gfx.setTextSize(scale);
  gfx.setTextColor(color);
  gfx.setCursor(x, top + text::kAscent * scale);
  gfx.print(text);
}

// Draws one line centred on centerX, shortened with "..." to maximumWidth.
// Returns the drawn width.
int16_t drawCjkCentered(Arduino_Canvas& gfx, const char* source, int16_t centerX,
                        int16_t top, uint8_t scale, uint16_t color,
                        int16_t maximumWidth) {
  char normalized[192];
  char visible[192];
  text::normalize(source, normalized, sizeof(normalized));
  text::ellipsize(normalized, maximumWidth, scale, visible, sizeof(visible));
  const int16_t width = text::width(visible, scale);
  CjkFontScope font(gfx);
  drawCjkLine(gfx, visible, centerX - width / 2, top, scale, color);
  return width;
}

// Word-aware wrapping for mixed CJK/ASCII text. The last allowed line ends
// with "..." when text remains. Returns the y below the last drawn line.
int16_t drawCjkWrapped(Arduino_Canvas& gfx, const char* source, int16_t x,
                       int16_t top, int16_t width, uint8_t maximumLines,
                       uint16_t color, int16_t lineHeight = 20) {
  char normalized[192];
  text::normalize(source, normalized, sizeof(normalized));
  CjkFontScope font(gfx);
  const char* cursor = normalized;
  while (*cursor == ' ') ++cursor;
  for (uint8_t line = 0; line < maximumLines && *cursor != '\0'; ++line) {
    size_t next = 0;
    const size_t take = text::fitLine(cursor, width, 1, next);
    char buffer[192];
    const size_t copy = std::min(take, sizeof(buffer) - 1);
    memcpy(buffer, cursor, copy);
    buffer[copy] = '\0';
    if (line + 1 == maximumLines && cursor[next] != '\0') {
      // Re-fit the remainder with an ellipsis so truncation is visible.
      char shortened[192];
      text::ellipsize(cursor, width, 1, shortened, sizeof(shortened));
      drawCjkLine(gfx, shortened, x, top, 1, color);
    } else {
      drawCjkLine(gfx, buffer, x, top, 1, color);
    }
    top += lineHeight;
    cursor += next;
  }
  return top;
}

// Darkens a landscape rectangle to a quarter of its brightness so text over
// a moving wallpaper stays readable without hiding the animation.
void darkenLandscapeRect(Arduino_Canvas& gfx, int16_t x, int16_t y,
                         int16_t width, int16_t height) {
  uint16_t* frameBuffer = gfx.getFramebuffer();
  if (frameBuffer == nullptr || board::kDisplayRotation != 1 || x < 0 || y < 0 ||
      width <= 0 || height <= 0 || x + width > board::kDisplayWidth ||
      y + height > board::kDisplayHeight) {
    return;
  }
  const int16_t nativeStartX = board::kPanelWidth - y - height;
  for (int16_t logicalX = x; logicalX < x + width; ++logicalX) {
    uint16_t* pixel = frameBuffer + static_cast<size_t>(logicalX) * board::kPanelWidth +
                      nativeStartX;
    for (int16_t i = 0; i < height; ++i) {
      pixel[i] = static_cast<uint16_t>((pixel[i] & 0xE79CU) >> 2U);
    }
  }
}

const char* eventDisplayName(EventType type) {
  switch (type) {
    case EventType::Info: return "通知";
    case EventType::Success: return "完成";
    case EventType::Warning: return "警告";
    case EventType::Error: return "错误";
    case EventType::ActionRequired: return "需要你的选择";
  }
  return "";
}

int16_t drawWrappedText(Arduino_Canvas& gfx, const char* text, int16_t x,
                        int16_t y, int16_t width, uint8_t size,
                        uint16_t color, uint8_t maximumLines) {
  if (text == nullptr || text[0] == '\0') {
    return y;
  }

  const size_t maxCharacters =
      static_cast<size_t>(width / static_cast<int16_t>(6 * size));
  if (maxCharacters < 2) {
    return y;
  }

  gfx.setTextSize(size);
  gfx.setTextColor(color);
  const char* cursor = text;
  for (uint8_t lineIndex = 0;
       lineIndex < maximumLines && cursor[0] != '\0'; ++lineIndex) {
    while (*cursor == ' ') {
      ++cursor;
    }

    size_t remaining = strlen(cursor);
    size_t take = remaining < maxCharacters ? remaining : maxCharacters;
    if (take < remaining) {
      size_t breakAt = take;
      while (breakAt > 0 && cursor[breakAt] != ' ') {
        --breakAt;
      }
      if (breakAt > 0) {
        take = breakAt;
      }
    }

    char line[64] = {};
    const size_t safeTake = take < sizeof(line) - 1 ? take : sizeof(line) - 1;
    memcpy(line, cursor, safeTake);
    line[safeTake] = '\0';
    if (lineIndex + 1 == maximumLines && take < remaining && safeTake >= 3) {
      line[safeTake - 3] = '.';
      line[safeTake - 2] = '.';
      line[safeTake - 1] = '.';
    }
    gfx.setCursor(x, y);
    gfx.print(line);
    y += static_cast<int16_t>(8 * size + 3);
    cursor += take;
  }
  return y;
}

const char* fallbackTitle(UiState state) {
  switch (state) {
    case UiState::NOTIFICATION:
      return "NOTIFICATION";
    case UiState::ACTION_REQUIRED:
      return "ACTION REQUIRED";
    case UiState::SUCCESS:
      return "SUCCESS";
    case UiState::ERROR:
      return "ERROR";
    case UiState::WARNING:
      return "WARNING";
    case UiState::WIFI_CONNECTING:
      return "WI-FI CONNECTING";
    case UiState::SETUP:
      return "SETUP MODE";
    case UiState::MAC_CONTROL: return "MAC CONTROL";
    case UiState::INFO_CARDS: return "CARDS";
    case UiState::TASKS: return "TASKS";
    case UiState::USAGE: return "USAGE";
    case UiState::HOST_STATUS:
      return "MAC STATUS";
    case UiState::ENVIRONMENT:
      return "ENVIRONMENT";
    case UiState::WALLPAPER:
      return "";
  }
  return "";
}

}  // namespace

void UiManager::begin(uint32_t wallpaperSeed) {
  wallpaper_.begin(wallpaperSeed);
  setWallpaper();
}

void UiManager::setConnectionIndicator(ConnectionIndicator indicator) {
  indicator_ = indicator;
}

void UiManager::showSetup(const char* accessPointName,
                          const char* accessPointPassword,
                          const char* failureMessage) {
  state_ = UiState::SETUP;
  copyBounded(setupAccessPoint_, sizeof(setupAccessPoint_), accessPointName);
  copyBounded(setupAccessPointPassword_,
              sizeof(setupAccessPointPassword_), accessPointPassword);
  copyBounded(setupFailure_, sizeof(setupFailure_), failureMessage);
  clearExpiry();
  armTouchReleaseBarrier();
}

void UiManager::showWifiConnecting(uint8_t attemptNumber,
                                   bool waitingForRetry,
                                   const char* failureMessage) {
  state_ = UiState::WIFI_CONNECTING;
  copyBounded(title_, sizeof(title_),
              waitingForRetry ? "WI-FI RETRY" : "WI-FI CONNECTING");
  if (failureMessage != nullptr && failureMessage[0] != '\0') {
    snprintf(message_, sizeof(message_),
             waitingForRetry ? "Retry %u pending. Previous: %s"
                             : "Attempt %u connecting. Previous: %s",
             static_cast<unsigned>(attemptNumber), failureMessage);
  } else {
    snprintf(message_, sizeof(message_),
             "Attempt %u. Looking for the saved 2.4 GHz network...",
             static_cast<unsigned>(attemptNumber));
  }
  requestId_[0] = '\0';
  actionCount_ = 0;
  clearExpiry();
  armTouchReleaseBarrier();
}

void UiManager::showWifiConnected(const char* localIp, const char* bridgeHost,
                                  uint16_t bridgePort, uint32_t nowMs) {
  state_ = UiState::WIFI_CONNECTING;
  copyBounded(title_, sizeof(title_), "WI-FI CONNECTED");
  snprintf(message_, sizeof(message_),
           "LAN %s. Connecting to Bridge %s:%u...",
           localIp == nullptr ? "" : localIp,
           bridgeHost == nullptr ? "" : bridgeHost,
           static_cast<unsigned>(bridgePort));
  requestId_[0] = '\0';
  actionCount_ = 0;
  // This is a truthful transient notice, not a modal that blocks the ambient
  // wallpaper forever while the Bridge is unavailable.
  setExpiry(nowMs, 3500);
  armTouchReleaseBarrier();
}

UiState UiManager::state() const {
  if (!events_.active()) return state_;
  switch (events_.current().type) {
    case EventType::Info: return UiState::NOTIFICATION;
    case EventType::Success: return UiState::SUCCESS;
    case EventType::Warning: return UiState::WARNING;
    case EventType::Error: return UiState::ERROR;
    case EventType::ActionRequired: return UiState::ACTION_REQUIRED;
  }
  return state_;
}

void UiManager::syncEvent(uint32_t nowMs) {
  if (eventRevision_ == events_.revision()) return;
  eventRevision_ = events_.revision();
  armTouchReleaseBarrierAt(nowMs, false);
  if (events_.active()) {
    overlayWallpaperMs_ = nowMs;
    actionDispatched_ = events_.phase() == ActionPhase::AwaitingAck;
    sending_ = actionDispatched_;
  } else {
    Serial.printf("UI_RESTORED base=%s\n", state_ == UiState::HOST_STATUS ?
        "MAC_STATUS" : state_ == UiState::WALLPAPER ? "WALLPAPER" : pageName(state_));
  }
}

bool UiManager::receiveEvent(const CompanionEvent& event, uint32_t nowMs) {
  const bool received = events_.receive(event, nowMs);
  syncEvent(nowMs);
  return received;
}

bool UiManager::acknowledgeAction(const char* requestId, const char* actionId,
                                   bool accepted, uint32_t nowMs) {
  const bool matched = events_.acknowledge(requestId, actionId, accepted, nowMs);
  syncEvent(nowMs);
  return matched;
}

void UiManager::actionSendFailed(uint32_t nowMs) {
  events_.sendFailed();
  syncEvent(nowMs);
}

bool UiManager::showNotification(const char* title, const char* message,
                                  uint32_t nowMs) {
  CompanionEvent event;
  snprintf(event.id, sizeof(event.id), "legacy-%lu", (unsigned long)++legacyEventCounter_);
  copyBounded(event.title, sizeof(event.title), title);
  copyBounded(event.message, sizeof(event.message), message);
  event.autoDismissMs = 6000;
  return receiveEvent(event, nowMs);
}

bool UiManager::showStatus(const char* status, const char* title,
                            const char* message, uint32_t nowMs) {
  CompanionEvent event;
  snprintf(event.id, sizeof(event.id), "legacy-%lu", (unsigned long)++legacyEventCounter_);
  copyBounded(event.title, sizeof(event.title), title);
  copyBounded(event.message, sizeof(event.message), message);
  if (status != nullptr && (strcmp(status, "success") == 0 ||
      strcmp(status, "complete") == 0 || strcmp(status, "completed") == 0)) {
    event.type = EventType::Success;
    event.autoDismissMs = 4500;
  } else if (status != nullptr && (strcmp(status, "error") == 0 || strcmp(status, "failed") == 0)) {
    event.type = EventType::Error;
  } else if (status != nullptr && strcmp(status, "warning") == 0) {
    event.type = EventType::Warning;
  } else {
    event.autoDismissMs = 4500;
  }
  return receiveEvent(event, nowMs);
}

bool UiManager::showActionRequired(const char* requestId, const char* title,
                                   const char* message,
                                   const UiActionOption* actions,
                                   size_t actionCount) {
  if (requestId == nullptr || requestId[0] == '\0' || actions == nullptr ||
      actionCount == 0 || actionCount > kMaximumActions) return false;
  CompanionEvent event;
  event.type = EventType::ActionRequired;
  copyBounded(event.id, sizeof(event.id), requestId);
  copyBounded(event.title, sizeof(event.title), title);
  copyBounded(event.message, sizeof(event.message), message);
  event.actionCount = actionCount;
  for (size_t i = 0; i < actionCount; ++i) event.actions[i] = actions[i];
  return receiveEvent(event, millis());
}

void UiManager::dismiss(const char* requestId) {
  if (requestId != nullptr && requestId[0] != '\0') {
    events_.dismiss(requestId, millis());
    syncEvent(millis());
  } else if (state_ == UiState::SETUP || state_ == UiState::WIFI_CONNECTING) {
    setWallpaper();
  }
}

void UiManager::showConnectionLost(uint32_t nowMs) {
  events_.disconnected();
  syncEvent(nowMs);
}

bool UiManager::selectVideoWallpaper(const char* wallpaperId) {
  return wallpaper_.selectVideo(wallpaperId);
}

bool UiManager::decodeVideoWallpaperFrame(const char* wallpaperId,
                                          const uint8_t* jpeg,
                                          size_t jpegLength,
                                          uint32_t nowMs) {
  return wallpaper_.decodeVideoFrame(wallpaperId, jpeg, jpegLength, nowMs);
}

bool UiManager::videoWallpaperFresh(uint32_t nowMs) const {
  return wallpaper_.videoFresh(nowMs);
}

void UiManager::updateHostStatus(const HostStatusSnapshot& status,
                                 uint32_t nowMs) {
  hostStatus_ = status;
  hostStatusUpdatedMs_ = nowMs;
  hostStatusReceived_ = true;
}

void UiManager::updateAgentStatus(const AgentStatusSnapshot& status,
                                  uint32_t nowMs) {
  updateAgentRotationPause(nowMs);
  const int8_t previous = visibleAgentSession(nowMs);
  const uint32_t elapsed = agentRotationElapsedMs_ +
      (agentRotationPaused_ ? 0U : nowMs - agentRotationEpochMs_);
  int8_t selected = -1;
  int8_t newWaiting = -1;
  for (size_t i = 0; i < status.count; ++i) {
    const AgentSession& incoming = status.sessions[i];
    bool wasWaiting = false;
    for (size_t j = 0; j < agentStatus_.count && agentStatusReceived_; ++j) {
      const AgentSession& old = agentStatus_.sessions[j];
      if (old.source == incoming.source && strcmp(old.id, incoming.id) == 0) {
        wasWaiting = old.state == AgentState::Waiting;
        if (static_cast<int8_t>(j) == previous) selected = static_cast<int8_t>(i);
        break;
      }
    }
    if (incoming.state == AgentState::Waiting && !wasWaiting && newWaiting < 0) {
      newWaiting = static_cast<int8_t>(i);
    }
  }
  agentRotationIndex_ = newWaiting >= 0 ? newWaiting : selected >= 0 ? selected : 0;
  agentRotationElapsedMs_ = newWaiting >= 0 || selected < 0 ? 0U : elapsed % 4000U;
  agentRotationEpochMs_ = nowMs;
  agentStatus_ = status;
  agentStatusReceivedMs_ = nowMs;
  agentStatusReceived_ = true;
}

size_t UiManager::visibleAgentCount(uint32_t nowMs) const {
  // The Bridge refreshes every 5 s; silence for 30 s means it is gone.
  constexpr uint32_t kAgentStatusStaleMs = 30000;
  constexpr uint32_t kDoneVisibleMs = 4000;
  if (!agentStatusReceived_ || nowMs - agentStatusReceivedMs_ > kAgentStatusStaleMs) {
    return 0;
  }
  const uint32_t sinceMs = nowMs - agentStatusReceivedMs_;
  size_t count = 0;
  for (size_t i = 0; i < agentStatus_.count; ++i) {
    const AgentSession& session = agentStatus_.sessions[i];
    if (session.state == AgentState::Working || session.state == AgentState::Waiting) {
      ++count;
    }
    if ((session.state == AgentState::Done || session.state == AgentState::Stopped) &&
        static_cast<uint64_t>(session.ageS) * 1000U + sinceMs < kDoneVisibleMs) {
      ++count;
    }
  }
  return count;
}

int8_t UiManager::visibleAgentSession(uint32_t nowMs) const {
  if (visibleAgentCount(nowMs) == 0) return -1;
  const uint32_t sinceMs = nowMs - agentStatusReceivedMs_;
  uint8_t eligible[AgentStatusSnapshot::kMaximumSessions] = {};
  size_t count = 0, start = 0;
  for (size_t i = 0; i < agentStatus_.count; ++i) {
    const AgentSession& s = agentStatus_.sessions[i];
    if (s.state == AgentState::Working || s.state == AgentState::Waiting ||
        ((s.state == AgentState::Done || s.state == AgentState::Stopped) &&
         static_cast<uint64_t>(s.ageS) * 1000U + sinceMs < 4000U)) {
      if (i == agentRotationIndex_) start = count;
      eligible[count++] = static_cast<uint8_t>(i);
    }
  }
  const uint32_t elapsed = agentRotationElapsedMs_ +
      (agentRotationPaused_ ? 0U : nowMs - agentRotationEpochMs_);
  return static_cast<int8_t>(eligible[(start + elapsed / 4000U) % count]);
}

void UiManager::updateAgentRotationPause(uint32_t nowMs) {
  const bool paused = events_.active() || state_ != UiState::WALLPAPER || otaProgressActive_;
  if (paused == agentRotationPaused_) return;
  if (paused) agentRotationElapsedMs_ += nowMs - agentRotationEpochMs_;
  agentRotationEpochMs_ = nowMs;
  agentRotationPaused_ = paused;
}

void UiManager::invalidateHostStatus() {
  hostStatus_ = HostStatusSnapshot{};
  hostStatusUpdatedMs_ = 0;
  hostStatusReceived_ = false;
}

void UiManager::tick(uint32_t nowMs) {
  const int8_t page=hubPage(state_);
  if(page>=0)hub_.tick(page,nowMs);
  if((page>=2 || state_==UiState::ENVIRONMENT) &&
     (hub_.busy() || physicalTouchDown_ || events_.active())) setExpiry(nowMs,60000);
  if (expiryActive_ &&
      static_cast<int32_t>(nowMs - expiresAtMs_) >= 0) {
    setWallpaper();
  }
  events_.tick(nowMs);
  syncEvent(nowMs);
  updateAgentRotationPause(nowMs);
}

bool UiManager::handleTouch(bool rawTouchDown, const TouchPoint& point,
                            uint32_t nowMs, UiActionEvent& event) {
  event = UiActionEvent{};
  if (otaProgressActive_) return false;
  // Buffered input from before a notification/action appeared belongs to the
  // old screen. In particular an old UP must not unlock a new modal's barrier.
  if (static_cast<int32_t>(nowMs - touchSamplesNotBeforeMs_) < 0) {
    return false;
  }

  // Track physical contact independently of every screen's gesture latch.
  // Transitions arm a global release barrier, so a finger already held down
  // can never become a fresh gesture merely because the visible state changed.
  if (rawTouchDown) {
    physicalTouchDown_ = true;
    physicalLastRawTouchMs_ = nowMs;
    lastInteractionMs_ = nowMs;
  } else if (physicalTouchDown_ &&
             nowMs - physicalLastRawTouchMs_ >= kTouchReleaseDebounceMs) {
    physicalTouchDown_ = false;
    physicalLastRawTouchMs_ = 0;
  }

  if (blockedUntilRelease_) {
    if (!rawTouchDown && !physicalTouchDown_) {
      blockedUntilRelease_ = false;
      clearTouchLatch(false);
      clearAmbientTouchLatch();
    }
    return false;
  }

  if (!events_.active() && state_ == UiState::SETUP) {
    if (rawTouchDown) {
      if (!ambientTouchDown_) { ambientTouchDown_=true;ambientStart_=point;ambientWakeOnly_=screenDimmed_; }
      ambientLast_=point;ambientLastRawTouchMs_=nowMs;
    } else if (ambientTouchDown_ && nowMs-ambientLastRawTouchMs_>=kTouchReleaseDebounceMs) {
      const TouchPoint start=ambientStart_,end=ambientLast_;
      const bool tap=std::abs(int(end.x)-int(start.x))<kTapSlopPx&&std::abs(int(end.y)-int(start.y))<kTapSlopPx;
      const bool exit=tap&&!ambientWakeOnly_&&start.x>=132&&start.x<348&&start.y>=252&&start.y<286;
      clearAmbientTouchLatch();
      if(exit){setupExitRequested_=true;setWallpaper();Serial.println("SETUP_EXIT requested");}
      armTouchReleaseBarrierAt(nowMs,!physicalTouchDown_);
    }
    return false;
  }

  if (!events_.active() && pageIndex(state_) >= 0) {
    if (rawTouchDown) {
      ambientLastRawTouchMs_ = nowMs;
      if (!ambientTouchDown_) {
        ambientTouchDown_ = true;
        ambientTouchStartedMs_ = nowMs;
        ambientStart_ = point;
        // On a dimmed screen the first gesture only brings the light back.
        ambientWakeOnly_ = screenDimmed_;
        const int8_t page = hubPage(state_);
        ambientControlConsumed_=!ambientWakeOnly_ && page >= 0 &&
            (hub_.hit(page,point)||hub_.scrollHit(page,point));
        if(ambientControlConsumed_)hub_.beginTouch(page,point,nowMs);
      }
      ambientLast_ = point;
      if(ambientControlConsumed_)hub_.drag(hubPage(state_),point);
      if(hubPage(state_)>=2 || state_==UiState::ENVIRONMENT)setExpiry(nowMs,60000);
    } else if (ambientTouchDown_ &&
               nowMs - ambientLastRawTouchMs_ >= kTouchReleaseDebounceMs) {
      handleAmbientGesture(nowMs);
    }
    return false;
  }

  if (!events_.active() || events_.phase() == ActionPhase::AwaitingAck) return false;
  bool dispatch = false;
  bool activateButton = false;
  if (rawTouchDown) {
    lastRawTouchMs_ = nowMs;
    const int8_t hit = buttonAt(point);
    if (!logicalTouchDown_) {
      logicalTouchDown_ = true;
      touchStartedMs_ = nowMs;
      pressedButton_ = hit;
    } else if (hit != pressedButton_) {
      pressedButton_ = -1;
    }
    const uint32_t required =
        std::max<uint32_t>(kTouchPressDebounceMs, holdDuration(pressedButton_));
    activateButton = pressedButton_ >= 0 && nowMs - touchStartedMs_ >= required;
  } else if (logicalTouchDown_ && nowMs - lastRawTouchMs_ >= kTouchReleaseDebounceMs) {
    // A complete short tap retained by the independent sampler also counts.
    // Hold buttons measure the real contact (first to last DOWN sample), not
    // the release-detection time, so a quick tap can never confirm them.
    const uint16_t hold = holdDuration(pressedButton_);
    if (hold > 0) {
      activateButton = pressedButton_ >= 0 && lastRawTouchMs_ - touchStartedMs_ >= hold;
      if (!activateButton && pressedButton_ >= 0) holdHintUntilMs_ = nowMs + 1500;
    } else {
      activateButton = pressedButton_ >= 0 && nowMs - touchStartedMs_ >= kTouchPressDebounceMs;
    }
    if (!activateButton) clearTouchLatch(false);
  }
  if (!activateButton) return false;
  const int8_t button = pressedButton_;
  const bool connected = indicator_ == ConnectionIndicator::BRIDGE_CONNECTED;
  if (events_.current().type != EventType::ActionRequired) {
    events_.dismiss(events_.current().id, millis());
  } else if (events_.phase() == ActionPhase::Unconfirmed) {
    if (button == 0) dispatch = events_.retry(connected, millis(), event);
    else events_.dismiss(events_.current().id, millis());
  } else if (size_t(button) < events_.current().actionCount) {
    dispatch = events_.choose(size_t(button), connected, millis(), event);
  } else {
    events_.dismiss(events_.current().id, millis());
  }
  syncEvent(millis());
  return dispatch;
}

uint16_t UiManager::holdDuration(int8_t button) const {
  if (button < 0 || !events_.active() ||
      events_.current().type != EventType::ActionRequired ||
      events_.phase() == ActionPhase::Unconfirmed ||
      static_cast<size_t>(button) >= events_.current().actionCount) {
    return 0;
  }
  return events_.current().actions[button].holdMs;
}

void UiManager::render(Arduino_Canvas& gfx, uint32_t nowMs) {
  clockRenderUs_ = 0;
  if (otaProgressActive_) {
    fillLandscapeRect(gfx, 0, 0, board::kDisplayWidth, board::kDisplayHeight, 0x0863);
    CjkFontScope font(gfx);
    const int16_t center = board::kDisplayWidth / 2;
    drawCjkCentered(gfx, otaRebooting_ ? "校验通过,即将重启" : "正在更新固件", center, 90, 1, WHITE, 400);
    char progress[16]; snprintf(progress, sizeof(progress), "%u%%", unsigned(otaPercent_));
    drawCjkCentered(gfx, progress, center, 130, 2, WHITE, 300);
    fillLandscapeRect(gfx, center-160, 190, 320, 8, 0x2945);
    fillLandscapeRect(gfx, center-160, 190, 320 * otaPercent_ / 100, 8, 0x7DFF);
    drawCjkCentered(gfx, "请保持供电", center, 226, 1, 0xBDF7, 320);
    return;
  }
  // Redraw the frozen wallpaper to avoid repeatedly dimming a composited frame.
  wallpaper_.render(gfx, events_.active() ? overlayWallpaperMs_ : nowMs);
  if (events_.active()) {
    drawOverlay(gfx);
    drawEvent(gfx, nowMs);
  } else if (state_ == UiState::SETUP) {
    drawOverlay(gfx);
    drawSetup(gfx);
  } else if (state_ == UiState::HOST_STATUS) {
    drawHostStatus(gfx, nowMs);
    drawPageDots(gfx);
#if defined(SENSOR_SERVICE_ENABLED) && SENSOR_SERVICE_ENABLED
  } else if (state_ == UiState::ENVIRONMENT) {
    drawEnvironment(gfx, nowMs);
    drawPageDots(gfx);
#endif
  } else if (hubPage(state_)>=2) {
    hub_.render(gfx,hubPage(state_),nowMs);
  } else if (state_ != UiState::WALLPAPER) {
    drawOverlay(gfx);
    drawModal(gfx);
  } else {
    if (clockVisible_ && localTimeValid_) {
      const uint32_t started = micros();
      glassClock_.render(gfx, localTime_);
      clockRenderUs_ = micros() - started;
    }
    drawAgentHud(gfx, nowMs);
    hub_.renderHome(gfx,nowMs);
  }
  drawConnectionIndicator(gfx);
}

void UiManager::setWallpaper() {
  state_ = UiState::WALLPAPER;
  title_[0] = '\0';
  message_[0] = '\0';
  requestId_[0] = '\0';
  actionCount_ = 0;
  clearExpiry();
  armTouchReleaseBarrier();
}

void UiManager::armTouchReleaseBarrier() {
  armTouchReleaseBarrierAt(millis(), false);
}

void UiManager::armTouchReleaseBarrierAt(uint32_t nowMs,
                                        bool releaseConfirmed) {
  blockedUntilRelease_ = !releaseConfirmed;
  touchSamplesNotBeforeMs_ = nowMs;
  clearTouchLatch();
  clearAmbientTouchLatch();
}

void UiManager::resetTouchAfterGap(uint32_t nowMs) {
  // A delayed overflow marker also belongs to its original screen. It must
  // not move a newer modal's input cutoff backwards and unlock old gestures.
  if (static_cast<int32_t>(nowMs - touchSamplesNotBeforeMs_) < 0) {
    return;
  }
  const bool wasDispatched = actionDispatched_;
  const bool wasSending = sending_;
  physicalTouchDown_ = true;
  physicalLastRawTouchMs_ = nowMs;
  armTouchReleaseBarrierAt(nowMs, false);
  // Input loss must never re-enable an action that was already dispatched.
  actionDispatched_ = wasDispatched;
  sending_ = wasSending;
}

void UiManager::handleAmbientGesture(uint32_t nowMs) {
  const int16_t dx = static_cast<int16_t>(ambientLast_.x) - static_cast<int16_t>(ambientStart_.x);
  const int16_t dy = static_cast<int16_t>(ambientLast_.y) - static_cast<int16_t>(ambientStart_.y);
  const uint32_t contactMs = ambientLastRawTouchMs_ - ambientTouchStartedMs_;
  const bool wakeOnly = ambientWakeOnly_;
  const bool controlConsumed=ambientControlConsumed_;
  const TouchPoint start=ambientStart_,end=ambientLast_;
  clearAmbientTouchLatch();
  const int16_t absX = dx < 0 ? -dx : dx, absY = dy < 0 ? -dy : dy;
  const int8_t page = hubPage(state_);
  const bool pageSwipe=absX>=kSwipeMinimumPx&&absX>2*absY&&
      (page < 0 || !hub_.hit(page,start));
  if(controlConsumed&&!wakeOnly&&!pageSwipe){
    const uint8_t destination=hub_.endTouch(page,start,end,nowMs);
    if(destination==1){setWallpaper();Serial.println("PAGE home");}
    else if(destination==4 || destination==5){
      state_=hubDestination(destination);setExpiry(nowMs,60000);
      hub_.enter(hubPage(state_),nowMs);Serial.printf("PAGE %s\n",pageName(state_));
    }
    armTouchReleaseBarrierAt(nowMs,!physicalTouchDown_);return;
  }
  if (wakeOnly) {
    Serial.println("GESTURE wake");
  } else if (absX >= kSwipeMinimumPx && absX > 2 * absY) {
    showPage(dx < 0 ? 1 : -1, nowMs);  // finger moves left: next page
  } else if (absX < kTapSlopPx && absY < kTapSlopPx && contactMs < kTapMaximumMs) {
    if (state_ == UiState::WALLPAPER) {
      clockVisible_ = !clockVisible_;
      clockVisibilityChanged_ = true;
      Serial.printf("CLOCK_VISIBLE=%d\n", clockVisible_ ? 1 : 0);
    } else if(state_==UiState::HOST_STATUS) {
      setWallpaper();
      Serial.println("PAGE home");
#if defined(SENSOR_SERVICE_ENABLED) && SENSOR_SERVICE_ENABLED
    } else if(state_==UiState::ENVIRONMENT) {
      const int8_t button = sensorButtonAt(start);
      if (button >= 0 && button == sensorButtonAt(end)) {
        activateSensorButton(button, nowMs);
      }
#endif
    }
  } else {
    return;  // long press or diagonal drag: deliberately ignored
  }
  // The finger has already lifted; only later buffered input is fenced off.
  armTouchReleaseBarrierAt(nowMs, !physicalTouchDown_);
}

void UiManager::showPage(int8_t direction, uint32_t nowMs) {
  const int8_t target = pageIndex(state_) + direction;
  if (target < 0 || target >= static_cast<int8_t>(kPageCount)) return;
  if (kPages[target] == UiState::WALLPAPER) {
    setWallpaper();
    Serial.println("PAGE home");
    return;
  }
  state_ = kPages[target];
#if defined(SENSOR_SERVICE_ENABLED) && SENSOR_SERVICE_ENABLED
  if (state_ == UiState::ENVIRONMENT) {
    environmentView_ = EnvironmentView::Live;
    historyDaysAgo_ = 0;
  }
#endif
  setExpiry(nowMs, state_==UiState::HOST_STATUS?kHostDashboardTimeoutMs:60000);
  const int8_t page = hubPage(state_);
  if (page >= 0) hub_.enter(page,nowMs);
  Serial.printf("PAGE %s\n",pageName(state_));
}

void UiManager::setLocalTime(bool valid, const struct tm& local) {
  localTimeValid_ = valid;
  if (valid) localTime_ = local;
}

bool UiManager::releaseStaleContact(uint32_t nowMs) {
  if (!physicalTouchDown_ ||
      static_cast<int32_t>(nowMs - physicalLastRawTouchMs_) <
          static_cast<int32_t>(kStaleContactMs)) {
    return false;
  }
  // The finger is not demonstrably down any more. Unlock input without
  // treating this as a tap: a half-seen gesture must not choose an action.
  physicalTouchDown_ = false;
  physicalLastRawTouchMs_ = 0;
  blockedUntilRelease_ = false;
  clearTouchLatch(false);
  clearAmbientTouchLatch();
  return true;
}

void UiManager::clearExpiry() {
  expiresAtMs_ = 0;
  expiryActive_ = false;
}

void UiManager::setExpiry(uint32_t nowMs, uint32_t durationMs) {
  expiresAtMs_ = nowMs + durationMs;
  expiryActive_ = true;
}

void UiManager::clearTouchLatch(bool resetActionState) {
  logicalTouchDown_ = false;
  if (resetActionState) {
    actionDispatched_ = false;
    sending_ = false;
  }
  pressedButton_ = -1;
  touchStartedMs_ = 0;
  lastRawTouchMs_ = 0;
}

void UiManager::clearAmbientTouchLatch() {
  ambientTouchDown_ = false;
  ambientToggleConsumed_ = false;
  ambientTouchStartedMs_ = 0;
  ambientLastRawTouchMs_ = 0;
}

size_t UiManager::eventButtonCount() const {
  if (!events_.active()) return 0;
  if (events_.current().type != EventType::ActionRequired) return 1;
  if (events_.phase() == ActionPhase::Unconfirmed) return 2;
  return events_.current().actionCount + (events_.phase() == ActionPhase::Rejected ? 1 : 0);
}

UiManager::ButtonRect UiManager::eventButtonRect(size_t index) const {
  const size_t count = eventButtonCount();
  const bool portrait = board::kDisplayHeight > board::kDisplayWidth;
  const size_t columns = portrait ? 1 : (count > 3 ? 2 : count);
  if (columns == 0 || index >= count) return {0, 0, 0, 0};
  const size_t rows = (count + columns - 1) / columns;
  const int16_t gap = rows > 1 ? 8 : 10;
  const int16_t height = rows > 1 ? 48 : 58;
  const int16_t bottomMargin = rows > 1 ? 36 : 52;
  const int16_t availableWidth = board::kDisplayWidth - 80;
  const int16_t width = portrait ? availableWidth :
      std::min<int16_t>(count > 3 ? 194 : 198,
                       (availableWidth - (columns - 1) * gap) / columns);
  const int16_t startX = (board::kDisplayWidth -
                          (columns * width + (columns - 1) * gap)) / 2;
  const int16_t startY = board::kDisplayHeight - bottomMargin -
                         (rows * height + (rows - 1) * gap);
  return {int16_t(startX + (index % columns) * (width + gap)),
          int16_t(startY + (index / columns) * (height + gap)), width, height};
}

int8_t UiManager::buttonAt(const TouchPoint& point) const {
  for (size_t index = 0; index < eventButtonCount(); ++index) {
    const ButtonRect r = eventButtonRect(index);
    if (point.x >= r.x && point.x < r.x + r.width &&
        point.y >= r.y && point.y < r.y + r.height) return int8_t(index);
  }
  return -1;
}

void UiManager::drawEvent(Arduino_Canvas& gfx, uint32_t nowMs) const {
  const CompanionEvent& event = events_.current();
  const bool action = event.type == EventType::ActionRequired;
  const ActionPhase phase = events_.phase();
  const uint16_t accent = event.type == EventType::Success ? 0x3EC8 :
      event.type == EventType::Error ? 0xF9E7 :
      event.type == EventType::Warning || action ? 0xFD20 : 0x4D7F;
  const int16_t x = 20, y = 28, width = board::kDisplayWidth - 40;
  const int16_t height = board::kDisplayHeight - 52;
  const int16_t centerX = board::kDisplayWidth / 2;
  fillLandscapeRect(gfx, x, y, width, height, 0x0863);
  gfx.drawRoundRect(x, y, width, height, 18, accent);

  char header[80];
  snprintf(header, sizeof(header), event.source[0] ? "%s  -  %s" : "%s",
           eventDisplayName(event.type), event.source);
  drawCjkCentered(gfx, header, centerX, y + 10, 1, accent, width - 40);

  const int16_t buttonY = eventButtonRect(0).y;
  // Large title when it fits on one line and leaves room for the message.
  const bool roomy = buttonY - y >= 150;
  const uint8_t titleScale =
      roomy && text::width(event.title, 2) <= width - 40 ? 2 : 1;
  drawCjkCentered(gfx, event.title, centerX, y + 34, titleScale, WHITE, width - 40);

  const bool failed = action && (phase == ActionPhase::Unconfirmed || phase == ActionPhase::Rejected);
  const bool hintVisible = action && phase == ActionPhase::Choice &&
                           static_cast<int32_t>(holdHintUntilMs_ - nowMs) > 0;
  const int16_t messageY = y + 34 + text::kGlyphHeight * titleScale + 10;
  const int16_t messageBottom = buttonY - (failed || hintVisible || phase == ActionPhase::AwaitingAck ? 30 : 8);
  const uint8_t lines = messageBottom > messageY ? (messageBottom - messageY) / 20 : 0;
  drawCjkWrapped(gfx, event.message, x + 24, messageY, width - 48, lines, 0xCE79);

  const char* status = "";
  if (action) {
    status = phase == ActionPhase::AwaitingAck ? "发送中,等待 Mac 确认..." :
        phase == ActionPhase::Unconfirmed ?
          (indicator_ == ConnectionIndicator::BRIDGE_CONNECTED ?
             "Mac 未确认 - 可重试或关闭" : "连接已断开 - 操作未确认") :
        phase == ActionPhase::Rejected ? "Mac 拒绝了这个选择 - 请重选或关闭" :
        hintVisible ? "请按住按钮直到进度条走满" : "";
  }
  if (status[0] != '\0') {
    drawCjkCentered(gfx, status, centerX, buttonY - 24, 1,
                    phase == ActionPhase::AwaitingAck ? 0xBDF7 : accent, width - 32);
  }

  for (size_t i = 0; i < eventButtonCount(); ++i) {
    const ButtonRect r = eventButtonRect(i);
    const char* label = "关闭";
    uint16_t hold = 0;
    if (action && phase == ActionPhase::Unconfirmed) {
      label = i == 0 ? "重试" : "关闭";
    } else if (action && i < event.actionCount) {
      label = event.actions[i].label;
      hold = event.actions[i].holdMs;
    }
    const bool selected = action && phase == ActionPhase::AwaitingAck &&
        i < event.actionCount && strcmp(events_.selectedAction(), event.actions[i].id) == 0;
    fillLandscapeRect(gfx, r.x, r.y, r.width, r.height, selected ? 0x3D7F : 0x18E8);
    if (hold > 0 && logicalTouchDown_ && pressedButton_ == static_cast<int8_t>(i) &&
        !actionDispatched_) {
      const uint32_t held = std::min<uint32_t>(nowMs - touchStartedMs_, hold);
      const int16_t filled = static_cast<int16_t>(
          static_cast<uint32_t>(r.width) * held / hold);
      if (filled > 0) fillLandscapeRect(gfx, r.x, r.y, filled, r.height, 0x3D7F);
    }
    gfx.drawRoundRect(r.x, r.y, r.width, r.height, 10, selected ? WHITE : accent);
    char buttonText[64];
    snprintf(buttonText, sizeof(buttonText), hold > 0 && !selected ? "%s (长按)" : "%s",
             selected ? "发送中..." : label);
    drawCjkCentered(gfx, buttonText, r.x + r.width / 2,
                    r.y + (r.height - text::kGlyphHeight) / 2, 1,
                    phase == ActionPhase::AwaitingAck && !selected ? 0x8410 : WHITE,
                    r.width - 16);
  }
}

void UiManager::drawAgentHud(Arduino_Canvas& gfx, uint32_t nowMs) const {
  const int8_t index = visibleAgentSession(nowMs);
  if (index < 0) return;
  const AgentSession& session = agentStatus_.sessions[index];
  const uint32_t sinceS = (nowMs - agentStatusReceivedMs_) / 1000U;
  const uint32_t elapsed = session.elapsedS +
      (session.state == AgentState::Done || session.state == AgentState::Stopped ? 0U : sinceS);
  const uint16_t color = session.state == AgentState::Waiting ? 0xFD20 :
      session.state == AgentState::Done ? 0x3EC8 : session.state == AgentState::Stopped ? 0xBDF7 : 0x4D7F;

  const char* activity = session.state == AgentState::Waiting ? "等你确认" :
      session.state == AgentState::Done ? "本轮结束" :
      session.state == AgentState::Stopped ? "已停止" :
      strcmp(session.tool, "Bash") == 0 ? "执行命令" :
      strcmp(session.tool, "apply_patch") == 0 ? "修改文件" :
      session.tool[0] != '\0' ? session.tool : "思考中";
  const char* source = session.source == AgentSource::Codex ? "Codex" : "Claude";
  // Reserve the operation before fitting the project, so a long CJK project
  // cannot hide what the agent is doing. Keep the existing 220 px text budget.
  char normalized[64], operation[64], project[40];
  text::normalize(activity, normalized, sizeof(normalized));
  text::ellipsize(normalized, 96, 1, operation, sizeof(operation));
  text::normalize(session.project, normalized, sizeof(normalized));
  const int16_t projectWidth = 220 - text::width(source, 1) -
      text::width("    ", 1) - text::width(operation, 1);
  text::ellipsize(normalized, projectWidth, 1, project, sizeof(project));
  char visible[128];
  snprintf(visible, sizeof(visible), "%s  %s  %s", source, project, operation);

  char clock[16];
  if (elapsed >= 3600U) {
    snprintf(clock, sizeof(clock), "%lu:%02lu:%02lu", (unsigned long)(elapsed / 3600U),
             (unsigned long)(elapsed / 60U % 60U), (unsigned long)(elapsed % 60U));
  } else {
    snprintf(clock, sizeof(clock), "%lu:%02lu", (unsigned long)(elapsed / 60U),
             (unsigned long)(elapsed % 60U));
  }
  const size_t count = visibleAgentCount(nowMs);
  size_t ordinal = 0;
  for (size_t i = 0; i < agentStatus_.count; ++i) {
    const AgentState state = agentStatus_.sessions[i].state;
    if (state == AgentState::Working || state == AgentState::Waiting ||
        ((state == AgentState::Done || state == AgentState::Stopped) &&
         static_cast<uint64_t>(agentStatus_.sessions[i].ageS) * 1000U +
         nowMs - agentStatusReceivedMs_ < 4000U)) ++ordinal;
    if (static_cast<int8_t>(i) == index) break;
  }
  char badge[8] = {};
  snprintf(badge, sizeof(badge), "%u/%u", unsigned(ordinal), unsigned(count));

  // A "live activity" capsule centred at the bottom, clear of the date.
  constexpr int16_t kHeight = 32, kIcon = 22;
  constexpr int16_t kY = board::kDisplayHeight - kHeight - 16;
  const int16_t labelWidth = text::width(visible, 1);
  const int16_t clockWidth = text::width(clock, 1);
  const int16_t badgeWidth = badge[0] ? text::width(badge, 1) + 10 : 0;
  const int16_t width = 8 + kIcon + labelWidth + 12 + clockWidth + badgeWidth + 10;
  const int16_t kX = (board::kDisplayWidth - width) / 2;
  // A true rounded translucent surface, with no rectangular corners or
  // bright outline competing with the wallpaper.
  uint16_t* fb = gfx.getFramebuffer();
  if (fb && board::kDisplayRotation == 1) {
    constexpr int16_t radius = kHeight / 2;
    for (int16_t x = 0; x < width; ++x) {
      for (int16_t y = 0; y < kHeight; ++y) {
        const int dx = x < radius ? radius - 1 - x : x >= width - radius ? x - (width - radius) : 0;
        const int dy = y < radius ? radius - 1 - y : y - radius;
        const int distance = dx * dx + dy * dy;
        if (distance >= radius * radius) continue;
        const int alpha = distance > (radius - 1) * (radius - 1) ? 96 : 190;
        const int px = kX + x, py = kY + y;
        if (px < 0 || px >= board::kDisplayWidth || py < 0 || py >= board::kDisplayHeight) continue;
        uint16_t& pixel = fb[px * board::kPanelWidth + board::kPanelWidth - 1 - py];
        const int r = (((pixel >> 11) & 31) * (255 - alpha) + 2 * alpha) / 255;
        const int g = (((pixel >> 5) & 63) * (255 - alpha) + 4 * alpha) / 255;
        const int b = ((pixel & 31) * (255 - alpha) + 3 * alpha) / 255;
        pixel = static_cast<uint16_t>((r << 11) | (g << 5) | b);
      }
    }
  }

  // Activity icon: running dots, a breathing dot, or a check mark.
  const int16_t iconX = kX + 8, centerY = kY + kHeight / 2;
  if (session.state == AgentState::Working) {
    const uint32_t step = (nowMs / 180U) % 3U;
    for (uint32_t dot = 0; dot < 3; ++dot) {
      gfx.fillCircle(iconX + 3 + dot * 6, centerY, 2,
                     dot == step ? color : 0x52AA);
    }
  } else if (session.state == AgentState::Waiting) {
    const uint32_t phase = nowMs % 1200U;
    const uint32_t triangle = phase < 600U ? phase : 1200U - phase;
    gfx.fillCircle(iconX + 9, centerY, 3 + triangle * 2 / 600U, color);
  } else if (session.state == AgentState::Stopped) {
    gfx.fillRect(iconX + 5, centerY - 4, 8, 8, color);
  } else {
    for (int16_t t = 0; t < 2; ++t) {
      gfx.drawLine(iconX + 4, centerY + t, iconX + 9, centerY + 5 + t, color);
      gfx.drawLine(iconX + 9, centerY + 5 + t, iconX + 19, centerY - 5 + t, color);
    }
  }
  {
    CjkFontScope font(gfx);
    const int16_t textTop = kY + (kHeight - text::kGlyphHeight) / 2;
    drawCjkLine(gfx, visible, kX + 8 + kIcon, textTop, 1, WHITE);
    drawCjkLine(gfx, clock, kX + 8 + kIcon + labelWidth + 12, textTop, 1, 0xBDF7);
    if (badge[0]) {
      drawCjkLine(gfx, badge, kX + 8 + kIcon + labelWidth + 12 + clockWidth + 10,
                  textTop, 1, color);
    }
  }
}

void UiManager::drawPageDots(Arduino_Canvas& gfx) const {
  const int8_t current = pageIndex(state_);
  constexpr int16_t kSpacing = 14, kY = board::kDisplayHeight - 12;
  const int16_t startX = board::kDisplayWidth / 2 -
                         static_cast<int16_t>((kPageCount - 1) * kSpacing / 2);
  for (size_t i = 0; i < kPageCount; ++i) {
    const int16_t x = startX + static_cast<int16_t>(i * kSpacing);
    if (static_cast<int8_t>(i) == current) gfx.fillCircle(x, kY, 4, WHITE);
    else gfx.drawCircle(x, kY, 4, 0x8410);
  }
}

void UiManager::drawConnectionIndicator(Arduino_Canvas& gfx) const {
  uint16_t color = 0x8410;  // gray
  if (indicator_ == ConnectionIndicator::WIFI_ONLY) {
    color = 0xFDE0;  // yellow
  } else if (indicator_ == ConnectionIndicator::BRIDGE_CONNECTED) {
    color = 0x07E0;  // green
  }
  const int16_t centerX =
      state_ == UiState::HOST_STATUS ? board::kDisplayWidth - 25
                                     : board::kDisplayWidth - 17;
  const int16_t centerY = state_ == UiState::HOST_STATUS ? 52 : 17;
  gfx.fillCircle(centerX, centerY, 3, color);
}

void UiManager::drawOverlay(Arduino_Canvas& gfx) const {
  uint16_t* frameBuffer = gfx.getFramebuffer();
  constexpr size_t kPixelCount =
      static_cast<size_t>(board::kPanelWidth) * board::kPanelHeight;
  for (size_t index = 0; index < kPixelCount; ++index) {
    frameBuffer[index] =
        static_cast<uint16_t>((frameBuffer[index] & 0xF7DEU) >> 1U);
  }
}

void UiManager::drawModal(Arduino_Canvas& gfx) const {
  uint16_t accent = 0x4D7F;
  if (state_ == UiState::SUCCESS) {
    accent = 0x3EC8;
  } else if (state_ == UiState::ERROR) {
    accent = 0xF9E7;
  } else if (state_ == UiState::ACTION_REQUIRED) {
    accent = 0xFD20;
  } else if (state_ == UiState::WIFI_CONNECTING) {
    accent = 0xFDE0;
  }

  fillLandscapeRect(gfx, kCardX, kCardY, kCardWidth, kCardHeight, 0x0863);
  gfx.drawRoundRect(kCardX, kCardY, kCardWidth, kCardHeight, 18, accent);
  const char* heading =
      state_ == UiState::ACTION_REQUIRED
          ? fallbackTitle(state_)
          : (title_[0] == '\0' ? fallbackTitle(state_) : title_);
  drawCenteredText(gfx, heading, board::kDisplayWidth / 2, 54, 2, WHITE,
                   kCardWidth - 36);
  int16_t messageY = 104;
  if (state_ == UiState::ACTION_REQUIRED && title_[0] != '\0') {
    drawCenteredText(gfx, title_, board::kDisplayWidth / 2, 84, 2, 0xD6BA,
                     kCardWidth - 36);
    messageY = 116;
  }
  drawWrappedText(gfx, message_, kCardX + 20, messageY, kCardWidth - 40, 2,
                  0xCE79, state_ == UiState::ACTION_REQUIRED ? 2 : 3);

  if (state_ != UiState::ACTION_REQUIRED) {
    return;
  }

  if (sending_) {
    drawCenteredText(gfx, "Sending...", board::kDisplayWidth / 2, 184, 1,
                     0xBDF7);
  }
  const int16_t firstX = buttonStartX(actionCount_);
  for (size_t index = 0; index < actionCount_; ++index) {
    const int16_t x =
        firstX + static_cast<int16_t>(index) * (kButtonWidth + kButtonGap);
    const bool selected = static_cast<int8_t>(index) == pressedButton_;
    const uint16_t fill = selected ? 0x3D7F : 0x18E8;
    fillLandscapeRect(gfx, x, kButtonY, kButtonWidth, kButtonHeight, fill);
    gfx.drawRoundRect(x, kButtonY, kButtonWidth, kButtonHeight, 10,
                      selected ? WHITE : 0x5CF4);
    drawCenteredText(gfx, actions_[index].label,
                     x + kButtonWidth / 2, kButtonY + 20, 2, WHITE,
                     kButtonWidth - 14);
  }
}

void UiManager::drawSetup(Arduino_Canvas& gfx) const {
  surface::glass(gfx, 60, 20, 360, 280, 0x0863,174,18);
  gfx.drawRoundRect(60, 20, 360, 280, 18, 0x4D7F);
  drawCenteredText(gfx, "SETUP MODE", board::kDisplayWidth / 2, 42, 2,
                   WHITE);
  const bool hasFailure = setupFailure_[0] != '\0';
  if (hasFailure) {
    drawCenteredText(gfx, setupFailure_, board::kDisplayWidth / 2, 68, 1,
                     0xF9E7, 330);
  }
  drawCenteredText(gfx, "Connect to:", board::kDisplayWidth / 2,
                   hasFailure ? 92 : 76, 1, 0xBDF7);
  drawCenteredText(gfx, setupAccessPoint_, board::kDisplayWidth / 2,
                   hasFailure ? 112 : 98, 2, 0x7DFF, 330);
  drawCenteredText(gfx, "Password:", board::kDisplayWidth / 2,
                   hasFailure ? 148 : 138, 1, 0xBDF7);
  drawCenteredText(gfx, setupAccessPointPassword_, board::kDisplayWidth / 2,
                   hasFailure ? 168 : 158, 2, WHITE, 330);
  drawCenteredText(gfx, "Open:", board::kDisplayWidth / 2,
                   hasFailure ? 212 : 204, 1, 0xBDF7);
  drawCenteredText(gfx, "192.168.4.1", board::kDisplayWidth / 2,
                   hasFailure ? 232 : 224, 2, WHITE);
  surface::glass(gfx,132,252,216,34,0x1B71,220,12);
  drawCjkCentered(gfx,"退出并返回主页",board::kDisplayWidth/2,261,1,WHITE,200);
}

void UiManager::drawHostStatus(Arduino_Canvas& gfx, uint32_t nowMs) const {
  const bool bridgeOnline =
      indicator_ == ConnectionIndicator::BRIDGE_CONNECTED;
  const bool fresh = bridgeOnline && hostStatusReceived_ &&
                     nowMs - hostStatusUpdatedMs_ <= kHostStatusStaleMs;

  bool memoryPercentAvailable =
      fresh && hostStatus_.hasMemoryPercent;
  uint8_t memoryPercent = hostStatus_.memoryPercent;
  if (!memoryPercentAvailable && fresh && hostStatus_.hasMemoryUsedMb &&
      hostStatus_.hasMemoryTotalMb && hostStatus_.memoryTotalMb > 0) {
    const uint64_t numerator =
        static_cast<uint64_t>(hostStatus_.memoryUsedMb) * 100U;
    memoryPercent = static_cast<uint8_t>(
        std::min<uint64_t>(100U, numerator / hostStatus_.memoryTotalMb));
    memoryPercentAvailable = true;
  }

  const bool cpuAvailable = fresh && hostStatus_.hasCpuPercent;
  const bool complete = cpuAvailable && memoryPercentAvailable;
  const char* freshness = "WAITING";
  uint16_t freshnessColor = 0xFDE0;
  if (!bridgeOnline) {
    freshness = "OFFLINE";
    freshnessColor = 0xF9E7;
  } else if (hostStatusReceived_ && !fresh) {
    freshness = "STALE";
    freshnessColor = 0xF9E7;
  } else if (fresh && complete) {
    freshness = "LIVE";
    freshnessColor = 0x3EC8;
  } else if (fresh) {
    freshness = "PARTIAL";
  }

  surface::glass(gfx,12,10,board::kDisplayWidth-24,board::kDisplayHeight-20,0x0023,110,18);
  gfx.drawRoundRect(12, 10, board::kDisplayWidth - 24,
                    board::kDisplayHeight - 20, 18, 0x39CF);
  drawCenteredText(gfx, "MAC SYSTEM", 94, 28, 2, WHITE, 160);
  drawCenteredText(gfx, freshness, board::kDisplayWidth / 2, 30, 1,
                   freshnessColor, 110);
  drawCenteredText(gfx, "TAP TO LOCK", 394, 30, 1, 0xBDF7, 130);

  drawMetricCard(gfx, 20, "CPU", cpuAvailable, hostStatus_.cpuPercent,
                 0x4D7F);
  drawMetricCard(gfx, 250, "MEMORY", memoryPercentAvailable, memoryPercent,
                 0xA51F);

  char memoryDetail[40] = "-- / -- GB";
  if (fresh && hostStatus_.hasMemoryUsedMb &&
      hostStatus_.hasMemoryTotalMb) {
    const uint32_t usedTenths = static_cast<uint32_t>(
        (static_cast<uint64_t>(hostStatus_.memoryUsedMb) * 10U + 512U) /
        1024U);
    const uint32_t totalTenths = static_cast<uint32_t>(
        (static_cast<uint64_t>(hostStatus_.memoryTotalMb) * 10U + 512U) /
        1024U);
    snprintf(memoryDetail, sizeof(memoryDetail), "%lu.%lu / %lu.%lu GB",
             static_cast<unsigned long>(usedTenths / 10U),
             static_cast<unsigned long>(usedTenths % 10U),
             static_cast<unsigned long>(totalTenths / 10U),
             static_cast<unsigned long>(totalTenths % 10U));
  }
  drawCenteredText(gfx, memoryDetail, 355, 210, 1,
                   fresh ? 0xBDF7 : 0x8410, 190);

  char uptime[40] = "UPTIME --";
  if (fresh && hostStatus_.hasUptimeSeconds) {
    const uint32_t days = hostStatus_.uptimeSeconds / 86400U;
    const uint32_t hours = (hostStatus_.uptimeSeconds / 3600U) % 24U;
    const uint32_t minutes = (hostStatus_.uptimeSeconds / 60U) % 60U;
    if (days > 0) {
      snprintf(uptime, sizeof(uptime), "UPTIME %lud %02luh %02lum",
               static_cast<unsigned long>(days),
               static_cast<unsigned long>(hours),
               static_cast<unsigned long>(minutes));
    } else {
      snprintf(uptime, sizeof(uptime), "UPTIME %02luh %02lum",
               static_cast<unsigned long>(hours),
               static_cast<unsigned long>(minutes));
    }
  }
  drawCenteredText(gfx, uptime, 132, 264, 1, fresh ? WHITE : 0x8410, 220);

  char updateAge[40] = "NO HOST DATA";
  if (hostStatusReceived_) {
    snprintf(updateAge, sizeof(updateAge), "UPDATED %lus AGO",
             static_cast<unsigned long>((nowMs - hostStatusUpdatedMs_) /
                                        1000U));
  }
  drawCenteredText(gfx, updateAge, 350, 264, 1,
                   fresh ? 0xBDF7 : 0x8410, 210);
  drawCenteredText(gfx, "AUTO-LOCK 15S", board::kDisplayWidth / 2, 289, 1,
                   0x7BEF, 220);
}

#if defined(SENSOR_SERVICE_ENABLED) && SENSOR_SERVICE_ENABLED
void UiManager::updateSensorSnapshot(const sensors::SensorSnapshot& snapshot,
                                     uint32_t nowMs, bool co2High) {
  sensorSnapshot_ = snapshot;
  co2High_ = co2High;
  if (sensors::radarFresh(snapshot.radar, nowMs)) {
    const auto range = sensors::radarRange(snapshot.radar.sample);
    if (range.movingValid) farthestRadarReportCm_ = std::max(
        farthestRadarReportCm_, snapshot.radar.sample.movingDistanceCm);
    if (range.stillValid) farthestRadarReportCm_ = std::max(
        farthestRadarReportCm_, snapshot.radar.sample.stillDistanceCm);
  }
}

int8_t UiManager::sensorButtonAt(const TouchPoint& point) const {
  if (point.x >= 20 && point.x < 92 && point.y >= 18 && point.y < 50) return 0;
  if (point.y >= 54 && point.y < 82) {
    if (point.x >= 20 && point.x < 160) return 3;
    if (point.x >= 170 && point.x < 310) return 4;
    if (point.x >= 320 && point.x < 460) return 5;
  }
  if (environmentView_ == EnvironmentView::History) {
    if (point.y >= 90 && point.y < 118) {
      if (point.x >= 20 && point.x < 92 && historyDaysAgo_ < 6) return 6;
      if (point.x >= 388 && point.x < 460 && historyDaysAgo_ > 0) return 7;
    }
    if (point.y >= 263 && point.y < 297) {
      if (point.x >= 20 && point.x < 160) return 8;
      if (point.x >= 170 && point.x < 310) return 9;
      if (point.x >= 320 && point.x < 460) return 10;
    }
  } else if (environmentView_ == EnvironmentView::Live &&
             point.y >= 263 && point.y < 297) {
    if (point.x >= 20 && point.x < 230) return 1;
    if (point.x >= 250 && point.x < 460) return 2;
  }
  return -1;
}

void UiManager::activateSensorButton(int8_t button, uint32_t nowMs) {
  if (button == 0) {
    setWallpaper();
    Serial.println("PAGE home");
  } else if (button == 1 || button == 2) {
    if (button == 1) presenceWakeEnabled_ = !presenceWakeEnabled_;
    else co2ReminderEnabled_ = !co2ReminderEnabled_;
    sensorOptionsChanged_ = true;
    Serial.printf("SENSOR_OPTIONS presence_wake=%u co2_reminder=%u\n",
                  unsigned(presenceWakeEnabled_), unsigned(co2ReminderEnabled_));
  } else if (button >= 3 && button <= 5) {
    environmentView_ = static_cast<EnvironmentView>(button - 3);
    if (environmentView_ == EnvironmentView::History) historyRequestPending_ = true;
    Serial.printf("ENVIRONMENT_VIEW %u\n", unsigned(button - 3));
    setExpiry(nowMs, 60000);
  } else if (button == 6 || button == 7) {
    historyDaysAgo_ = static_cast<uint8_t>(historyDaysAgo_ + (button == 6 ? 1 : -1));
    historyRequestPending_ = true;
    setExpiry(nowMs, 60000);
  } else if (button >= 8 && button <= 10) {
    historyMetric_ = static_cast<uint8_t>(button - 8);
    setExpiry(nowMs, 60000);
  }
}

void UiManager::drawEnvironment(Arduino_Canvas& gfx, uint32_t nowMs) const {
  surface::glass(gfx, 12, 10, 456, 290, 0x0023, 150, 18);
  gfx.drawRoundRect(12, 10, 456, 290, 18, 0x39CF);
  surface::glass(gfx, 20, 18, 72, 32, 0x0863, 188, 10);
  gfx.drawRoundRect(20, 18, 72, 32, 10, 0x52AA);
  drawCjkCentered(gfx, "主页", 56, 26, 1, WHITE, 60);
  drawCjkCentered(gfx, "本地环境", 240, 25, 1, WHITE, 160);
  drawCjkCentered(gfx, "设备直读", 406, 26, 1, 0xBDF7, 100);
  const char* tabs[] = {"实时", "每日曲线", "雷达距离"};
  for (uint8_t i = 0; i < 3; ++i) {
    const int16_t x = 20 + i * 150;
    const bool selected = unsigned(environmentView_) == i;
    surface::glass(gfx, x, 54, 140, 28, selected ? 0x1B71 : 0x0863,
                   selected ? 214 : 188, 8);
    gfx.drawRoundRect(x, 54, 140, 28, 8, selected ? 0x9FE7 : 0x52AA);
    drawCjkCentered(gfx, tabs[i], x + 70, 60, 1,
                    selected ? WHITE : 0xBDF7, 126);
  }
  switch (environmentView_) {
    case EnvironmentView::Live: drawEnvironmentLive(gfx, nowMs); break;
    case EnvironmentView::History: drawEnvironmentHistory(gfx); break;
    case EnvironmentView::Radar: drawEnvironmentRadar(gfx, nowMs); break;
  }
}

void UiManager::drawEnvironmentLive(Arduino_Canvas& gfx, uint32_t nowMs) const {
  const auto& air = sensorSnapshot_.air;
  const auto& radar = sensorSnapshot_.radar;
  const bool airFresh = sensors::airFresh(air, nowMs);
  const bool radarFresh = sensors::radarFresh(radar, nowMs) &&
      radar.sample.presenceKnown;
  const auto healthText = [](sensors::Health health, bool fresh) {
    switch (health) {
      case sensors::Health::Starting: return "启动中";
      case sensors::Health::Recovering: return "恢复中";
      case sensors::Health::Offline: return "未连接";
      case sensors::Health::Stale: return "数据过期";
      case sensors::Health::Healthy: return fresh ? "实时" : "数据过期";
    }
    return "等待数据";
  };
  const bool high = airFresh && (co2High_ || air.co2Ppm >= 1500U);
  const uint16_t airColor = !airFresh ? 0xBDF7 : high ? 0xFDE0 : 0x9FE7;
  const uint16_t presenceColor = radarFresh ? 0x9FE7 : 0xFDE0;

  surface::glass(gfx, 20, 90, 260, 112, 0x0863, 144, 14);
  gfx.drawRoundRect(20, 90, 260, 112, 14, airColor);
  drawCenteredText(gfx, "CO2", 150, 99, 2, 0xBDF7, 220);
  char value[32] = "--";
  if (airFresh) snprintf(value, sizeof(value), "%u", unsigned(air.co2Ppm));
  drawCenteredText(gfx, value, 150, 124, 4,
                   airFresh ? airColor : 0x8410, 224);
  drawCenteredText(gfx, "ppm", 150, 160, 1, 0xBDF7, 220);
  drawCjkCentered(gfx, high ? "偏高，请通风" : healthText(air.health, airFresh),
                  150, 180, 1, airColor, 240);

  surface::glass(gfx, 290, 90, 170, 52, 0x0863, 144, 12);
  drawCjkCentered(gfx, "温度", 375, 97, 1, 0xBDF7, 150);
  if (airFresh) snprintf(value, sizeof(value), "%.1f C", air.temperatureC);
  else strlcpy(value, "-- C", sizeof(value));
  drawCenteredText(gfx, value, 375, 119, 2, airFresh ? WHITE : 0x8410, 150);

  surface::glass(gfx, 290, 150, 170, 52, 0x0863, 144, 12);
  drawCjkCentered(gfx, "相对湿度", 375, 157, 1, 0xBDF7, 150);
  if (airFresh) snprintf(value, sizeof(value), "%.1f%%", air.humidityRh);
  else strlcpy(value, "-- %", sizeof(value));
  drawCenteredText(gfx, value, 375, 179, 2, airFresh ? WHITE : 0x8410, 150);

  const char* presence = !radarFresh ? "有人状态未知" :
      radar.sample.presence ? "有人" : "无人";
  drawCjkCentered(gfx, presence, 150, 213, 1, presenceColor, 240);
  char status[64];
  const auto range = sensors::radarRange(radar.sample);
  if (radarFresh && range.hasDistance) {
    snprintf(status, sizeof(status), "最近 %.2f m", range.nearestDistanceCm / 100.0);
  } else if (radarFresh && radar.sample.presence) {
    strlcpy(status, "距离未知", sizeof(status));
  } else snprintf(status, sizeof(status), "雷达：%s", healthText(radar.health, radarFresh));
  drawCjkCentered(gfx, status, 375, 213, 1, presenceColor, 170);

  char airAge[32] = "空气尚无读数";
  char radarAge[32] = "雷达尚无数据";
  if (air.hasSample) snprintf(airAge, sizeof(airAge), "空气 %lus前",
      static_cast<unsigned long>(sensors::sampleAgeMs(nowMs, air.sampledAtMs) / 1000U));
  if (radar.hasSample) snprintf(radarAge, sizeof(radarAge), "雷达 %lus前",
      static_cast<unsigned long>(sensors::sampleAgeMs(nowMs, radar.sample.receivedAtMs) / 1000U));
  drawCjkCentered(gfx, airAge, 150, 238, 1, 0xBDF7, 240);
  drawCjkCentered(gfx, radarAge, 375, 238, 1, 0xBDF7, 170);

  const bool enabled[] = {presenceWakeEnabled_, co2ReminderEnabled_};
  const char* labels[] = {"感应亮屏", "CO2提醒"};
  for (size_t i = 0; i < 2; ++i) {
    const int16_t x = i == 0 ? 20 : 250;
    const uint16_t accent = enabled[i] ? 0x9FE7 : 0x8410;
    surface::glass(gfx, x, 263, 210, 34, enabled[i] ? 0x1B71 : 0x0863,
                   enabled[i] ? 214 : 188, 10);
    gfx.drawRoundRect(x, 263, 210, 34, 10, accent);
    drawCjkCentered(gfx, labels[i], x + 78, 272, 1, WHITE, 136);
    drawCjkCentered(gfx, enabled[i] ? "开" : "关", x + 176, 272, 1,
                    accent, 40);
  }
}

void UiManager::drawEnvironmentRadar(Arduino_Canvas& gfx, uint32_t nowMs) const {
  const auto& radar = sensorSnapshot_.radar;
  const bool fresh = sensors::radarFresh(radar, nowMs) && radar.sample.presenceKnown;
  const auto range = sensors::radarRange(radar.sample);
  const char* unknown = radar.health == sensors::Health::Starting ? "雷达启动中 / 有人状态未知" :
      radar.health == sensors::Health::Recovering ? "雷达恢复中 / 有人状态未知" :
      radar.health == sensors::Health::Offline ? "雷达未连接 / 有人状态未知" :
      "雷达数据过期 / 有人状态未知";
  const char* presence = !fresh ? unknown :
      radar.sample.presence ? "房间有人" : "房间无人";
  drawCjkCentered(gfx, presence, 240, 91, 1, fresh ? 0x9FE7 : 0xFDE0, 440);
  const bool valid[] = {fresh && range.movingValid, fresh && range.stillValid};
  const uint16_t distances[] = {radar.sample.movingDistanceCm, radar.sample.stillDistanceCm};
  const uint8_t energies[] = {radar.sample.movingEnergy, radar.sample.stillEnergy};
  const char* labels[] = {"运动目标", "静止目标"};
  for (uint8_t index = 0; index < 2; ++index) {
    const int16_t x = index == 0 ? 20 : 250;
    const uint16_t accent = valid[index] ? 0x9FE7 : 0x8410;
    surface::glass(gfx, x, 116, 210, 102, 0x0863, 144, 12);
    gfx.drawRoundRect(x, 116, 210, 102, 12, accent);
    drawCjkCentered(gfx, labels[index], x + 105, 124, 1, 0xBDF7, 190);
    char distance[32] = "-- m";
    if (valid[index]) snprintf(distance, sizeof(distance), "%.2f m", distances[index] / 100.0);
    drawCenteredText(gfx, distance, x + 105, 150, 3, valid[index] ? WHITE : 0x8410, 190);
    char detail[48] = "-- cm / ENERGY --";
    if (valid[index]) snprintf(detail, sizeof(detail), "%u cm / ENERGY %u",
                              unsigned(distances[index]), unsigned(energies[index]));
    drawCenteredText(gfx, detail, x + 105, 190, 1, accent, 194);
  }
  const char* wake = !fresh ? "无有效雷达，回退触摸唤醒" :
      !presenceWakeEnabled_ ? "感应亮屏已关闭" :
      range.nearPresence ? "1 m 内有人：可感应亮屏" :
      range.hasDistance ? "1 m 外：不触发感应亮屏" :
      radar.sample.presence ? "距离未知：不触发感应亮屏" : "无人：等待空闲调暗";
  drawCjkCentered(gfx, wake, 240, 229, 1,
                  fresh && range.nearPresence ? 0x9FE7 : 0xBDF7, 438);
  char status[64] = "本次最远报告 --";
  if (farthestRadarReportCm_ > 0) snprintf(status, sizeof(status), "本次最远报告 %.2f m (%u cm)",
      farthestRadarReportCm_ / 100.0, unsigned(farthestRadarReportCm_));
  drawCjkCentered(gfx, status, 240, 253, 1, 0xBDF7, 438);
  drawCjkCentered(gfx, "两类目标不代表两个人，无法识别人数", 240, 277, 1, 0xBDF7, 438);
}

void UiManager::drawEnvironmentHistory(Arduino_Canvas& gfx) const {
  const auto* view = airHistory_;
  const bool selected = view && view->daysAgo == historyDaysAgo_;
  const char* state = !view ? "历史服务启动中" :
      !view->clockValid ? "未校时，等待本地日期" :
      !selected ? "正在载入所选日期" :
      view->state == sensors::HistoryState::WaitingForClock ? "未校时，等待本地日期" :
      view->state == sensors::HistoryState::Starting ? "历史服务启动中" :
      view->state == sensors::HistoryState::Loading ? "正在载入所选日期" :
      view->state == sensors::HistoryState::SdUnavailable ? "SD 卡不可用，无法保存历史" :
      view->state == sensors::HistoryState::StorageError ? "历史存储错误，实时数据仍可查看" :
      view->state == sensors::HistoryState::MemoryUnavailable ? "历史内存不足" : "";
  const bool ready = selected && view->clockValid &&
      view->state == sensors::HistoryState::Ready && view->buckets;
  const char* navigation[] = {"前一天", "后一天"};
  for (uint8_t index = 0; index < 2; ++index) {
    const bool enabled = index == 0 ? historyDaysAgo_ < 6 : historyDaysAgo_ > 0;
    const int16_t x = index == 0 ? 20 : 388;
    surface::glass(gfx, x, 90, 72, 28, 0x0863, 188, 8);
    gfx.drawRoundRect(x, 90, 72, 28, 8, enabled ? 0x52AA : 0x2945);
    drawCjkCentered(gfx, navigation[index], x + 36, 96, 1,
                    enabled ? WHITE : 0x8410, 64);
  }
  char date[40] = "日期等待校时";
  if (selected && view->clockValid) {
    snprintf(date, sizeof(date), "%04u-%02u-%02u%s", unsigned(view->year),
        unsigned(view->month), unsigned(view->day), historyDaysAgo_ == 0 ? " 今日" : "");
  } else if (view && view->clockValid) {
    snprintf(date, sizeof(date), "%u 天前 / 载入中", unsigned(historyDaysAgo_));
  }
  drawCjkCentered(gfx, date, 240, 96, 1, WHITE, 282);
  const uint16_t accent = historyMetric_ == 0 ? 0x9FE7 : historyMetric_ == 1 ? 0xFD20 : 0x4D7F;
  const char* metrics[] = {"CO2 ppm", "温度 C", "湿度 %RH"};
  const char* units[] = {"ppm", "C", "%RH"};
  if (ready) {
    const auto range = environment_chart::scale(*view, historyMetric_);
    char summary[80];
    snprintf(summary, sizeof(summary), "1分钟记录 / 5分钟均值 / %lu 分钟", static_cast<unsigned long>(view->validMinutes));
    drawCjkCentered(gfx, summary, 240, 122, 1, 0xBDF7, 438);
    if (range.points == 0) {
      drawCjkCentered(gfx, "当天尚无有效记录", 255, 179, 1, 0xBDF7, 380);
    }
    for (uint8_t index = 0; index < 3; ++index) {
      const int16_t y = static_cast<int16_t>(142 + index * 42);
      gfx.drawLine(66, y, 446, y, 0x2945);
      const int32_t tick = range.high - (range.high - range.low) * index / 2;
      char label[24];
      if (historyMetric_ == 0) snprintf(label, sizeof(label), "%ld", static_cast<long>(tick));
      else snprintf(label, sizeof(label), "%.0f", tick / 100.0);
      drawCenteredText(gfx, label, 37, y - 4, 1, 0xBDF7, 52);
    }
    for (uint16_t index = 0; index < std::min<uint16_t>(view->bucketCount, sensors::kAirHistoryBuckets); ++index) {
      const auto& bucket = view->buckets[index];
      if (!bucket.valid) continue;
      const int16_t x = environment_chart::x(index);
      const int16_t y = environment_chart::y(environment_chart::value(bucket, historyMetric_), range);
      if (environment_chart::connects(*view, index)) {
        gfx.drawLine(environment_chart::x(index - 1),
            environment_chart::y(environment_chart::value(view->buckets[index - 1], historyMetric_), range),
            x, y, accent);
      }
      gfx.fillCircle(x, y, 1, accent);  // A first real minute is already visible.
    }
    drawCenteredText(gfx, units[historyMetric_], 37, 240, 1, accent, 52);
  } else {
    drawCjkCentered(gfx, state[0] ? state : "历史数据不可用", 240, 174, 1, 0xFDE0, 432);
    drawCjkCentered(gfx, "本地保存最近 7 天，缺失时段留空", 240, 202, 1, 0xBDF7, 432);
  }
  const char* hours[] = {"00", "06", "12", "18", "24"};
  for (uint8_t index = 0; index < 5; ++index) {
    drawCenteredText(gfx, hours[index], 66 + index * 95, 240, 1, 0xBDF7, 30);
  }
  for (uint8_t index = 0; index < 3; ++index) {
    const int16_t x = 20 + index * 150;
    const bool active = historyMetric_ == index;
    surface::glass(gfx, x, 263, 140, 34, active ? 0x1B71 : 0x0863,
                   active ? 214 : 188, 10);
    gfx.drawRoundRect(x, 263, 140, 34, 10, active ? accent : 0x52AA);
    drawCjkCentered(gfx, metrics[index], x + 70, 272, 1, active ? WHITE : 0xBDF7, 126);
  }
}
#endif
