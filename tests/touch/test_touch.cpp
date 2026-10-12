#include "touch/touch_sampler.h"
#include "touch/touch_transform.h"
#include "ui/ui_manager.h"

#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The real UI/sampler/coordinate transform are compiled. Only graphics,
// video, RTOS scheduling, and the physical I2C samples are substituted.
namespace uifont { const uint8_t* cjkFont() { return nullptr; } }
void Wallpaper::begin(uint32_t) {}
void Wallpaper::render(Arduino_Canvas&, uint32_t) {}
bool Wallpaper::selectVideo(const char*) { return true; }
bool Wallpaper::decodeVideoFrame(const char*, const uint8_t*, size_t, uint32_t) {
  return true;
}
bool Wallpaper::videoFresh(uint32_t) const { return false; }

namespace {
std::deque<touch::Sample> devicePackets;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
  std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #condition); \
} while (false)

void reset() {
  fake_arduino::resetPinsAndClock();
  Serial.clear();
  fake_queue::reset();
  fake_task::reset();
  devicePackets.clear();
}
UiActionEvent deliver(UiManager& ui, bool down, uint32_t sampledAt,
                      uint32_t processedAt) {
  fake_arduino::setMillis(processedAt);
  UiActionEvent event;
  // This pixel is above the home dock, and inside the modal confirm button.
  ui.handleTouch(down, TouchPoint{200, 214}, sampledAt, event);
  return event;
}
UiActionEvent deliver(UiManager& ui, bool down, uint32_t at) {
  return deliver(ui, down, at, at);
}
void prime(UiManager& ui, uint32_t at = 0) {
  fake_arduino::setMillis(at);
  ui.begin(0);
  deliver(ui, false, at);
}
UiActionEvent deliverAt(UiManager& ui, bool down, uint16_t x, uint16_t y, uint32_t at) {
  fake_arduino::setMillis(at);
  UiActionEvent event;
  ui.handleTouch(down, TouchPoint{x, y}, at, event);
  return event;
}
// Drags horizontally from fromX to toX over 100 ms and lifts; returns the
// time at which the release is recognised.
uint32_t swipe(UiManager& ui, uint16_t fromX, uint16_t toX, uint32_t at,
               uint16_t fromY = 160, uint16_t toY = 160) {
  for (uint32_t step = 0; step <= 10; ++step) {
    deliverAt(ui, true, static_cast<uint16_t>(fromX + (int(toX) - int(fromX)) * int(step) / 10),
              static_cast<uint16_t>(fromY + (int(toY) - int(fromY)) * int(step) / 10),
              at + step * 10);
  }
  deliverAt(ui, false, 0, 0, at + 200);
  return at + 200;
}
void requireAction(UiManager& ui, uint32_t at) {
  fake_arduino::setMillis(at);
  UiActionOption option;
  strlcpy(option.id, "confirm", sizeof(option.id));
  strlcpy(option.label, "Confirm", sizeof(option.label));
  CHECK(ui.showActionRequired("test-request", "Title", "Message", &option, 1));
  ui.setConnectionIndicator(ConnectionIndicator::BRIDGE_CONNECTED);
}
void packet(TouchReadResult result, uint32_t at) {
  touch::Sample sample;
  sample.result = result;
  sample.sampledAtMs = at;
  sample.point = {100, 200};
  devicePackets.push_back(sample);
}
void replay(touch::Sampler& sampler, UiManager& ui, uint32_t processedAt) {
  touch::Sample sample;
  while (sampler.pop(sample)) {
    if (sample.gap) ui.resetTouchAfterGap(sample.sampledAtMs);
    if (sample.result == TouchReadResult::ERROR) continue;
    deliver(ui, sample.result == TouchReadResult::DOWN,
            sample.sampledAtMs, processedAt);
  }
}

