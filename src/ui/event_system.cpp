#include "event_system.h"

#include <cstring>

namespace {
bool sameId(const CompanionEvent& left, const CompanionEvent& right) {
  return (left.type == EventType::ActionRequired) ==
             (right.type == EventType::ActionRequired) &&
         strcmp(left.id, right.id) == 0;
}
}

const char* eventTypeName(EventType type) {
  switch (type) {
    case EventType::Info: return "INFO";
    case EventType::Success: return "SUCCESS";
    case EventType::Warning: return "WARNING";
    case EventType::Error: return "ERROR";
    case EventType::ActionRequired: return "ACTION_REQUIRED";
  }
  return "UNKNOWN";
}

bool EventSystem::known(const CompanionEvent& event) const {
  if (active_ && sameId(current_, event)) return true;
  for (size_t i = 0; i < queueCount_; ++i) {
    if (sameId(queue_[i], event)) return true;
  }
  for (const RecentId& recent : recent_) {
    if (recent.request == (event.type == EventType::ActionRequired) &&
        strcmp(recent.id, event.id) == 0) return true;
  }
  return false;
}

void EventSystem::remember(const CompanionEvent& event) {
  RecentId& recent = recent_[recentCursor_];
  strlcpy(recent.id, event.id, sizeof(recent.id));
  recent.request = event.type == EventType::ActionRequired;
  recentCursor_ = (recentCursor_ + 1) % kRecentCapacity;
}

bool EventSystem::enqueue(const CompanionEvent& event) {
  if (queueCount_ == kQueueCapacity) {
    // Evict the oldest event at the lowest strictly lower priority. Equal
    // priority requests/errors are retained; the incoming packet is rejected.
    size_t victim = kQueueCapacity;
    EventType lowest = event.type;
    for (size_t i = 0; i < queueCount_; ++i) {
      if (queue_[i].type < lowest) { lowest = queue_[i].type; victim = i; }
    }
    ++statistics_.overflowDrops;
    if (victim == kQueueCapacity) {
      Serial.printf("EVENT_QUEUE_FULL rejected=%s id=%s depth=%u\n",
                    eventTypeName(event.type), event.id, unsigned(queueCount_));
      return false;
    }
    Serial.printf("EVENT_QUEUE_EVICT level=%s id=%s\n",
                  eventTypeName(queue_[victim].type), queue_[victim].id);
    record(queue_[victim], "overflow", "");
    for (size_t i = victim + 1; i < queueCount_; ++i) queue_[i - 1] = queue_[i];
    --queueCount_;
  }
  queue_[queueCount_++] = event;
  return true;
}

void EventSystem::activate(const CompanionEvent& event, uint32_t nowMs) {
  current_ = event;
  active_ = true;
  phase_ = ActionPhase::Choice;
  selectedAction_[0] = '\0';
  shownAtMs_ = nowMs;
  ++revision_;
  Serial.printf("EVENT_SHOWN type=%s id=%s actions=%u queue=%u\n",
                eventTypeName(event.type), event.id, unsigned(event.actionCount),
                unsigned(queueCount_));
}

bool EventSystem::receive(const CompanionEvent& input, uint32_t nowMs) {
  if (input.id[0] == '\0' ||
      (input.type == EventType::ActionRequired &&
       (input.actionCount == 0 || input.actionCount > kMaxEventActions))) return false;
  if (known(input)) {
    ++statistics_.duplicateEvents;
    Serial.printf("EVENT_DUPLICATE id=%s\n", input.id);
    return false;
  }
  CompanionEvent event = input;
  event.receivedAtMs = nowMs;
  if (!active_) {
    activate(event, nowMs);
  } else if (current_.type != EventType::ActionRequired && event.type > current_.type) {
    // The higher priority event is never lost because the pending queue is full.
    // Failure to retain the preempted notification is explicit in the log.
    enqueue(current_);
    activate(event, nowMs);
  } else if (!enqueue(event)) {
    return false;
  }
  remember(event);
  ++statistics_.received;
  return true;
}

bool EventSystem::sendSelected(bool connected, uint32_t nowMs, UiActionEvent& action) {
  action = UiActionEvent{};
  phase_ = connected ? ActionPhase::AwaitingAck : ActionPhase::Unconfirmed;
  sentAtMs_ = nowMs;
  ++revision_;
  if (!connected) return false;
  strlcpy(action.requestId, current_.id, sizeof(action.requestId));
  strlcpy(action.actionId, selectedAction_, sizeof(action.actionId));
  action.valid = true;
  ++statistics_.actionAttempts;
  return true;
}

