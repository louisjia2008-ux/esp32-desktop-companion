#include "ui/ui_manager.h"
#include "ui/text_layout.h"
#include "ui/backlight_policy.h"
#include "ui/glass_clock.h"
#include "protocol/protocol.h"
#include "board_config.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <functional>

namespace uifont { const uint8_t* cjkFont() { return nullptr; } }
void Wallpaper::begin(uint32_t) {}
void Wallpaper::render(Arduino_Canvas&, uint32_t) {}
bool Wallpaper::selectVideo(const char*) { return true; }
bool Wallpaper::decodeVideoFrame(const char*, const uint8_t*, size_t, uint32_t) { return true; }
bool Wallpaper::videoFresh(uint32_t) const { return false; }

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #x); } while (false)

CompanionEvent event(const char* id, EventType type = EventType::Info, size_t count = 3) {
  CompanionEvent e;
  strlcpy(e.id, id, sizeof(e.id));
  strlcpy(e.title, "A bounded title", sizeof(e.title));
  strlcpy(e.message, "A message that must stay above the buttons.", sizeof(e.message));
  e.type = type;
  if (type == EventType::ActionRequired) {
    e.actionCount = count;
    for (size_t i = 0; i < count && i < kMaxEventActions; ++i) {
      snprintf(e.actions[i].id, sizeof(e.actions[i].id), "option-%u", unsigned(i));
      snprintf(e.actions[i].label, sizeof(e.actions[i].label), "Option %u", unsigned(i));
    }
  }
  return e;
}

void ackAndIdempotentTouch() {
  UiManager ui;
  ui.begin(1);
  ui.setConnectionIndicator(ConnectionIndicator::BRIDGE_CONNECTED);
  UiActionEvent a;
  auto e = event("req", EventType::ActionRequired);
  CHECK(ui.receiveEvent(e, 100));
  CHECK(!ui.receiveEvent(e, 101));
  fake_arduino::setMillis(102);
  ui.handleTouch(false, {}, 102, a);
  auto r = ui.eventButtonRect(1);
  const TouchPoint p{uint16_t(r.x + r.width / 2), uint16_t(r.y + r.height / 2)};
  fake_arduino::setMillis(110);
  ui.handleTouch(true, p, 110, a);
  fake_arduino::setMillis(170);
  CHECK(ui.handleTouch(true, p, 170, a) && a.valid);
  CHECK(std::string(a.actionId) == "option-1");
  for (uint32_t t = 180; t < 600; t += 10) {
    fake_arduino::setMillis(t);
    CHECK(!ui.handleTouch(true, p, t, a));
  }
  CHECK(ui.events().statistics().actionAttempts == 1);
  CHECK(!ui.acknowledgeAction("wrong", "option-1", true, 601));
  CHECK(!ui.acknowledgeAction("req", "option-0", true, 602));
  CHECK(ui.overlayActive());
  CHECK(ui.acknowledgeAction("req", "option-1", true, 603));
  CHECK(!ui.overlayActive());
  CHECK(!ui.receiveEvent(e, 604));
  CHECK(!ui.acknowledgeAction("req", "option-1", true, 605));
}

void reconnectAndRetry() {
  EventSystem events;
  CHECK(events.receive(event("offline", EventType::ActionRequired), 0));
  UiActionEvent a;
  CHECK(!events.choose(1, false, 100, a));
  CHECK(events.phase() == ActionPhase::Unconfirmed);
  CHECK(std::string(events.selectedAction()) == "option-1");
  CHECK(events.retry(true, 200, a));
  CHECK(std::string(a.requestId) == "offline" && std::string(a.actionId) == "option-1");
  events.disconnected();
  CHECK(events.phase() == ActionPhase::Unconfirmed && events.active());
  CHECK(events.retry(true, 300, a));
  events.tick(4299);
  CHECK(events.phase() == ActionPhase::AwaitingAck);
  events.tick(4300);
  CHECK(events.phase() == ActionPhase::Unconfirmed);
  CHECK(!events.choose(0, true, 4400, a));
  // A matching late ACK still confirms the same immutable selection.
  CHECK(events.acknowledge("offline", "option-1", true, 4500));
  CHECK(!events.active());
}

void rejectedChoiceCanChange() {
  EventSystem events;
  events.receive(event("rejected", EventType::ActionRequired), 0);
  UiActionEvent a;
  CHECK(events.choose(1, true, 10, a));
  CHECK(events.acknowledge("rejected", "option-1", false, 20));
  CHECK(events.phase() == ActionPhase::Rejected);
  CHECK(events.choose(2, true, 30, a));
  CHECK(std::string(a.actionId) == "option-2");
  CHECK(events.acknowledge("rejected", "option-2", true, 40));
  CHECK(events.historyCount() == 2);
}