void consecutiveShortTaps() {
  UiManager ui;
  prime(ui);
  const bool initial = ui.clockVisible();
  for (uint32_t tap = 0; tap < 6; ++tap) {
    deliver(ui, true, tap * 200 + 100);
    deliver(ui, false, tap * 200 + 200);
    CHECK(ui.clockVisible() == (tap % 2 == 0 ? !initial : initial));
    CHECK(ui.state() == UiState::WALLPAPER);
  }
}
void delayedBatchRetainsEveryTap() {
  UiManager ui;
  prime(ui);
  const bool initial = ui.clockVisible();
  // All these samples are consumed after a synchronous render/decode stall.
  for (uint32_t tap = 0; tap < 4; ++tap) {
    deliver(ui, true, tap * 200 + 100, 1000);
    deliver(ui, false, tap * 200 + 200, 1000);
    CHECK(ui.clockVisible() == (tap % 2 == 0 ? !initial : initial));
  }
}
void holdNeedsReleaseAndNeverRepeats() {
  UiManager ui;
  prime(ui);
  const bool initial = ui.clockVisible();
  for (uint32_t at = 100; at <= 900; at += 10) deliver(ui, true, at);
  deliver(ui, false, 1000);
  CHECK(ui.clockVisible() == initial);  // a long press is not a tap
  CHECK(ui.state() == UiState::WALLPAPER);
  deliver(ui, true, 1100);
  deliver(ui, false, 1200);
  CHECK(ui.clockVisible() != initial);
}
void timeoutDoesNotTurnHeldFingerIntoNewTap() {
  UiManager ui;
  prime(ui);
  const bool initial = ui.clockVisible();
  const uint32_t released = swipe(ui, 400, 200, 100);
  CHECK(ui.state() == UiState::HOST_STATUS);
  // A finger lands just before the page times out and stays down across it.
  for (uint32_t at = released + 14900; at < released + 15000; at += 10) deliver(ui, true, at);
  fake_arduino::setMillis(released + 15000);
  ui.tick(released + 15000);
  CHECK(ui.state() == UiState::WALLPAPER);
  for (uint32_t at = released + 15000; at <= released + 15200; at += 10) deliver(ui, true, at);
  deliver(ui, false, released + 15300);
  CHECK(ui.clockVisible() == initial);
  deliver(ui, true, released + 15400);
  deliver(ui, false, released + 15500);
  CHECK(ui.clockVisible() != initial);
}
void oldInputCannotUnlockNewAction() {
  UiManager ui;
  prime(ui);
  requireAction(ui, 500);
  deliver(ui, false, 200, 600);  // Old UP may not clear the new barrier.
  CHECK(!deliver(ui, true, 300, 600).valid);
  CHECK(!deliver(ui, true, 360, 600).valid);
  CHECK(!deliver(ui, true, 550, 600).valid);
  CHECK(!deliver(ui, true, 610, 700).valid);
  deliver(ui, false, 690, 700);
  CHECK(!deliver(ui, true, 700).valid);
  CHECK(deliver(ui, true, 760).valid);
}
void heldFingerCannotAcceptNewAction() {
  UiManager ui;
  prime(ui);
  deliver(ui, true, 100);
  requireAction(ui, 110);
  CHECK(!deliver(ui, true, 150).valid);
  CHECK(!deliver(ui, true, 220).valid);
  deliver(ui, false, 300);
  deliver(ui, true, 310);
  const auto event = deliver(ui, true, 370);
  CHECK(event.valid);
  CHECK(std::string(event.requestId) == "test-request");
  CHECK(std::string(event.actionId) == "confirm");
}
void oldGapCannotMoveNewActionCutoffBackwards() {
  UiManager ui;
  prime(ui);
  requireAction(ui, 500);
  ui.resetTouchAfterGap(400);
  deliver(ui, false, 480, 600);
  CHECK(!deliver(ui, true, 490, 600).valid);
  CHECK(!deliver(ui, true, 550, 600).valid);
  deliver(ui, false, 630);
  deliver(ui, true, 650);
  CHECK(deliver(ui, true, 710).valid);
}
void dispatchedActionSurvivesInputGap() {
  UiManager ui;
  prime(ui);
  requireAction(ui, 100);
  deliver(ui, false, 100);
  deliver(ui, true, 110);
  CHECK(deliver(ui, true, 170).valid);
  ui.resetTouchAfterGap(200);
  deliver(ui, false, 280);
  CHECK(!deliver(ui, true, 300).valid);
  CHECK(!deliver(ui, true, 360).valid);
}
void gapWaitsForDebouncedRelease() {
  UiManager ui;
  prime(ui);
  const bool initial = ui.clockVisible();
  ui.resetTouchAfterGap(100);
  deliver(ui, true, 120);
  deliver(ui, true, 170);
  deliver(ui, false, 249);  // 79 ms: not yet a release
  deliver(ui, true, 250);
  deliver(ui, false, 330);  // released: only unlocks input
  CHECK(ui.clockVisible() == initial);
  deliver(ui, true, 340);
  deliver(ui, true, 400);
  deliver(ui, false, 480);
  CHECK(ui.clockVisible() != initial);
}
void actionCancelsWhenFingerMovesOffButton() {
  UiManager ui;
  prime(ui);
  requireAction(ui, 100);
  deliver(ui, false, 100);
  deliver(ui, true, 110);
  fake_arduino::setMillis(150);
  UiActionEvent event;
  ui.handleTouch(true, TouchPoint{0, 0}, 150, event);
  CHECK(!event.valid);
  CHECK(!deliver(ui, true, 180).valid);
}
void toggleAndExpirySurviveClockWrap() {
  const uint32_t start = std::numeric_limits<uint32_t>::max() - 200;
  UiManager ui;
  prime(ui, start);
  const bool initial = ui.clockVisible();
  deliver(ui, true, start + 10);
  deliver(ui, false, start + 110);
  CHECK(ui.clockVisible() != initial);
  deliver(ui, true, start + 120);
  deliver(ui, false, start + 220);
  CHECK(ui.clockVisible() == initial);
  const uint32_t released = swipe(ui, 400, 200, start + 230);  // wraps past zero
  CHECK(ui.state() == UiState::HOST_STATUS);
  fake_arduino::setMillis(released + 15000);
  ui.tick(released + 15000);
  CHECK(ui.state() == UiState::WALLPAPER);
}
void samplerCapturesShortTapDuringRenderStall() {
  UiManager ui;
  prime(ui);
  Axs15231bTouch device;
  touch::Sampler sampler;
  CHECK(sampler.begin(device));
  CHECK(sampler.begin(device));
  CHECK(fake_task::creationCalls == 1);
  CHECK(fake_task::task.core == 0);
  CHECK(fake_task::task.priority == 2);
  for (uint32_t at = 0; at < 200; at += 10) {
    packet(at == 10 || at == 20 ? TouchReadResult::DOWN : TouchReadResult::UP,
           at);
  }
  const bool initial = ui.clockVisible();
  fake_task::run(20);  // UI has not been serviced for the entire 200 ms.
  CHECK(ui.clockVisible() == initial);
  replay(sampler, ui, 200);
  CHECK(ui.clockVisible() != initial);
  const auto stats = sampler.statistics();
  CHECK(stats.samples == 20);
  CHECK(stats.downSamples == 2);
  CHECK(stats.queueOverflows == 0);
  CHECK(stats.maximumGapMs == 10);
}
void samplerOverflowCarriesExplicitGap() {
  Axs15231bTouch device;
  touch::Sampler sampler;
  CHECK(sampler.begin(device));
  for (uint32_t index = 0; index <= touch::Sampler::kQueueCapacity; ++index) {
    packet(index == 1 ? TouchReadResult::DOWN : TouchReadResult::UP, index * 10);
  }
  fake_task::run(touch::Sampler::kQueueCapacity + 1);
  touch::Sample sample;
  CHECK(sampler.pop(sample));
  CHECK(sample.gap);
  CHECK(sample.sampledAtMs == touch::Sampler::kQueueCapacity * 10);
  CHECK(!sampler.pop(sample));
  CHECK(sampler.statistics().queueOverflows == 1);
}
void samplerRetainsGesturesAcrossNetworkWait() {
  UiManager ui;
  prime(ui);
  Axs15231bTouch device;
  touch::Sampler sampler;
  CHECK(sampler.begin(device));
  for (uint32_t at = 0; at < 1600; at += 10) {
    const bool down = at >= 100 && at % 300 >= 100 && at % 300 < 140;
    packet(down ? TouchReadResult::DOWN : TouchReadResult::UP, at);
  }
  fake_task::run(160);
  CHECK(sampler.statistics().queueOverflows == 0);
  const bool initial = ui.clockVisible();
  replay(sampler, ui, 1600);
  uint32_t taps = 0;
  for (const auto& line : Serial.lines) {
    if (line.rfind("CLOCK_VISIBLE=", 0) == 0) ++taps;
  }
  CHECK(taps == 5);  // Five complete taps retained.
  CHECK(ui.clockVisible() != initial);
}
void samplerErrorsAreNotReleaseEvents() {
  UiManager ui;
  prime(ui);
  deliver(ui, true, 10);
  requireAction(ui, 20);
  Axs15231bTouch device;
  touch::Sampler sampler;
  CHECK(sampler.begin(device));
  packet(TouchReadResult::ERROR, 100);
  packet(TouchReadResult::DOWN, 110);
  packet(TouchReadResult::DOWN, 170);
  fake_task::run(3);
  replay(sampler, ui, 200);
  CHECK(!deliver(ui, true, 220).valid);
  CHECK(sampler.statistics().readErrors == 1);
  deliver(ui, false, 300);
  deliver(ui, true, 310);
  CHECK(deliver(ui, true, 370).valid);
}
void samplerGapMeasurementSurvivesWrap() {
  Axs15231bTouch device;
  touch::Sampler sampler;
  CHECK(sampler.begin(device));
  const auto maximum = std::numeric_limits<uint32_t>::max();
  packet(TouchReadResult::UP, maximum - 15);
  packet(TouchReadResult::UP, maximum - 5);
  packet(TouchReadResult::UP, 5);
  packet(TouchReadResult::UP, 45);
  fake_task::run(4);
  CHECK(sampler.statistics().maximumGapMs == 40);
}
void samplerInitializationFailureIsSafe() {
  Axs15231bTouch device;
  touch::Sampler noQueue;
  fake_queue::creationSucceeds = false;
  CHECK(!noQueue.begin(device));
  CHECK(fake_task::creationCalls == 0);
  touch::Sample sample;
  CHECK(!noQueue.pop(sample));
  fake_queue::creationSucceeds = true;
  fake_task::creationSucceeds = false;
  touch::Sampler noTask;
  CHECK(!noTask.begin(device));
  CHECK(fake_queue::deleted == 1);
  CHECK(!noTask.pop(sample));
}
void coordinateMappingIsUnchanged() {
  TouchPoint display;
  CHECK(touch::toDisplayCoordinates(TouchPoint{0, 0}, display));
  CHECK(display.x == 0 && display.y == 319);
  CHECK(touch::toDisplayCoordinates(TouchPoint{319, 479}, display));
  CHECK(display.x == 479 && display.y == 0);
}
}  // namespace