bool EventSystem::choose(size_t index, bool connected, uint32_t nowMs, UiActionEvent& action) {
  action = UiActionEvent{};
  if (!active_ || current_.type != EventType::ActionRequired ||
      (phase_ != ActionPhase::Choice && phase_ != ActionPhase::Rejected) ||
      index >= current_.actionCount) return false;
  strlcpy(selectedAction_, current_.actions[index].id, sizeof(selectedAction_));
  Serial.printf("ACTION_SELECTED request_id=%s action_id=%s connected=%u\n",
                current_.id, selectedAction_, unsigned(connected));
  return sendSelected(connected, nowMs, action);
}

bool EventSystem::retry(bool connected, uint32_t nowMs, UiActionEvent& action) {
  action = UiActionEvent{};
  if (!active_ || phase_ != ActionPhase::Unconfirmed || selectedAction_[0] == '\0') return false;
  ++statistics_.retries;
  return sendSelected(connected, nowMs, action);
}

void EventSystem::sendFailed() {
  if (active_ && phase_ == ActionPhase::AwaitingAck) {
    phase_ = ActionPhase::Unconfirmed;
    ++revision_;
  }
}

void EventSystem::disconnected() { sendFailed(); }

void EventSystem::record(const CompanionEvent& event, const char* result, const char* actionId) {
  EventHistoryEntry& entry = history_[historyCursor_];
  entry = EventHistoryEntry{};
  entry.type = event.type;
  entry.receivedAtMs = event.receivedAtMs;
  strlcpy(entry.id, event.id, sizeof(entry.id));
  strlcpy(entry.actionId, actionId, sizeof(entry.actionId));
  strlcpy(entry.result, result, sizeof(entry.result));
  historyCursor_ = (historyCursor_ + 1) % kHistoryCapacity;
  if (historyCount_ < kHistoryCapacity) ++historyCount_;
}

const EventHistoryEntry& EventSystem::history(size_t index) const {
  return history_[(historyCursor_ + kHistoryCapacity - 1 -
                   (index % kHistoryCapacity)) % kHistoryCapacity];
}

void EventSystem::finish(const char* result, uint32_t nowMs) {
  Serial.printf("EVENT_CLOSED id=%s result=%s\n", current_.id, result);
  record(current_, result, selectedAction_);
  active_ = false;
  selectedAction_[0] = '\0';
  ++revision_;
  if (queueCount_ == 0) return;
  size_t best = 0;
  for (size_t i = 1; i < queueCount_; ++i) {
    if (queue_[i].type > queue_[best].type) best = i;
  }
  const CompanionEvent next = queue_[best];
  for (size_t i = best + 1; i < queueCount_; ++i) queue_[i - 1] = queue_[i];
  --queueCount_;
  activate(next, nowMs);
}

bool EventSystem::acknowledge(const char* requestId, const char* actionId,
                               bool accepted, uint32_t nowMs) {
  if (!active_ || current_.type != EventType::ActionRequired ||
      (phase_ != ActionPhase::AwaitingAck && phase_ != ActionPhase::Unconfirmed) ||
      strcmp(current_.id, requestId) != 0 || strcmp(selectedAction_, actionId) != 0) {
    ++statistics_.ignoredAcks;
    Serial.println("ACTION_ACK_IGNORED mismatched_or_completed");
    return false;
  }
  Serial.printf("ACTION_ACK request_id=%s action_id=%s accepted=%u\n",
                requestId, actionId, unsigned(accepted));
  if (accepted) {
    ++statistics_.accepted;
    finish("accepted", nowMs);
  } else {
    ++statistics_.rejected;
    record(current_, "rejected", selectedAction_);
    phase_ = ActionPhase::Rejected;
    selectedAction_[0] = '\0';
    ++revision_;
  }
  return true;
}

bool EventSystem::dismiss(const char* id, uint32_t nowMs) {
  if (id == nullptr || id[0] == '\0') return false;
  if (active_ && strcmp(id, current_.id) == 0) {
    finish("dismissed", nowMs);
    return true;
  }
  for (size_t i = 0; i < queueCount_; ++i) {
    if (strcmp(queue_[i].id, id) != 0) continue;
    record(queue_[i], "dismissed", "");
    for (size_t j = i + 1; j < queueCount_; ++j) queue_[j - 1] = queue_[j];
    --queueCount_;
    return true;
  }
  return false;
}

void EventSystem::tick(uint32_t nowMs) {
  if (!active_) return;
  if (current_.type != EventType::ActionRequired && current_.autoDismissMs > 0 &&
      nowMs - shownAtMs_ >= current_.autoDismissMs) {
    finish("auto_dismissed", nowMs);
  } else if (current_.type == EventType::ActionRequired &&
             phase_ == ActionPhase::AwaitingAck && nowMs - sentAtMs_ >= kAckTimeoutMs) {
    ++statistics_.ackTimeouts;
    phase_ = ActionPhase::Unconfirmed;
    ++revision_;
    Serial.printf("ACTION_UNCONFIRMED request_id=%s reason=ack_timeout\n", current_.id);
  }
}