void queueAndOverflow() {
  EventSystem events;
  CHECK(events.receive(event("active", EventType::ActionRequired), 0));
  for (int i = 0; i < 8; ++i) {
    char id[20]; snprintf(id, sizeof(id), "info-%d", i);
    CHECK(events.receive(event(id), i + 1));
  }
  CHECK(events.queueDepth() == 8);
  CHECK(events.receive(event("error", EventType::Error), 10));
  CHECK(events.receive(event("action", EventType::ActionRequired), 11));
  CHECK(events.queueDepth() == 8);
  CHECK(!events.receive(event("new-info"), 12));
  CHECK(std::string(events.current().id) == "active");
  CHECK(events.dismiss("active", 20));
  CHECK(std::string(events.current().id) == "action");
  CHECK(events.dismiss("action", 30));
  CHECK(std::string(events.current().id) == "error");
  CHECK(events.statistics().overflowDrops == 3);
}

void preemptionHistoryAndExpiry() {
  EventSystem events;
  auto info = event("info"); info.autoDismissMs = 2000;
  events.receive(info, UINT32_MAX - 1000);
  events.tick(998);
  CHECK(events.active());
  events.tick(999);
  CHECK(!events.active());
  events.receive(event("warning", EventType::Warning), 2000);
  events.tick(200000);
  CHECK(events.active());
  events.receive(event("success", EventType::Success), 200001);
  events.receive(event("request", EventType::ActionRequired), 200002);
  CHECK(std::string(events.current().id) == "request");
  events.dismiss("request", 200003);
  CHECK(std::string(events.current().id) == "warning");
  events.dismiss("warning", 200004);
  CHECK(std::string(events.current().id) == "success");
  events.dismiss("success", 200005);
  for (int i = 0; i < 1000; ++i) {
    char id[20]; snprintf(id, sizeof(id), "stress-%d", i);
    CHECK(events.receive(event(id), 201000 + i));
    CHECK(events.dismiss(id, 201000 + i));
  }
  CHECK(events.historyCount() == 10 && events.queueDepth() == 0);
  CHECK(std::string(events.history(0).id) == "stress-999");
  CHECK(std::string(events.history(9).id) == "stress-990");
}

void restoreHostPageAndRespectTimeout() {
  UiManager ui; ui.begin(1);
  UiActionEvent a;
  fake_arduino::setMillis(0); ui.handleTouch(false, {}, 0, a);
  for (uint32_t t = 100; t <= 200; t += 10) {  // swipe left to the Mac page
    fake_arduino::setMillis(t);
    ui.handleTouch(true, {uint16_t(400 - (t - 100) * 2), 160}, t, a);
  }
  fake_arduino::setMillis(290); ui.handleTouch(false, {}, 290, a);
  CHECK(ui.state() == UiState::HOST_STATUS);
  CHECK(ui.receiveEvent(event("notice"), 300));
  CHECK(ui.state() == UiState::NOTIFICATION && ui.baseState() == UiState::HOST_STATUS);
  fake_arduino::setMillis(500); ui.dismiss("notice");
  CHECK(ui.state() == UiState::HOST_STATUS);
  CHECK(ui.receiveEvent(event("second"), 600));
  fake_arduino::setMillis(15300); ui.tick(15300);
  CHECK(ui.overlayActive() && ui.baseState() == UiState::WALLPAPER);
  ui.dismiss("second");
  CHECK(ui.state() == UiState::WALLPAPER);
}

void layoutsAndNotificationDismiss() {
  for (size_t count = 1; count <= 3; ++count) {
    UiManager ui; ui.begin(1);
    CHECK(ui.receiveEvent(event("layout", EventType::ActionRequired, count), 0));
    UiManager::ButtonRect previous{0,0,0,0};
    for (size_t i = 0; i < count; ++i) {
      const auto r = ui.eventButtonRect(i);
      CHECK(r.width >= 100 && r.height >= 48);
      CHECK(r.x + r.width <= board::kDisplayWidth - 20 &&
            r.y + r.height <= board::kDisplayHeight - 24);
      if (i > 0) CHECK(r.x >= previous.x + previous.width || r.y >= previous.y + previous.height);
      previous = r;
    }
  }
  UiManager ui; ui.begin(1);
  CHECK(ui.receiveEvent(event("dismiss"), 100));
  auto r = ui.eventButtonRect(0);
  TouchPoint p{uint16_t(r.x + 10), uint16_t(r.y + 10)};
  UiActionEvent a;
  fake_arduino::setMillis(100); ui.handleTouch(false, {}, 100, a);
  fake_arduino::setMillis(110); ui.handleTouch(true, p, 110, a);
  fake_arduino::setMillis(210); ui.handleTouch(false, {}, 210, a);
  CHECK(!ui.overlayActive() && !a.valid);
}

bool parse(const std::string& json) {
  protocol::IncomingMessage m;
  const char* reason;
  return protocol::parseIncoming(reinterpret_cast<const uint8_t*>(json.data()), json.size(), m, reason);
}