namespace {
size_t countLines(const char* text) {
  size_t count = 0;
  for (const auto& line : Serial.lines) if (line == text) ++count;
  return count;
}
// Hardware regression: the dashboard opened, then the controller produced
// only unusable reports, so the release barrier never unlocked.
void staleContactCannotLockDashboard() {
  UiManager ui;
  prime(ui);
  const bool initial = ui.clockVisible();
  deliver(ui, true, 10);
  deliver(ui, true, 70);  // then the controller only returns errors
  CHECK(ui.physicalTouchDown());
  CHECK(!ui.releaseStaleContact(70 + 2990));
  CHECK(ui.releaseStaleContact(70 + 3000));
  CHECK(!ui.touchReleaseBarrierActive());
  CHECK(!ui.physicalTouchDown());
  CHECK(!ui.releaseStaleContact(70 + 3010));
  CHECK(ui.clockVisible() == initial);  // the half-seen contact is no tap
  deliver(ui, true, 3200);
  deliver(ui, false, 3300);
  CHECK(ui.clockVisible() != initial);
}
void staleContactNeverChoosesAction() {
  UiManager ui;
  prime(ui);
  requireAction(ui, 10);
  deliver(ui, false, 20);
  deliver(ui, false, 120);
  const UiManager::ButtonRect r = ui.eventButtonRect(0);
  const TouchPoint onButton{static_cast<uint16_t>(r.x + r.width / 2),
                            static_cast<uint16_t>(r.y + r.height / 2)};
  UiActionEvent event;
  fake_arduino::setMillis(200);
  // One real report shorter than the press debounce, then only read errors.
  // Without the stale release, the eventual UP would complete this as a tap.
  ui.handleTouch(true, onButton, 200, event);
  CHECK(!event.valid);
  CHECK(ui.releaseStaleContact(200 + 3000));
  ui.handleTouch(false, onButton, 3300, event);
  ui.handleTouch(false, onButton, 3400, event);
  CHECK(!event.valid);
  CHECK(ui.state() == UiState::ACTION_REQUIRED);
}
}  // namespace

