#include "ui/ui_manager.h"
#include "board_config.h"
#include "ui/environment_chart.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Exercise the real navigation, input fencing, event handling and rendering.
// Only pixels, font data and physical video decoding are substituted.
namespace uifont { const uint8_t* cjkFont() { return nullptr; } }
void Wallpaper::begin(uint32_t) {}
void Wallpaper::render(Arduino_Canvas&, uint32_t) {}
bool Wallpaper::selectVideo(const char*) { return true; }
bool Wallpaper::decodeVideoFrame(const char*, const uint8_t*, size_t, uint32_t) { return true; }
bool Wallpaper::videoFresh(uint32_t) const { return false; }

namespace {
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
  std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #condition); \
} while (false)

struct Fixture {
  UiManager ui;
  std::vector<uint16_t> pixels = std::vector<uint16_t>(
      board::kPanelWidth * board::kPanelHeight, 0x7BEF);
  Arduino_Canvas gfx{pixels.data()};

  explicit Fixture(uint32_t start = 0) {
    fake_arduino::resetPinsAndClock();
    Serial.clear();
    fake_arduino::setMillis(start);
    ui.begin(0);
    touch(false, 0, 0, start);
  }
  void touch(bool down, uint16_t x, uint16_t y, uint32_t at) {
    fake_arduino::setMillis(at);
    UiActionEvent ignored;
    ui.handleTouch(down, TouchPoint{x, y}, at, ignored);
  }
  void tap(uint16_t x, uint16_t y, uint32_t at) {
    touch(true, x, y, at);
    touch(false, x, y, at + 100U);
  }
  void swipe(bool next, uint32_t at) {
    const uint16_t from = next ? 400 : 200, to = next ? 200 : 400;
    touch(true, from, 310, at);
    touch(true, to, 310, at + 100U);
    touch(false, to, 310, at + 200U);
  }
  void environment(uint32_t at = 100) {
    swipe(true, at);
    CHECK(ui.baseState() == UiState::HOST_STATUS);
    swipe(true, at + 300U);
    CHECK(ui.baseState() == UiState::ENVIRONMENT);
  }
  void render(uint32_t at) {
    gfx.printed.clear();
    gfx.textPositions.clear();
    gfx.lines.clear();
    gfx.circles.clear();
    ui.render(gfx, at);
  }
  bool contains(const std::string& value) const {
    return std::any_of(gfx.printed.begin(), gfx.printed.end(), [&](const std::string& item) {
      return item.find(value) != std::string::npos;
    });
  }
  size_t chartLines(uint16_t color) const {
    return std::count_if(gfx.lines.begin(), gfx.lines.end(), [=](const auto& line) {
      return line.color == color && line.y1 >= 142 && line.y1 <= 226 &&
          line.y2 >= 142 && line.y2 <= 226;
    });
  }
  size_t chartPoints(uint16_t color) const {
    return std::count_if(gfx.circles.begin(), gfx.circles.end(), [=](const auto& point) {
      return point.color == color && point.radius == 1 &&
          point.x >= 66 && point.x <= 446 && point.y >= 142 && point.y <= 226;
    });
  }
};

struct HistoryFixture {
  std::array<sensors::AirHistoryBucket, sensors::kAirHistoryBuckets> buckets{};
  sensors::AirHistoryView view;
  HistoryFixture() {
    view.state = sensors::HistoryState::Ready;
    view.clockValid = true;
    view.dayOrdinal = view.todayOrdinal = 20738;
    view.year = 2026;
    view.month = 10;
    view.day = 12;
    view.buckets = buckets.data();
  }
  void point(uint16_t index, uint16_t co2 = 1270, int16_t temperature = 2450,
             uint16_t humidity = 5120, bool connected = false) {
    buckets[index] = {true, connected, co2, humidity, temperature, 1};
    ++view.validMinutes;
  }
};

