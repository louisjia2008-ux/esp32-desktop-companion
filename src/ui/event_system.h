#pragma once

#include <Arduino.h>
#include <array>

enum class EventType : uint8_t { Info, Success, Warning, Error, ActionRequired };

// Four choices fit a 2x2 grid; older Bridges never send more than three.
constexpr size_t kMaxEventActions = 4;
enum class ActionPhase : uint8_t { Choice, AwaitingAck, Unconfirmed, Rejected };

struct UiActionOption {
  char id[33] = {};
  char label[49] = {};
  // Non-zero: the button must be held this long before it dispatches.
  uint16_t holdMs = 0;
};

struct UiActionEvent {
  bool valid = false;
  char requestId[49] = {};
  char actionId[33] = {};
};

struct CompanionEvent {
  EventType type = EventType::Info;
  char id[49] = {};
  char source[33] = {};
  char title[49] = {};
  char message[129] = {};
  std::array<UiActionOption, kMaxEventActions> actions{};
  size_t actionCount = 0;
  uint32_t autoDismissMs = 0;
  uint32_t receivedAtMs = 0;
};

struct EventHistoryEntry {
  EventType type = EventType::Info;
  char id[49] = {};
  char actionId[33] = {};
  char result[17] = {};
  uint32_t receivedAtMs = 0;
};

struct EventStatistics {
  uint32_t received = 0;
  uint32_t duplicateEvents = 0;
  uint32_t overflowDrops = 0;
  uint32_t actionAttempts = 0;
  uint32_t retries = 0;
  uint32_t accepted = 0;
  uint32_t rejected = 0;
  uint32_t ackTimeouts = 0;
  uint32_t ignoredAcks = 0;
};

// All event, history and identity storage is bounded and lives in RAM.
// The network callback never changes this state: the main loop owns it.
class EventSystem {
 public:
  static constexpr size_t kQueueCapacity = 8;
  static constexpr size_t kRecentCapacity = 64;
  static constexpr size_t kHistoryCapacity = 10;
  static constexpr uint32_t kAckTimeoutMs = 4000;

  bool receive(const CompanionEvent& event, uint32_t nowMs);
  bool choose(size_t index, bool connected, uint32_t nowMs, UiActionEvent& action);
  bool retry(bool connected, uint32_t nowMs, UiActionEvent& action);
  void sendFailed();
  bool acknowledge(const char* requestId, const char* actionId, bool accepted,
                   uint32_t nowMs);
  void disconnected();
  bool dismiss(const char* id, uint32_t nowMs);
  void tick(uint32_t nowMs);

  bool active() const { return active_; }
  const CompanionEvent& current() const { return current_; }
  ActionPhase phase() const { return phase_; }
  const char* selectedAction() const { return selectedAction_; }
  uint32_t revision() const { return revision_; }
  size_t queueDepth() const { return queueCount_; }
  size_t historyCount() const { return historyCount_; }
  const EventHistoryEntry& history(size_t index) const;
  const EventStatistics& statistics() const { return statistics_; }

 private:
  struct RecentId { char id[49] = {}; bool request = false; };
  bool known(const CompanionEvent& event) const;
  bool enqueue(const CompanionEvent& event);
  void activate(const CompanionEvent& event, uint32_t nowMs);
  void finish(const char* result, uint32_t nowMs);
  void remember(const CompanionEvent& event);
  void record(const CompanionEvent& event, const char* result, const char* actionId);
  bool sendSelected(bool connected, uint32_t nowMs, UiActionEvent& action);

  CompanionEvent current_{};
  std::array<CompanionEvent, kQueueCapacity> queue_{};
  std::array<RecentId, kRecentCapacity> recent_{};
  std::array<EventHistoryEntry, kHistoryCapacity> history_{};
  size_t queueCount_ = 0;
  size_t recentCursor_ = 0;
  size_t historyCursor_ = 0;
  size_t historyCount_ = 0;
  bool active_ = false;
  ActionPhase phase_ = ActionPhase::Choice;
  char selectedAction_[33] = {};
  uint32_t shownAtMs_ = 0;
  uint32_t sentAtMs_ = 0;
  uint32_t revision_ = 0;
  EventStatistics statistics_{};
};

const char* eventTypeName(EventType type);