namespace {
void swipesMoveBetweenPages() {
  UiManager ui;prime(ui);const bool initial=ui.clockVisible();
  swipe(ui,200,400,100);CHECK(ui.state()==UiState::WALLPAPER&&ui.clockVisible()==initial);
  uint32_t at=400;
  const UiState pages[]={UiState::HOST_STATUS,UiState::MAC_CONTROL,UiState::INFO_CARDS,UiState::TASKS,UiState::USAGE};
  for(auto page:pages){swipe(ui,400,200,at,310,310);CHECK(ui.state()==page);at+=300;}
  swipe(ui,400,200,at,310,310);CHECK(ui.state()==UiState::USAGE);at+=300;
  for(int i=3;i>=0;--i){swipe(ui,200,400,at,310,310);CHECK(ui.state()==pages[i]);at+=300;}
  swipe(ui,200,400,at,310,310);CHECK(ui.state()==UiState::WALLPAPER&&ui.clockVisible()==initial);
}
void diagonalDragAndTapOnPage() {
  UiManager ui;
  prime(ui);
  const bool initial = ui.clockVisible();
  swipe(ui, 400, 300, 100, 60, 260);  // mostly vertical: ignored
  CHECK(ui.state() == UiState::WALLPAPER && ui.clockVisible() == initial);
  swipe(ui, 400, 340, 400);  // too short to be a swipe, too long for a tap
  CHECK(ui.state() == UiState::WALLPAPER && ui.clockVisible() == initial);
  swipe(ui, 400, 200, 700);
  CHECK(ui.state() == UiState::HOST_STATUS);
  deliverAt(ui, true, 240, 160, 1000);
  deliverAt(ui, false, 0, 0, 1100);
  CHECK(ui.state() == UiState::WALLPAPER);   // tap on a page returns home
  CHECK(ui.clockVisible() == initial);       // without toggling the clock
}
void dimmedScreenFirstGestureOnlyWakes() {
  UiManager ui;
  prime(ui);
  const bool initial = ui.clockVisible();
  ui.setScreenDimmed(true);
  deliver(ui, true, 100);
  CHECK(ui.lastInteractionMs() == 100);  // brightness logic sees the touch
  deliver(ui, false, 200);
  CHECK(ui.clockVisible() == initial);
  ui.setScreenDimmed(false);
  deliver(ui, true, 300);
  deliver(ui, false, 400);
  CHECK(ui.clockVisible() != initial);
  ui.setScreenDimmed(true);
  swipe(ui, 400, 200, 500);
  CHECK(ui.state() == UiState::WALLPAPER);
}
}  // namespace