sensors::SensorSnapshot reading(uint32_t at) {
  sensors::SensorSnapshot value;
  value.air.health = sensors::Health::Healthy;
  value.air.hasSample = true;
  value.air.sampledAtMs = at;
  value.air.co2Ppm = 1270;
  value.air.temperatureC = 24.5f;
  value.air.humidityRh = 51.2f;
  value.radar.health = sensors::Health::Healthy;
  value.radar.hasSample = true;
  value.radar.sample.receivedAtMs = at;
  value.radar.sample.presenceKnown = true;
  value.radar.sample.presence = true;
  return value;
}

void existingStateValuesDoNotShift() {
  CHECK(static_cast<uint8_t>(UiState::WALLPAPER) == 0);
  CHECK(static_cast<uint8_t>(UiState::MAC_CONTROL) == 2);
  CHECK(static_cast<uint8_t>(UiState::USAGE) == 5);
  CHECK(static_cast<uint8_t>(UiState::SETUP) == 12);
  CHECK(static_cast<uint8_t>(UiState::ENVIRONMENT) == 13);
}

void allPagesNavigateAndRenderInBothDirections() {
  Fixture f;
  f.ui.setHubOnline(true, 0);
  const UiState pages[] = {UiState::HOST_STATUS, UiState::ENVIRONMENT,
      UiState::MAC_CONTROL, UiState::INFO_CARDS, UiState::TASKS, UiState::USAGE};
  const char* labels[] = {"MAC SYSTEM", "本地环境", "Mac 控制", "信息卡", "任务", "AI 用量"};
  uint32_t at = 100;
  for (size_t i = 0; i < 6; ++i) {
    f.swipe(true, at);
    CHECK(f.ui.baseState() == pages[i]);
    f.render(at + 200);
    CHECK(f.contains(labels[i]));
    at += 300;
  }
  hub::Action action;
  CHECK(f.ui.takeHubAction(action));
  CHECK(std::string(action.action) == "usage_refresh");
  f.swipe(true, at);  // No navigation beyond the last page.
  CHECK(f.ui.baseState() == UiState::USAGE);
  at += 300;
  for (int i = 4; i >= 0; --i) {
    f.swipe(false, at);
    CHECK(f.ui.baseState() == pages[i]);
    at += 300;
  }
  f.swipe(false, at);
  CHECK(f.ui.baseState() == UiState::WALLPAPER);
  f.swipe(false, at + 300);
  CHECK(f.ui.baseState() == UiState::WALLPAPER);
}

void homeShortcutsKeepHubDestinations() {
  Fixture tasks;
  tasks.tap(60, 240, 100);
  CHECK(tasks.ui.baseState() == UiState::TASKS);
  tasks.render(200);
  CHECK(tasks.contains("任务"));
  tasks.tap(40, 24, 300);
  CHECK(tasks.ui.baseState() == UiState::WALLPAPER);

  Fixture usage;
  usage.ui.setHubOnline(true, 0);
  usage.tap(200, 240, 100);
  CHECK(usage.ui.baseState() == UiState::USAGE);
  usage.render(200);
  CHECK(usage.contains("AI 用量"));
  hub::Action action;
  CHECK(usage.ui.takeHubAction(action));
  CHECK(std::string(action.action) == "usage_refresh");
  usage.tap(40, 24, 300);
  CHECK(usage.ui.baseState() == UiState::WALLPAPER);
}

void controlButtonsKeepHubPageId() {
  Fixture f;
  f.ui.setHubOnline(true, 0);
  f.environment();
  f.swipe(true, 700);
  CHECK(f.ui.baseState() == UiState::MAC_CONTROL);
  f.tap(300, 276, 1000);
  hub::Action action;
  CHECK(f.ui.takeHubAction(action));
  CHECK(std::string(action.action) == "control_refresh");
  f.tap(40, 24, 1300);
  CHECK(f.ui.baseState() == UiState::WALLPAPER);
}

