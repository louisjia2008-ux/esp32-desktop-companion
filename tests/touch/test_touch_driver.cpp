#include "touch_driver.h"
#include <Wire.h>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
  std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #condition); \
} while (false)

namespace {
void packet(uint8_t count, uint16_t x, uint16_t y, uint8_t event = 0) {
  Wire.payload = {0, count, static_cast<uint8_t>((event << 6) | (x >> 8)),
                  static_cast<uint8_t>(x), static_cast<uint8_t>(y >> 8),
                  static_cast<uint8_t>(y), 0, 0};
}
void realCommandAndCoordinates() {
  Axs15231bTouch device;
  CHECK(device.begin());
  packet(1, 319, 479, 2);
  TouchPoint point;
  CHECK(device.read(point) == TouchReadResult::DOWN);
  CHECK(point.x == 319 && point.y == 479);
  CHECK(Wire.command == std::vector<uint8_t>(
      {0xB5, 0xAB, 0xA5, 0x5A, 0, 0, 0, 8, 0, 0, 0}));
  CHECK(device.diagnostics().event == 2);
  CHECK(device.diagnostics().error == TouchReadError::NONE);
}
void releasedPacketIgnoresUnusedCoordinates() {
  Axs15231bTouch device;
  packet(0, 4095, 4095, 3);
  TouchPoint point{7, 9};
  CHECK(device.read(point) == TouchReadResult::UP);
  CHECK(point.x == 7 && point.y == 9);
  CHECK(device.diagnostics().error == TouchReadError::NONE);
}
void commandFailureNeverMeansRelease() {
  Axs15231bTouch device;
  Wire.endStatus = 2;
  TouchPoint point{7, 9};
  CHECK(device.read(point) == TouchReadResult::ERROR);
  CHECK(device.diagnostics().error == TouchReadError::COMMAND);
  Wire.endStatus = 0;
  CHECK(device.read(point) == TouchReadResult::UP);
  CHECK(device.diagnostics().error == TouchReadError::NONE);
}
void shortResponseNeverMeansRelease() {
  Axs15231bTouch device;
  TouchPoint point;
  Wire.receiveLength = 7;
  CHECK(device.read(point) == TouchReadResult::ERROR);
  CHECK(device.diagnostics().error == TouchReadError::RESPONSE);
  CHECK(Wire.available() == 0);
  Wire.receiveLength = 8;
  Wire.payload.resize(7);
  CHECK(device.read(point) == TouchReadResult::ERROR);
  CHECK(device.diagnostics().error == TouchReadError::RESPONSE);
}
void invalidCountAndCoordinatesAreClassified() {
  Axs15231bTouch device;
  TouchPoint point{7, 9};
  packet(2, 100, 200);
  CHECK(device.read(point) == TouchReadResult::ERROR);
  CHECK(device.diagnostics().error == TouchReadError::POINT_COUNT);
  CHECK(device.diagnostics().pointCount == 2);
  packet(1, 4095, 4095, 2);
  CHECK(device.read(point) == TouchReadResult::ERROR);
  CHECK(device.diagnostics().error == TouchReadError::COORDINATES);
  CHECK(device.diagnostics().rawX == 4095);
  CHECK(device.diagnostics().event == 2);
  CHECK(point.x == 7 && point.y == 9);
}
// Packets captured from the real panel (TOUCH_TRACE) after a finger lifted.
// The controller never returned count=0 again; these must read as release.
void observedIdleFillerIsRelease() {
  Axs15231bTouch device;
  TouchPoint point{7, 9};
  const struct { uint8_t count, event; uint16_t x, y; } observed[] = {
      {255, 3, 4095, 3958}, {255, 3, 4095, 4094}, {151, 0, 225, 1800},
      {177, 2, 433, 433}};
  for (const auto& p : observed) {
    packet(p.count, p.x, p.y, p.event);
    CHECK(device.read(point) == TouchReadResult::UP);
    CHECK(device.diagnostics().error == TouchReadError::NONE);
    CHECK(device.diagnostics().idleReport);
    CHECK(device.diagnostics().pointCount == p.count);
  }
  CHECK(point.x == 7 && point.y == 9);
  packet(1, 151, 225, 2);
  CHECK(device.read(point) == TouchReadResult::DOWN);
  CHECK(!device.diagnostics().idleReport);
}
void liftEventIsRelease() {
  Axs15231bTouch device;
  TouchPoint point{7, 9};
  packet(1, 151, 225, 1);
  CHECK(device.read(point) == TouchReadResult::UP);
  CHECK(device.diagnostics().idleReport);
  CHECK(point.x == 7 && point.y == 9);
  packet(1, 4095, 4095, 1);
  CHECK(device.read(point) == TouchReadResult::UP);
  packet(2, 100, 200, 1);
  CHECK(device.read(point) == TouchReadResult::ERROR);
  CHECK(device.diagnostics().error == TouchReadError::POINT_COUNT);
}
}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"actual I2C command and valid native coordinates", realCommandAndCoordinates},
      {"release ignores unused coordinates", releasedPacketIgnoresUnusedCoordinates},
      {"command error is not release and diagnostics reset", commandFailureNeverMeansRelease},
      {"short or missing response is not release", shortResponseNeverMeansRelease},
      {"invalid count and coordinates retain diagnostic evidence", invalidCountAndCoordinatesAreClassified},
      {"observed idle filler packets are release", observedIdleFillerIsRelease},
      {"lift event is release", liftEventIsRelease},
  };
  size_t passed = 0;
  for (const auto& test : tests) {
    Wire = {};
    try {
      test.second();
      ++passed;
      std::cout << "PASS " << test.first << '\n';
    } catch (const std::exception& error) {
      std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
    }
  }
  std::cout << passed << '/' << tests.size() << " driver tests passed\n";
  return passed == tests.size() ? 0 : 1;
}