void strictProtocolAndMalformedInput() {
  CHECK(parse(R"({"v":1,"type":"notification","event_id":"evt","level":"warning","source":"demo","title":"Title","message":"Text"})"));
  CHECK(parse(R"({"v":1,"type":"action_required","request_id":"req","source":"demo","title":"Title","message":"Text","actions":[{"id":"refactor","label":"Refactor"}]})"));
  CHECK(parse(R"({"v":1,"type":"action_ack","request_id":"req","action_id":"refactor","accepted":true})"));
  for (const auto& json : {
       R"({"v":true,"type":"notification"})",
       R"({"v":1,"type":"action_ack","request_id":"req","action_id":"refactor","accepted":1})",
       R"({"v":1,"type":"action_ack","request_id":"req","action_id":"refactor","accepted":true,"command":"id"})",
       R"({"v":1,"type":"notification","event_id":"evt","level":"unknown","source":"demo","title":"Title","message":"Text"})",
       R"({"v":1,"type":"notification","event_id":"evt","level":"info","source":"demo","title":"Title","message":"Text","auto_dismiss_ms":true})",
       R"({"v":1,"type":"action_required","request_id":"req","source":"demo","title":"Title","message":"Text","actions":[{"id":"x","label":"X"},{"id":"x","label":"X"}]})",
       R"({"v":1,"type":"action_required","request_id":"req","source":"demo","title":"Title","message":"Text","actions":[{"id":"a","label":"A"},{"id":"b","label":"B"},{"id":"c","label":"C"},{"id":"d","label":"D"},{"id":"e","label":"E"}]})",
       R"({"v":1,"type":"shell","command":"id"})", "{", "[]"}) CHECK(!parse(json));
  CHECK(!parse(std::string(2049, 'x')));
  const std::string valid = R"({"v":1,"type":"action_ack","request_id":"req","action_id":"a","accepted":true})";
  CHECK(parse(valid + " \t\r\n"));
  CHECK(!parse(valid + "garbage"));
  CHECK(!parse(valid + "{}"));
  CHECK(!parse(valid + std::string(1, '\0')));
  CHECK(!parse(R"({"v":1,"type":"action_ack","request_id":"req\u0000other","action_id":"a","accepted":true})"));
  CHECK(parse(R"({"v":1,"type":"wallpaper_sync","op":"begin","transfer_id":"0123456789abcdef","wallpaper_id":"fedcba9876543210","bytes":1024,"crc32":4294967295})"));
  CHECK(parse(R"({"v":1,"type":"wallpaper_sync","op":"commit","transfer_id":"0123456789abcdef","wallpaper_id":"fedcba9876543210"})"));
  for (const auto& json : {
      R"({"v":1,"type":"wallpaper_sync","op":"begin","transfer_id":"0123456789abcdef","wallpaper_id":"fedcba9876543210","bytes":true,"crc32":0})",
      R"({"v":1,"type":"wallpaper_sync","op":"begin","transfer_id":"0123456789abcdef","wallpaper_id":"fedcba9876543210","bytes":134217729,"crc32":0})",
      R"({"v":1,"type":"wallpaper_sync","op":"clear","transfer_id":"0123456789abcdef","wallpaper_id":"fedcba9876543210","path":"/user-file"})",
      R"({"v":1,"type":"wallpaper_sync","op":"format","transfer_id":"0123456789abcdef","wallpaper_id":"fedcba9876543210"})"}) CHECK(!parse(json));
  for (int i = 0; i < 1000; ++i) {
    std::string malformed = "{" + std::string(i % 150, '[');
    CHECK(!parse(malformed));
  }
}

protocol::IncomingMessage parsed(const std::string& json) {
  protocol::IncomingMessage m;
  const char* reason;
  CHECK(protocol::parseIncoming(reinterpret_cast<const uint8_t*>(json.data()), json.size(), m, reason));
  return m;
}