void switchesDispatchOnlyCompletedTaps() {
  Fixture f;
  f.environment();
  CHECK(f.ui.presenceWakeEnabled() && f.ui.co2ReminderEnabled());
  f.touch(true, 100, 280, 700);
  f.touch(true, 100, 280, 780);
  CHECK(f.ui.presenceWakeEnabled());
  CHECK(!f.ui.takeSensorOptionsChange());
  f.touch(false, 100, 280, 880);
  CHECK(!f.ui.presenceWakeEnabled());
  CHECK(f.ui.co2ReminderEnabled());
  CHECK(f.ui.takeSensorOptionsChange());
  CHECK(!f.ui.takeSensorOptionsChange());
  f.tap(330, 280, 1000);
  CHECK(!f.ui.co2ReminderEnabled());
  CHECK(f.ui.takeSensorOptionsChange());
  // Holding or sliding off the small target cannot silently toggle it.
  f.touch(true, 330, 280, 1300);
  f.touch(true, 330, 280, 2000);
  f.touch(false, 330, 280, 2100);
  CHECK(!f.ui.co2ReminderEnabled());
  f.touch(true, 252, 280, 2300);
  f.touch(true, 240, 280, 2310);
  f.touch(false, 240, 280, 2400);
  CHECK(!f.ui.co2ReminderEnabled());
  CHECK(!f.ui.takeSensorOptionsChange());
  f.ui.setSensorOptions(true, false);
  CHECK(f.ui.presenceWakeEnabled() && !f.ui.co2ReminderEnabled());
  CHECK(!f.ui.takeSensorOptionsChange());  // Loading NVS is not a user edit.
  f.tap(50, 30, 2600);
  CHECK(f.ui.baseState() == UiState::WALLPAPER);
}

void dimmedFirstGestureOnlyWakes() {
  Fixture f;
  f.environment();
  f.ui.setScreenDimmed(true);
  f.tap(100, 280, 700);
  CHECK(f.ui.lastInteractionMs() == 700);
  CHECK(f.ui.presenceWakeEnabled());
  CHECK(!f.ui.takeSensorOptionsChange());
  f.ui.setScreenDimmed(false);
  f.tap(100, 280, 1000);
  CHECK(!f.ui.presenceWakeEnabled());
}

void staleOrRecoveringValuesNeverLookLive() {
  Fixture f;
  f.environment();
  auto value = reading(1000);
  f.ui.updateSensorSnapshot(value, 1000);
  f.render(1000);
  CHECK(f.contains("1270") && f.contains("24.5 C") && f.contains("51.2%"));
  CHECK(f.contains("有人") && !f.contains("有人状态未知"));
  f.render(16001);
  CHECK(!f.contains("1270") && !f.contains("24.5 C") && !f.contains("51.2%"));
  CHECK(f.contains("数据过期") && f.contains("有人状态未知"));
  value.air.health = sensors::Health::Recovering;
  value.radar.health = sensors::Health::Offline;
  f.ui.updateSensorSnapshot(value, 16001);
  f.render(16001);
  CHECK(f.contains("恢复中") && f.contains("未连接"));
  CHECK(!f.contains("1270"));
  value = reading(17000);
  value.radar.sample.presence = false;
  f.ui.updateSensorSnapshot(value, 17000);
  f.render(17000);
  CHECK(f.contains("1270") && f.contains("无人"));
}

void startupAndHighStateStayClearWithoutBridge() {
  Fixture f;
  f.environment();
  sensors::SensorSnapshot starting;
  starting.air.health = starting.radar.health = sensors::Health::Starting;
  f.ui.updateSensorSnapshot(starting, 700);
  f.render(700);
  CHECK(f.contains("启动中") && f.contains("尚无读数") && f.contains("有人状态未知"));
  auto value = reading(1000);
  value.air.co2Ppm = 1400;
  f.ui.setSensorOptions(true, false);
  f.ui.updateSensorSnapshot(value, 1000, true);
  f.render(1000);
  CHECK(f.contains("1400") && f.contains("偏高，请通风"));
  CHECK(!f.ui.co2ReminderEnabled());
  f.ui.setConnectionIndicator(ConnectionIndicator::NO_WIFI);
  f.ui.setHubOnline(false, 1100);
  f.render(1100);
  CHECK(f.contains("1400") && f.contains("本地环境"));
}