TouchReadResult Axs15231bTouch::read(TouchPoint& point) {
  if (devicePackets.empty()) throw std::runtime_error("I2C fixture exhausted");
  const auto sample = devicePackets.front();
  devicePackets.pop_front();
  point = sample.point;
  fake_arduino::setMillis(sample.sampledAtMs);
  return sample.result;
}

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"every consecutive short tap toggles the clock", consecutiveShortTaps},
      {"swipes move between pages", swipesMoveBetweenPages},
      {"diagonal drags are ignored and a tap on a page returns home", diagonalDragAndTapOnPage},
      {"the first gesture on a dimmed screen only wakes it", dimmedScreenFirstGestureOnlyWakes},
      {"delayed sample batches retain every tap", delayedBatchRetainsEveryTap},
      {"a long press is not a tap", holdNeedsReleaseAndNeverRepeats},
      {"a finger held across a page timeout is not a tap", timeoutDoesNotTurnHeldFingerIntoNewTap},
      {"old buffered input cannot unlock new action", oldInputCannotUnlockNewAction},
      {"held finger cannot accept new action", heldFingerCannotAcceptNewAction},
      {"old gap cannot unlock newer action", oldGapCannotMoveNewActionCutoffBackwards},
      {"input gap cannot duplicate dispatched action", dispatchedActionSurvivesInputGap},
      {"input gap waits for debounced release", gapWaitsForDebouncedRelease},
      {"moving off action button cancels gesture", actionCancelsWhenFingerMovesOffButton},
      {"taps, swipes and page expiry survive clock wrap", toggleAndExpirySurviveClockWrap},
      {"sampler captures short tap during UI stall", samplerCapturesShortTapDuringRenderStall},
      {"sampler retains gestures across network wait", samplerRetainsGesturesAcrossNetworkWait},
      {"queue overflow has an explicit resync marker", samplerOverflowCarriesExplicitGap},
      {"I2C errors are not release events", samplerErrorsAreNotReleaseEvents},
      {"sample gap measurement survives wrap", samplerGapMeasurementSurvivesWrap},
      {"sampler initialization failures are safe", samplerInitializationFailureIsSafe},
      {"coordinate mapping is unchanged", coordinateMappingIsUnchanged},
      {"stale contact cannot lock the dashboard", staleContactCannotLockDashboard},
      {"stale contact release never chooses an action", staleContactNeverChoosesAction},
  };
  size_t passed = 0;
  for (const auto& test : tests) {
    reset();
    try {
      test.second();
      ++passed;
      std::cout << "PASS " << test.first << '\n';
    } catch (const std::exception& error) {
      std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
    }
  }
  std::cout << passed << '/' << tests.size() << " tests passed\n";
  return passed == tests.size() ? 0 : 1;
}