void protocolExtensions() {
  auto four = parsed(R"({"v":1,"type":"action_required","request_id":"cc-0123456789ab-0","source":"eps32","title":"选择","message":"中文消息","actions":[{"id":"o0","label":"红色"},{"id":"o1","label":"蓝色"},{"id":"o2","label":"绿色"},{"id":"o3","label":"黄色","hold_ms":800}]})");
  CHECK(four.actionCount == 4 && four.event.actionCount == 4);
  CHECK(four.event.actions[3].holdMs == 800 && four.event.actions[0].holdMs == 0);
  CHECK(std::string(four.event.actions[1].label) == "蓝色");
  for (const char* hold : {"100", "2001", "true", "-1", "\"800\""}) {
    CHECK(!parse(std::string(R"({"v":1,"type":"action_required","request_id":"r","source":"s","title":"t","message":"m","actions":[{"id":"a","label":"A","hold_ms":)") + hold + "}]}"));
  }
  auto status = parsed(R"({"v":1,"type":"agent_status","sessions":[{"id":"a1b2c3d4","project":"eps32","state":"waiting","tool":"Bash","elapsed_s":42,"age_s":3,"pending":1},{"id":"00000002","project":"项目","state":"done","elapsed_s":192,"age_s":0,"pending":0}]})");
  CHECK(status.type == protocol::IncomingType::AGENT_STATUS && status.agentStatus.count == 2);
  CHECK(status.agentStatus.sessions[0].state == AgentState::Waiting);
  CHECK(std::string(status.agentStatus.sessions[0].tool) == "Bash");
  CHECK(status.agentStatus.sessions[1].elapsedS == 192 && status.agentStatus.sessions[1].tool[0] == '\0');
  CHECK(parse(R"({"v":1,"type":"agent_status","sessions":[]})"));
  auto combined = parsed(R"({"v":1,"type":"agent_status","sessions":[{"id":"a1b2c3d4","source":"codex","project":"eps32","state":"stopped","elapsed_s":42,"age_s":0,"pending":0},{"id":"a1b2c3d4","source":"claude","project":"eps32","state":"working","elapsed_s":12,"age_s":0,"pending":0}]})");
  CHECK(combined.agentStatus.sessions[0].source == AgentSource::Codex);
  CHECK(combined.agentStatus.sessions[0].state == AgentState::Stopped);
  CHECK(combined.agentStatus.sessions[1].source == AgentSource::Claude);
  for (const char* source : {"\"unknown\"", "null", "7", "true"}) {
    CHECK(!parse(std::string(R"({"v":1,"type":"agent_status","sessions":[{"id":"a1b2c3d4","source":)") + source +
        R"(,"project":"p","state":"working","elapsed_s":1,"age_s":0,"pending":0}]})"));
  }
  const std::string ok = R"({"id":"a1b2c3d4","project":"p","state":"working","elapsed_s":1,"age_s":1,"pending":0})";
  for (const std::string& json : {
       std::string(R"({"v":1,"type":"agent_status","sessions":[)") + ok + "," + ok + "," + ok + "," + ok + "," + ok + "]}",
       std::string(R"({"v":1,"type":"agent_status","sessions":[{"id":"A1B2C3D4","project":"p","state":"working","elapsed_s":1,"age_s":1,"pending":0}]})"),
       std::string(R"({"v":1,"type":"agent_status","sessions":[{"id":"a1b2c3d4","project":"p","state":"running","elapsed_s":1,"age_s":1,"pending":0}]})"),
       std::string(R"({"v":1,"type":"agent_status","sessions":[{"id":"a1b2c3d4","project":"p","state":"working","elapsed_s":1,"age_s":1}]})"),
       std::string(R"({"v":1,"type":"agent_status","sessions":[{"id":"a1b2c3d4","project":"p","state":"working","elapsed_s":1,"age_s":1,"pending":0,"prompt":"x"}]})"),
       std::string(R"({"v":1,"type":"agent_status","sessions":[{"id":"a1b2c3d4","project":"","state":"working","elapsed_s":1,"age_s":1,"pending":0}]})"),
       std::string(R"({"v":1,"type":"agent_status","sessions":[{"id":"a1b2c3d4","project":"p","state":"working","elapsed_s":16777216,"age_s":1,"pending":0}]})"),
       std::string(R"({"v":1,"type":"agent_status","sessions":{}})"),
       std::string(R"({"v":1,"type":"agent_status","sessions":[],"extra":1})")}) CHECK(!parse(json));
}

void pairingMessages() {
  auto challenge = parsed(R"({"v":1,"type":"auth_challenge","nonce":"0123456789abcdef0123456789abcdef"})");
  CHECK(challenge.type == protocol::IncomingType::AUTH_CHALLENGE);
  CHECK(std::string(challenge.nonce) == "0123456789abcdef0123456789abcdef");
  for (const char* json : {
       R"({"v":1,"type":"auth_challenge","nonce":"0123456789ABCDEF0123456789abcdef"})",
       R"({"v":1,"type":"auth_challenge","nonce":"0123"})",
       R"({"v":1,"type":"auth_challenge","nonce":"0123456789abcdef0123456789abcdef","key":"x"})",
       R"({"v":1,"type":"auth_challenge"})"}) CHECK(!parse(json));
  const String plain = protocol::makeHello("t", false, false);
  const String paired = protocol::makeHello("t", true, true);
  CHECK(std::string(plain.c_str()).find("claude_companion") == std::string::npos);
  CHECK(std::string(paired.c_str()).find("\"claude_companion\"") != std::string::npos);
  CHECK(std::string(paired.c_str()).find("\"events_v2\"") != std::string::npos);
  const std::string response = protocol::makeAuthResponse(std::string(64, 'a').c_str()).c_str();
  CHECK(response == R"({"v":1,"type":"auth_response","mac":")" + std::string(64, 'a') + "\"}");
}