void eventsAndOtaFenceEnvironmentInput() {
  Fixture f;
  f.environment();
  f.touch(true, 100, 280, 700);
  CompanionEvent event;
  strlcpy(event.id, "local-info", sizeof(event.id));
  strlcpy(event.title, "测试提醒", sizeof(event.title));
  event.autoDismissMs = 12000;
  fake_arduino::setMillis(750);
  CHECK(f.ui.receiveEvent(event, 750));
  CHECK(f.ui.state() == UiState::NOTIFICATION);
  f.touch(false, 100, 280, 900);
  CHECK(f.ui.presenceWakeEnabled());
  f.render(1000);
  CHECK(f.contains("测试提醒") && !f.contains("本地环境"));
  f.ui.setOtaProgress(true, 50, false);
  f.render(1100);
  CHECK(f.contains("正在更新固件") && !f.contains("测试提醒"));
  f.tap(100, 280, 1200);
  CHECK(f.ui.presenceWakeEnabled());
  f.ui.setOtaProgress(false, 0, false);
  fake_arduino::setMillis(12750);
  f.ui.tick(12750);
  CHECK(!f.ui.overlayActive() && f.ui.baseState() == UiState::ENVIRONMENT);
  f.render(12750);
  CHECK(f.contains("本地环境"));
  f.touch(false, 0, 0, 12800);
  f.tap(100, 280, 12900);
  CHECK(!f.ui.presenceWakeEnabled());
}

void setupKeepsPriorityAndReturnWorks() {
  Fixture f;
  f.environment();
  f.ui.showSetup("TEST", "test-pass", "");
  f.render(700);
  CHECK(f.contains("SETUP MODE") && !f.contains("本地环境"));
  f.touch(false, 0, 0, 700);
  f.tap(200, 270, 800);
  CHECK(f.ui.takeSetupExitRequest());
  CHECK(f.ui.baseState() == UiState::WALLPAPER);
}

void navigationAndFreshnessSurviveWrap() {
  const uint32_t start = std::numeric_limits<uint32_t>::max() - 500;
  Fixture f(start);
  f.environment(start + 100U);
  auto value = reading(start + 400U);
  f.ui.updateSensorSnapshot(value, 200);
  f.render(200);
  CHECK(f.contains("1270") && !f.contains("有人状态未知"));
  f.render(15200);
  CHECK(!f.contains("1270") && f.contains("数据过期"));
  f.tap(50, 30, 16000);
  CHECK(f.ui.baseState() == UiState::WALLPAPER);
  f.environment(16300);
  fake_arduino::setMillis(76900);
  f.ui.tick(76900);
  CHECK(f.ui.baseState() == UiState::WALLPAPER);
}