void backlightRules() {
  DisplaySettings s;  // 23:00-07:00 night 10%, wake 40%, day 100%, idle 5 min -> 30%
  CHECK(validDisplaySettings(s));
  BacklightInputs in;
  in.timeValid = true;
  in.minuteOfDay = 12 * 60;
  in.sinceInteractionMs = 0;
  CHECK(targetBacklight(s, in) == 100);
  in.sinceInteractionMs = 299999;
  CHECK(targetBacklight(s, in) == 100);
  in.sinceInteractionMs = 300000;
  CHECK(targetBacklight(s, in) == 30);
  in.attention = true;                       // a card wakes the screen
  CHECK(targetBacklight(s, in) == 100);
  in.attention = false;
  for (uint16_t minute : {uint16_t(23 * 60), uint16_t(23 * 60 + 59), uint16_t(0), uint16_t(6 * 60 + 59)}) {
    in.minuteOfDay = minute;                 // night wraps over midnight
    CHECK(isNight(s, minute) && targetBacklight(s, in) == 10);
  }
  in.minuteOfDay = 7 * 60;
  CHECK(!isNight(s, in.minuteOfDay) && targetBacklight(s, in) == 30);
  in.minuteOfDay = 2 * 60;
  in.sinceInteractionMs = 1000;
  CHECK(targetBacklight(s, in) == 40);       // touched at night
  in.timeValid = false;                      // no clock: day rules only
  CHECK(targetBacklight(s, in) == 100);
  DisplaySettings sameDay = s;
  sameDay.nightStartMin = 9 * 60; sameDay.nightEndMin = 17 * 60;
  CHECK(isNight(sameDay, 12 * 60) && !isNight(sameDay, 17 * 60) && !isNight(sameDay, 8 * 60));
  DisplaySettings never = s; never.nightEndMin = never.nightStartMin;
  CHECK(!isNight(never, 23 * 60));
  DisplaySettings noIdle = s; noIdle.idleDimS = 0;
  in.timeValid = true; in.minuteOfDay = 12 * 60; in.sinceInteractionMs = 3600000;
  CHECK(targetBacklight(noIdle, in) == 100);
  DisplaySettings bad = s; bad.idleLevel = 101;
  CHECK(!validDisplaySettings(bad));
  bad = s; bad.nightWakeLevel = 5;
  CHECK(!validDisplaySettings(bad));

  BacklightFader fader;
  CHECK(fader.step(100, 0) == 100);          // starts at the first target
  CHECK(fader.step(10, 1000) == 100);
  CHECK(fader.step(10, 1150) == 55);
  CHECK(fader.step(10, 1300) == 10);         // either direction takes 300 ms
  fader.step(40, 1400);
  CHECK(fader.step(40, 1550) == 25);
  CHECK(fader.step(40, 1700) == 40);
  fader.step(100, 2000, 2000);
  CHECK(fader.step(100, 3000) == 70);       // day boundary retains its 2 s fade
  CHECK(fader.step(100, 4000) == 100);
}

void clockMessagesAndFormatting() {
  auto clock = parsed(R"({"v":1,"type":"clock","epoch_s":1790850000,"utc_offset_s":28800})");
  CHECK(clock.type == protocol::IncomingType::CLOCK);
  CHECK(clock.clockEpoch == 1790850000UL && clock.clockOffsetS == 28800);
  CHECK(parse(R"({"v":1,"type":"clock","epoch_s":1790850000,"utc_offset_s":-18000})"));
  for (const char* json : {
       R"({"v":1,"type":"clock","epoch_s":1790850000,"utc_offset_s":90000})",
       R"({"v":1,"type":"clock","epoch_s":100,"utc_offset_s":0})",
       R"({"v":1,"type":"clock","epoch_s":true,"utc_offset_s":0})",
       R"({"v":1,"type":"clock","epoch_s":1790850000})",
       R"({"v":1,"type":"clock","epoch_s":1790850000,"utc_offset_s":0,"tz":"x"})"}) CHECK(!parse(json));
  auto settings = parsed(R"({"v":1,"type":"display_settings","night_start_min":1380,"night_end_min":420,"day_level":90,"night_level":8,"night_wake_level":35,"idle_dim_s":120,"idle_level":25})");
  CHECK(settings.type == protocol::IncomingType::DISPLAY_SETTINGS);
  CHECK(settings.displaySettings.dayLevel == 90 && settings.displaySettings.idleDimS == 120 &&
        settings.displaySettings.nightEndMin == 420);
  for (const char* json : {
       R"({"v":1,"type":"display_settings","night_start_min":1440,"night_end_min":420,"day_level":90,"night_level":8,"night_wake_level":35,"idle_dim_s":120,"idle_level":25})",
       R"({"v":1,"type":"display_settings","night_start_min":1380,"night_end_min":420,"day_level":90,"night_level":8,"night_wake_level":5,"idle_dim_s":120,"idle_level":25})",
       R"({"v":1,"type":"display_settings","night_start_min":1380,"night_end_min":420,"day_level":90,"night_level":8,"night_wake_level":35,"idle_dim_s":120})",
       R"({"v":1,"type":"display_settings","night_start_min":1380,"night_end_min":420,"day_level":90,"night_level":8,"night_wake_level":35,"idle_dim_s":120,"idle_level":25,"x":1})"}) CHECK(!parse(json));
  struct tm local {};
  local.tm_mon = 9; local.tm_mday = 1; local.tm_wday = 3; local.tm_hour = 9; local.tm_min = 5;
  char date[48], time[8];
  uifont::GlassClock::formatDate(local, date, sizeof(date));
  uifont::GlassClock::formatTime(local, time, sizeof(time));
  CHECK(std::string(date) == "10月1日 周三");
  CHECK(std::string(time) == "09:05");
  local.tm_wday = 0;
  uifont::GlassClock::formatDate(local, date, sizeof(date));
  CHECK(std::string(date) == "10月1日 周日");
}