void radarShowsOnlyQualifiedDistancesAndBootMaximum() {
  Fixture f;
  f.environment();
  auto value = reading(1000);
  value.radar.sample.moving = true;
  value.radar.sample.stationary = true;
  value.radar.sample.movingDistanceCm = 75;
  value.radar.sample.stillDistanceCm = 240;
  value.radar.sample.movingEnergy = 46;
  value.radar.sample.stillEnergy = 81;
  f.ui.updateSensorSnapshot(value, 1000);
  f.render(1000);
  CHECK(f.contains("最近 0.75 m"));
  f.tap(390, 68, 1100);
  f.render(1200);
  CHECK(f.contains("房间有人") && f.contains("0.75 m") && f.contains("2.40 m"));
  CHECK(f.contains("75 cm / ENERGY 46") && f.contains("240 cm / ENERGY 81"));
  CHECK(f.contains("1 m 内有人") && f.contains("本次最远报告 2.40 m (240 cm)"));
  CHECK(f.contains("无法识别人数"));
  value.radar.sample.stationary = false;  // Retained static fields are invalid.
  value.radar.sample.movingEnergy = 0;
  f.ui.updateSensorSnapshot(value, 1300);
  f.render(1300);
  CHECK(!f.contains("0.75 m") && !f.contains("240 cm / ENERGY 81"));
  CHECK(f.contains("距离未知") && f.contains("-- cm / ENERGY --"));
  CHECK(f.contains("本次最远报告 2.40 m (240 cm)"));
  value.radar.sample.presence = false;
  f.ui.updateSensorSnapshot(value, 1400);
  f.render(1400);
  CHECK(f.contains("房间无人") && f.contains("无人：等待空闲调暗"));
  value.radar.sample.presence = true;
  value.radar.sample.movingEnergy = 90;
  value.radar.sample.movingDistanceCm = 600;
  value.radar.sample.receivedAtMs = 0;
  f.ui.updateSensorSnapshot(value, 3001);  // A stale long value cannot alter max.
  f.render(3001);
  CHECK(f.contains("有人状态未知") && !f.contains("6.00 m"));
  CHECK(f.contains("本次最远报告 2.40 m (240 cm)"));
}

void radarNearBoundaryAndDisabledWakeAreExplicit() {
  Fixture f;
  f.environment();
  f.tap(390, 68, 700);
  auto value = reading(1000);
  value.radar.sample.moving = true;
  value.radar.sample.movingEnergy = 1;
  value.radar.sample.movingDistanceCm = 100;
  f.ui.updateSensorSnapshot(value, 1000);
  f.render(1000);
  CHECK(f.contains("1 m 内有人"));
  value.radar.sample.movingDistanceCm = 101;
  f.ui.updateSensorSnapshot(value, 1100);
  f.render(1100);
  CHECK(f.contains("1 m 外") && !f.contains("1 m 内有人"));
  f.ui.setSensorOptions(false, true);
  f.render(1200);
  CHECK(f.contains("感应亮屏已关闭") && !f.contains("可感应亮屏"));
}

void radarHealthAndFreshnessStatesRenderExplicitly() {
  Fixture f;
  f.environment();
  f.tap(390, 68, 700);
  auto value = reading(1000);
  value.radar.sample.moving = true;
  value.radar.sample.movingEnergy = 20;
  value.radar.sample.movingDistanceCm = 80;
  const std::pair<sensors::Health, const char*> states[] = {
      {sensors::Health::Starting, "雷达启动中"},
      {sensors::Health::Recovering, "雷达恢复中"},
      {sensors::Health::Offline, "雷达未连接"},
      {sensors::Health::Stale, "雷达数据过期"},
  };
  for (const auto& state : states) {
    value.radar.health = state.first;
    f.ui.updateSensorSnapshot(value, 1000);
    f.render(1000);
    CHECK(f.contains(state.second) && f.contains("有人状态未知"));
    CHECK(f.contains("无有效雷达，回退触摸唤醒") && !f.contains("雷达过期或离线"));
    CHECK(f.contains("-- cm / ENERGY --") && !f.contains("0.80 m"));
    CHECK(f.contains("本次最远报告 --"));
  }
  value.radar.health = sensors::Health::Healthy;
  value.radar.hasSample = false;
  f.ui.updateSensorSnapshot(value, 1000);
  f.render(1000);
  CHECK(f.contains("雷达数据过期") && f.contains("无有效雷达"));
  value.radar.hasSample = true;
  value.radar.sample.presenceKnown = false;
  f.ui.updateSensorSnapshot(value, 1000);
  f.render(1000);
  CHECK(f.contains("雷达数据过期") && f.contains("有人状态未知"));
  value.radar.sample.presenceKnown = true;
  f.ui.updateSensorSnapshot(value, 3001);  // Healthy but age 2001 ms is stale.
  f.render(3001);
  CHECK(f.contains("雷达数据过期") && !f.contains("0.80 m"));
  value.radar.sample.receivedAtMs = 3100;
  f.ui.updateSensorSnapshot(value, 3100);
  f.render(3100);
  CHECK(f.contains("房间有人") && f.contains("0.80 m"));
  CHECK(!f.contains("有人状态未知") && !f.contains("无有效雷达"));
}