void fourButtonGrid() {
  UiManager ui; ui.begin(1);
  CHECK(ui.receiveEvent(event("grid", EventType::ActionRequired, 4), 0));
  for (size_t i = 0; i < 4; ++i) {
    const auto r = ui.eventButtonRect(i);
    CHECK(r.width >= 100 && r.height >= 48);
    CHECK(r.x >= 20 && r.x + r.width <= board::kDisplayWidth - 20);
    CHECK(r.y + r.height <= board::kDisplayHeight - 24);
    for (size_t j = 0; j < i; ++j) {
      const auto o = ui.eventButtonRect(j);
      CHECK(r.x >= o.x + o.width || o.x >= r.x + r.width ||
            r.y >= o.y + o.height || o.y >= r.y + r.height);
    }
  }
  // Leave room for the header, title and at least one message line.
  CHECK(ui.eventButtonRect(0).y >= 28 + 120);
}

void holdToConfirm() {
  auto holdEvent = [] {
    auto e = event("hold", EventType::ActionRequired, 2);
    e.actions[0].holdMs = 800;
    return e;
  };
  // A quick tap on a hold button never dispatches, and shows the hint.
  {
    UiManager ui; ui.begin(1); ui.setConnectionIndicator(ConnectionIndicator::BRIDGE_CONNECTED);
    UiActionEvent a;
    CHECK(ui.receiveEvent(holdEvent(), 100));
    fake_arduino::setMillis(102); ui.handleTouch(false, {}, 102, a);
    const auto r = ui.eventButtonRect(0);
    const TouchPoint p{uint16_t(r.x + r.width / 2), uint16_t(r.y + r.height / 2)};
    for (uint32_t t = 110; t <= 300; t += 10) { fake_arduino::setMillis(t); CHECK(!ui.handleTouch(true, p, t, a)); }
    for (uint32_t t = 310; t <= 450; t += 10) { fake_arduino::setMillis(t); CHECK(!ui.handleTouch(false, p, t, a)); }
    CHECK(!a.valid && ui.events().statistics().actionAttempts == 0);
    // Holding long enough dispatches exactly once, while still pressed.
    bool dispatched = false;
    for (uint32_t t = 500; t <= 1400; t += 10) {
      fake_arduino::setMillis(t);
      if (ui.handleTouch(true, p, t, a) && a.valid) {
        CHECK(!dispatched && t >= 500 + 800);
        dispatched = true;
        CHECK(std::string(a.actionId) == "option-0");
      }
    }
    CHECK(dispatched && ui.events().statistics().actionAttempts == 1);
  }
  // A complete long contact processed late (buffered samples) still counts,
  // measured from first to last DOWN sample.
  {
    UiManager ui; ui.begin(1); ui.setConnectionIndicator(ConnectionIndicator::BRIDGE_CONNECTED);
    UiActionEvent a;
    CHECK(ui.receiveEvent(holdEvent(), 100));
    fake_arduino::setMillis(102); ui.handleTouch(false, {}, 102, a);
    const auto r = ui.eventButtonRect(0);
    const TouchPoint p{uint16_t(r.x + 5), uint16_t(r.y + 5)};
    fake_arduino::setMillis(1200);
    ui.handleTouch(true, p, 110, a);
    ui.handleTouch(true, p, 950, a);
    CHECK(a.valid || ui.events().statistics().actionAttempts == 1);
  }
  // Sliding off the hold button cancels it.
  {
    UiManager ui; ui.begin(1); ui.setConnectionIndicator(ConnectionIndicator::BRIDGE_CONNECTED);
    UiActionEvent a;
    CHECK(ui.receiveEvent(holdEvent(), 100));
    fake_arduino::setMillis(102); ui.handleTouch(false, {}, 102, a);
    const auto r = ui.eventButtonRect(0);
    const TouchPoint on{uint16_t(r.x + 5), uint16_t(r.y + 5)};
    const TouchPoint off{uint16_t(r.x + r.width + 30), uint16_t(r.y - 40)};
    for (uint32_t t = 110; t <= 1500; t += 10) {
      fake_arduino::setMillis(t);
      CHECK(!ui.handleTouch(true, t < 400 ? on : off, t, a));
    }
    CHECK(ui.events().statistics().actionAttempts == 0);
  }
  // Ordinary buttons on the same card keep the quick-tap behaviour.
  {
    UiManager ui; ui.begin(1); ui.setConnectionIndicator(ConnectionIndicator::BRIDGE_CONNECTED);
    UiActionEvent a;
    CHECK(ui.receiveEvent(holdEvent(), 100));
    fake_arduino::setMillis(102); ui.handleTouch(false, {}, 102, a);
    const auto r = ui.eventButtonRect(1);
    const TouchPoint p{uint16_t(r.x + 5), uint16_t(r.y + 5)};
    fake_arduino::setMillis(110); ui.handleTouch(true, p, 110, a);
    fake_arduino::setMillis(170);
    CHECK(ui.handleTouch(true, p, 170, a) && std::string(a.actionId) == "option-1");
  }
}

AgentStatusSnapshot agents(std::initializer_list<std::pair<AgentState, uint32_t>> states) {
  AgentStatusSnapshot snapshot;
  for (const auto& item : states) {
    AgentSession& s = snapshot.sessions[snapshot.count++];
    snprintf(s.id, sizeof(s.id), "%08x", unsigned(snapshot.count));
    strlcpy(s.project, "eps32", sizeof(s.project));
    s.state = item.first;
    s.ageS = item.second;
  }
  return snapshot;
}

void agentHudVisibility() {
  UiManager ui; ui.begin(1);
  CHECK(ui.visibleAgentSession(0) == -1);
  ui.updateAgentStatus(agents({{AgentState::Idle, 0}, {AgentState::Working, 0}}), 1000);
  CHECK(ui.visibleAgentSession(1000) == 1);
  CHECK(ui.visibleAgentSession(1000 + 30000) == 1);
  CHECK(ui.visibleAgentSession(1000 + 30001) == -1);  // Bridge went quiet
  ui.updateAgentStatus(agents({{AgentState::Done, 1}}), 50000);
  CHECK(ui.visibleAgentSession(50000) == 0);
  CHECK(ui.visibleAgentSession(52999) == 0);
  CHECK(ui.visibleAgentSession(53000) == -1);          // finished turn fades
  ui.updateAgentStatus(agents({{AgentState::Waiting, 0}}), 60000);
  CHECK(ui.visibleAgentSession(60000) == 0);
  ui.clearAgentStatus();
  CHECK(ui.visibleAgentSession(60000) == -1);
  ui.updateAgentStatus(agents({{AgentState::Idle, 0}}), 70000);
  CHECK(ui.visibleAgentSession(70000) == -1);
}

void agentHudRotation() {
  UiManager ui; ui.begin(1);
  auto snapshot = agents({{AgentState::Working, 0}, {AgentState::Working, 0}});
  snapshot.sessions[1].source = AgentSource::Codex;
  ui.updateAgentStatus(snapshot, 1000);
  CHECK(ui.visibleAgentCount(1000) == 2);
  CHECK(ui.visibleAgentSession(4999) == 0);
  CHECK(ui.visibleAgentSession(5000) == 1);
  ui.updateAgentStatus(snapshot, 6000);  // refresh must not reset rotation
  CHECK(ui.visibleAgentSession(6000) == 1);
  CHECK(ui.visibleAgentSession(9000) == 0);
  CHECK(ui.receiveEvent(event("pause", EventType::ActionRequired), 6500));
  ui.tick(6500);
  CHECK(ui.visibleAgentSession(16500) == 1);  // ten seconds hidden, same task
  fake_arduino::setMillis(16500);
  ui.dismiss("pause"); ui.tick(16500);
  CHECK(ui.visibleAgentSession(18999) == 1);
  CHECK(ui.visibleAgentSession(19000) == 0);
  snapshot.sessions[1].state = AgentState::Waiting;
  ui.updateAgentStatus(snapshot, 20000);
  CHECK(ui.visibleAgentSession(20000) == 1);  // new wait is immediate
  CHECK(ui.visibleAgentSession(24000) == 0);
  snapshot.sessions[0].state = AgentState::Stopped;
  ui.updateAgentStatus(snapshot, 25000);
  CHECK(ui.visibleAgentCount(28999) == 2);
  CHECK(ui.visibleAgentCount(29000) == 1);
  CHECK(ui.visibleAgentCount(55001) == 0);
}

void agentHudCjkLabel() {
  UiManager ui; ui.begin(1);
  auto snapshot = agents({{AgentState::Working, 0}});
  AgentSession& session = snapshot.sessions[0];
  session.source = AgentSource::Codex;
  strlcpy(session.project, "中文项目中文项目中文", sizeof(session.project));
  strlcpy(session.tool, "Bash", sizeof(session.tool));
  session.elapsedS = 42;
  ui.updateAgentStatus(snapshot, 1000);
  Arduino_Canvas canvas;
  ui.render(canvas, 1000);
  bool label = false, counter = false, clock = false;
  for (const auto& printed : canvas.printed) {
    // The home quota dock also starts with Codex; the HUD uses two separators.
    if (printed.rfind("Codex  ", 0) == 0) {
      label = true;
      CHECK(printed.find("执行命令") != std::string::npos);
      CHECK(printed.find("...") != std::string::npos);
      CHECK(text::width(printed.c_str(), 1) <= 220);
    }
    counter |= printed == "1/1";
    clock |= printed == "0:42";
  }
  CHECK(label && counter && clock);
}