void environmentTabsKeepGlobalNavigationAndReturn() {
  Fixture f;
  f.environment();
  f.tap(390, 68, 700);
  f.render(800);
  CHECK(f.contains("运动目标"));
  f.swipe(true, 1000);
  CHECK(f.ui.baseState() == UiState::MAC_CONTROL);
  f.swipe(false, 1300);
  f.render(1500);
  CHECK(f.contains("CO2提醒") && !f.contains("运动目标"));
  f.tap(240, 68, 1600);
  uint8_t requested = 255;
  CHECK(f.ui.takeAirHistoryRequest(requested) && requested == 0);
  CHECK(!f.ui.takeAirHistoryRequest(requested));
  f.tap(50, 30, 1900);
  CHECK(f.ui.baseState() == UiState::WALLPAPER);
}

void historyDailyLineHasRealPointsAndBreaks() {
  Fixture f;
  HistoryFixture history;
  history.point(0, 700);
  history.point(1, 800, 2450, 5120, true);
  history.point(2, 900, 2450, 5120, false);  // A missing minute inside a bin.
  history.point(4, 1100, 2450, 5120, true);  // A wholly missing previous bin.
  f.ui.setAirHistoryView(&history.view);
  f.environment();
  f.tap(240, 68, 700);
  f.render(800);
  CHECK(f.contains("2026-10-12 今日") && f.contains("5分钟均值"));
  CHECK(f.contains("4 分钟") && f.contains("00") && f.contains("24"));
  CHECK(f.chartPoints(0x9FE7) == 4 && f.chartLines(0x9FE7) == 1);
  for (const auto& position : f.gfx.textPositions) {
    CHECK(position.x >= 0 && position.x < 480 && position.y >= 0 && position.y < 320);
  }
  history = HistoryFixture{};
  history.view.buckets = history.buckets.data();
  history.point(140, 1200);
  f.render(900);
  CHECK(f.chartPoints(0x9FE7) == 1 && f.chartLines(0x9FE7) == 0);
}

void historyMetricSwitchDoesNotRequestIoOrChangeOptions() {
  Fixture f;
  HistoryFixture history;
  history.point(100, 1270, -150, 5120);
  history.point(101, 1400, 250, 5500, true);
  f.ui.setAirHistoryView(&history.view);
  f.environment();
  f.tap(240, 68, 700);
  uint8_t requested;
  CHECK(f.ui.takeAirHistoryRequest(requested));
  f.tap(240, 280, 1000);
  f.render(1100);
  CHECK(f.chartPoints(0xFD20) == 2 && f.chartLines(0xFD20) == 1);
  CHECK(!f.ui.takeAirHistoryRequest(requested));
  f.tap(390, 280, 1300);
  f.render(1400);
  CHECK(f.chartPoints(0x4D7F) == 2 && f.chartLines(0x4D7F) == 1);
  CHECK(!f.ui.takeAirHistoryRequest(requested));
  CHECK(f.ui.presenceWakeEnabled() && f.ui.co2ReminderEnabled());
  CHECK(!f.ui.takeSensorOptionsChange());
  const auto temperature = environment_chart::scale(history.view, 1);
  CHECK(temperature.low < -150 && temperature.high > 250);
  const auto humidity = environment_chart::scale(history.view, 2);
  CHECK(humidity.low >= 0 && humidity.high <= 10000);
}

void historyDateBoundariesHideWrongDayAndConsumeRequestsOnce() {
  Fixture f;
  HistoryFixture history;
  history.point(0, 1270);
  f.ui.setAirHistoryView(&history.view);
  f.environment();
  f.tap(240, 68, 700);
  uint8_t requested;
  CHECK(f.ui.takeAirHistoryRequest(requested));
  f.tap(420, 103, 1000);  // Today has no future day.
  CHECK(!f.ui.takeAirHistoryRequest(requested));
  for (uint8_t day = 1; day <= 6; ++day) {
    f.tap(50, 103, 1000 + day * 300U);
    CHECK(f.ui.takeAirHistoryRequest(requested) && requested == day);
    CHECK(!f.ui.takeAirHistoryRequest(requested));
  }
  f.render(3000);
  CHECK(f.contains("正在载入所选日期") && f.chartPoints(0x9FE7) == 0);
  f.tap(50, 103, 3100);
  CHECK(!f.ui.takeAirHistoryRequest(requested));
  history.view.daysAgo = 6;
  history.view.day = 6;
  f.render(3300);
  CHECK(f.contains("2026-10-06") && f.chartPoints(0x9FE7) == 1);
  f.tap(420, 103, 3400);
  CHECK(f.ui.takeAirHistoryRequest(requested) && requested == 5);
  f.render(3600);
  CHECK(f.chartPoints(0x9FE7) == 0);
}

void historyStatesEmptyDataAndMissingMemoryAreExplicit() {
  Fixture f;
  f.environment();
  f.tap(240, 68, 700);
  f.render(800);
  CHECK(f.contains("历史服务启动中"));
  HistoryFixture history;
  f.ui.setAirHistoryView(&history.view);
  f.render(900);
  CHECK(f.contains("当天尚无有效记录") && f.chartPoints(0x9FE7) == 0);
  const std::pair<sensors::HistoryState, const char*> states[] = {
      {sensors::HistoryState::Starting, "历史服务启动中"},
      {sensors::HistoryState::Loading, "正在载入所选日期"},
      {sensors::HistoryState::SdUnavailable, "SD 卡不可用"},
      {sensors::HistoryState::StorageError, "历史存储错误"},
      {sensors::HistoryState::MemoryUnavailable, "历史内存不足"},
      {sensors::HistoryState::WaitingForClock, "未校时"},
  };
  history.point(0, 1270);
  for (const auto& state : states) {
    history.view.state = state.first;
    f.render(1000);
    CHECK(f.contains(state.second) && f.chartPoints(0x9FE7) == 0);
  }
  history.view.state = sensors::HistoryState::Ready;
  history.view.clockValid = false;
  f.render(1100);
  CHECK(f.contains("未校时") && f.chartPoints(0x9FE7) == 0);
  history.view.clockValid = true;
  history.view.buckets = nullptr;
  f.render(1200);
  CHECK(f.contains("历史数据不可用") && f.chartPoints(0x9FE7) == 0);
}

void tabAndDateEdgesRequireCompletedTap() {
  Fixture f;
  f.environment();
  f.tap(169, 68, 700);  // Gap between tabs.
  uint8_t requested;
  CHECK(!f.ui.takeAirHistoryRequest(requested));
  f.touch(true, 170, 54, 1000);
  f.touch(true, 169, 54, 1020);
  f.touch(false, 169, 54, 1100);
  CHECK(!f.ui.takeAirHistoryRequest(requested));
  f.tap(170, 54, 1300);  // Inclusive top/left of history tab.
  CHECK(f.ui.takeAirHistoryRequest(requested) && requested == 0);
  f.tap(92, 103, 1600);  // Exclusive right edge of previous-day target.
  CHECK(!f.ui.takeAirHistoryRequest(requested));
  f.ui.setScreenDimmed(true);
  f.tap(50, 103, 1900);
  CHECK(!f.ui.takeAirHistoryRequest(requested));
  f.ui.setScreenDimmed(false);
  f.tap(50, 103, 2200);
  CHECK(f.ui.takeAirHistoryRequest(requested) && requested == 1);
}