void cjkTextLayout() {
  CHECK(text::width("abc", 1) == 24 && text::width("中文", 1) == 32 && text::width("中a", 2) == 48);
  size_t next = 0;
  // ASCII words break at spaces; CJK may break between any two characters.
  const char* mixed = "hello world again";
  const size_t take = text::fitLine(mixed, 8 * 13, 1, next);
  CHECK(std::string(mixed, take) == "hello world" && std::string(mixed + next) == "again");
  const char* cjk = "一二三四五六";
  CHECK(text::fitLine(cjk, 16 * 4 + 7, 1, next) == 12 && next == 12);
  // Never splits a multi-byte character, even for a too-narrow line.
  CHECK(text::fitLine(cjk, 4, 1, next) == 3);
  char out[64];
  text::ellipsize("这是一个很长很长的标题文本", 16 * 6, 1, out, sizeof(out));
  CHECK(text::width(out, 1) <= 16 * 6 && std::string(out).size() >= 3 &&
        std::string(out).substr(std::string(out).size() - 3) == "...");
  text::ellipsize("short", 200, 1, out, sizeof(out));
  CHECK(std::string(out) == "short");
  text::normalize("a\u2026b\u201cc\u201d\u00b7d\ne", out, sizeof(out));
  CHECK(std::string(out) == "a...b\"c\"-d e");
  text::normalize("2\u00d72", out, sizeof(out));
  CHECK(std::string(out) == "2x2");
  text::normalize("ok\xff\xfe", out, sizeof(out));
  CHECK(std::string(out) == "ok??");
  text::normalize("中文中文", out, 8);  // room for two characters only
  CHECK(std::string(out) == "中文");
  uint32_t cp = 0;
  CHECK(text::decode("\xe4\xb8", cp) == 1 && cp == 0xFFFD);   // truncated
  CHECK(text::decode("\xc0\x80", cp) == 1 && cp == 0xFFFD);   // overlong
}


void otaMessages() {
  protocol::IncomingMessage incoming;
  const char* reason = nullptr;
  const std::string valid = R"({"v":1,"type":"ota_offer","transfer_id":"0123456789abcdef","version":"0.7.1","build":"Oct  1 2026 12:00:00","bytes":1500000,"sha256":")" + std::string(64,'a') + R"(","mac":")" + std::string(64,'b') + R"("})";
  auto parse = [&](const std::string& json) { return protocol::parseIncoming(reinterpret_cast<const uint8_t*>(json.data()),json.size(),incoming,reason); };
  CHECK(parse(valid)); CHECK(incoming.type == protocol::IncomingType::OTA_OFFER);
  CHECK(incoming.cacheBytes == 1500000); CHECK(strcmp(incoming.otaVersion,"0.7.1") == 0);
  for (const std::string& replacement : {std::string("true"), std::string("0"), std::string("6553601")}) {
    auto bad=valid; bad.replace(bad.find("1500000"),7,replacement); CHECK(!parse(bad));
  }
  auto extra=valid;extra.insert(extra.size()-1,",\"extra\":1");CHECK(!parse(extra));
  CHECK(parse(R"({"v":1,"type":"ota_control","transfer_id":"0123456789abcdef","op":"commit"})"));
  CHECK(incoming.type == protocol::IncomingType::OTA_CONTROL);
  CHECK(!parse(R"({"v":1,"type":"ota_control","transfer_id":"0123456789abcdef","op":"install"})"));
  // An update progress screen consumes physical contacts without dispatching
  // queued permission actions or ambient page gestures.
  UiManager ui;ui.begin(1);ui.setOtaProgress(true,50,false);UiActionEvent action;
  CHECK(!ui.handleTouch(true,TouchPoint{220,240},100,action));CHECK(!action.valid);
}

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> tests = {
      {"single action + exact ACK + completed dedup", ackAndIdempotentTouch},
      {"disconnect, immutable Retry, timeout, late ACK", reconnectAndRetry},
      {"rejection allows a new choice", rejectedChoiceCanChange},
      {"bounded priority queue and overflow", queueAndOverflow},
      {"preemption, clock wrap, bounded RAM history", preemptionHistoryAndExpiry},
      {"restore Mac status and honor its own timeout", restoreHostPageAndRespectTimeout},
      {"one/two/three button layout and touch Dismiss", layoutsAndNotificationDismiss},
      {"actual JSON parser rejects malformed/unsafe input", strictProtocolAndMalformedInput},
      {"four actions, hold_ms and agent_status parsing", protocolExtensions},
      {"pairing challenge, response and hello capability", pairingMessages},
      {"four buttons form a 2x2 grid", fourButtonGrid},
      {"backlight rules and fading", backlightRules},
      {"clock and display settings messages, date formatting", clockMessagesAndFormatting},
      {"strict OTA protocol and progress input barrier", otaMessages},
      {"hold-to-confirm buttons", holdToConfirm},
      {"agent HUD visibility rules", agentHudVisibility},
      {"agent HUD rotation, refresh, urgent wait and modal pause", agentHudRotation},
      {"agent HUD long CJK project preserves operation and ordinal", agentHudCjkLabel},
      {"CJK text measurement and wrapping", cjkTextLayout},
  };
  size_t passed = 0;
  for (const auto& t : tests) {
    fake_arduino::resetPinsAndClock(); Serial.clear();
    try { t.second(); ++passed; std::cout << "PASS " << t.first << '\n'; }
    catch (const std::exception& e) { std::cerr << "FAIL " << t.first << ": " << e.what() << '\n'; }
  }
  std::cout << passed << '/' << tests.size() << " event tests passed\n";
  return passed == tests.size() ? 0 : 1;
}