void historyAndRadarRetainEventAndOtaPriority() {
  Fixture f;
  HistoryFixture history;
  history.point(0, 1270);
  f.ui.setAirHistoryView(&history.view);
  f.environment();
  f.tap(240, 68, 700);
  uint8_t requested;
  CHECK(f.ui.takeAirHistoryRequest(requested));
  CompanionEvent event;
  strlcpy(event.id, "history-overlay", sizeof(event.id));
  strlcpy(event.title, "曲线通知", sizeof(event.title));
  event.autoDismissMs = 12000;
  CHECK(f.ui.receiveEvent(event, 1000));
  f.tap(50, 103, 1100);
  CHECK(!f.ui.takeAirHistoryRequest(requested));
  f.render(1200);
  CHECK(f.contains("曲线通知") && f.chartPoints(0x9FE7) == 0);
  f.ui.setOtaProgress(true, 25, false);
  f.tap(390, 68, 1400);
  f.render(1500);
  CHECK(f.contains("正在更新固件") && !f.contains("运动目标"));
  f.ui.setOtaProgress(false, 0, false);
  f.ui.tick(13000);
  f.touch(false, 0, 0, 13100);
  f.render(13200);
  CHECK(f.chartPoints(0x9FE7) == 1);
  f.tap(390, 68, 13300);
  f.render(13400);
  CHECK(f.contains("运动目标"));
}
}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"existing state values are preserved", existingStateValuesDoNotShift},
      {"all pages render and navigate in both directions", allPagesNavigateAndRenderInBothDirections},
      {"home shortcuts retain Hub destination IDs", homeShortcutsKeepHubDestinations},
      {"control buttons retain Hub page IDs", controlButtonsKeepHubPageId},
      {"switches require completed taps and signal persistence once", switchesDispatchOnlyCompletedTaps},
      {"first gesture on dimmed environment only wakes", dimmedFirstGestureOnlyWakes},
      {"stale and recovering readings cannot look live", staleOrRecoveringValuesNeverLookLive},
      {"startup and high state are local and clear", startupAndHighStateStayClearWithoutBridge},
      {"event and OTA priority fence environment input", eventsAndOtaFenceEnvironmentInput},
      {"setup keeps priority and returns home", setupKeepsPriorityAndReturnWorks},
      {"navigation and freshness survive clock wrap", navigationAndFreshnessSurviveWrap},
      {"radar shows qualified distances and boot maximum", radarShowsOnlyQualifiedDistancesAndBootMaximum},
      {"radar one-meter boundary and disabled wake are explicit", radarNearBoundaryAndDisabledWakeAreExplicit},
      {"radar health and freshness states render explicitly", radarHealthAndFreshnessStatesRenderExplicitly},
      {"environment tabs keep global navigation and return", environmentTabsKeepGlobalNavigationAndReturn},
      {"daily lines draw real single points and preserve gaps", historyDailyLineHasRealPointsAndBreaks},
      {"metric switching is local and preserves options", historyMetricSwitchDoesNotRequestIoOrChangeOptions},
      {"date requests stay within seven days and hide old-day plots", historyDateBoundariesHideWrongDayAndConsumeRequestsOnce},
      {"history states and empty data are explicit", historyStatesEmptyDataAndMissingMemoryAreExplicit},
      {"tab and date boundaries require completed taps", tabAndDateEdgesRequireCompletedTap},
      {"history and radar retain event and OTA priority", historyAndRadarRetainEventAndOtaPriority},
  };
  size_t passed = 0;
  for (const auto& item : tests) {
    try {
      item.second();
      ++passed;
      std::cout << "PASS " << item.first << '\n';
    } catch (const std::exception& error) {
      std::cerr << "FAIL " << item.first << ": " << error.what() << '\n';
    }
  }
  std::cout << passed << '/' << tests.size() << " environment UI tests passed\n";
  return passed == tests.size() ? 0 : 1;
}
