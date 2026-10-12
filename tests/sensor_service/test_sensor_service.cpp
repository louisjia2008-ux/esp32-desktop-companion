// OFFLINE worker/driver fixtures, never physical measurement evidence.
#include "Arduino.h"
#include "HardwareSerial.h"
#include "sensors/sensor_service.h"

#include <iostream>
#include <limits>

namespace mock {
uint32_t now = 0, scenarioStart = 0, runStart = 0, runDuration = 0;
int criticalDepth = 0;
bool running = false;
std::string failTask;
std::vector<Task> tasks;
std::function<void()> onAdvance;
std::deque<uint8_t> uartBytes;
int8_t uartRx = -1, uartTx = -1;
uint32_t uartBeginCalls = 0, uartBaud = 0, uartRxBuffer = 0;
bool uartValid = true;
std::function<void(int)> uartError;
bool busInitialized = false, wireBeginSucceeds = true;
uint32_t wireBeginCalls = 0, wireClock = 0;
uint16_t wireTimeout = 0;
uint8_t wireProbeResult = 0;
int wireSda = -1, wireScl = -1;
std::vector<uint32_t> probeTimes;
std::vector<std::string> commands;
std::vector<uint32_t> stopTimes;
uint32_t startAt = 0, lastEmissionAt = 0, measurements = 0;
bool neverReady = false;
uint16_t variant = 0x1000, fixtureCo2 = 1600;
std::function<int16_t()> readyError;
std::function<int16_t()> measurementError;

void advance(uint32_t milliseconds) {
  hardwareCall();
  now += milliseconds;
  if (onAdvance) onAdvance();
  if (running && uint32_t(now - runStart) >= runDuration) throw Stop();
}

void run(const char* name, uint32_t duration) {
  for (const Task& task : tasks) {
    if (task.name != name) continue;
    runStart = now;
    runDuration = duration;
    running = true;
    try { task.function(nullptr); } catch (const Stop&) {}
    running = false;
    if (criticalDepth != 0) throw std::runtime_error("snapshot lock leaked");
    return;
  }
  throw std::runtime_error(std::string("missing task: ") + name);
}
}  // namespace mock

ForbiddenUsb Serial;
HardwareSerial Serial1;

namespace {
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
  std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #condition); \
} while (false)

using sensors::Health;
const uint8_t kOfflineRadarFrame[] = {
  0xF4, 0xF3, 0xF2, 0xF1, 0x0D, 0x00, 0x02, 0xAA,
  0x02, 0x51, 0x00, 0x00, 0x00, 0x00, 0x3B, 0x00,
  0x00, 0x55, 0x00, 0xF8, 0xF7, 0xF6, 0xF5,
};

void addRadarFrame() {
  for (uint8_t byte : kOfflineRadarFrame) mock::uartBytes.push_back(byte);
}

void radarContinuously() {
  mock::onAdvance = [] { if (mock::now % 100 == 0) addRadarFrame(); };
}

void verifyCommandAllowlist() {
  for (const std::string& command : mock::commands) {
    CHECK(command == "stop" || command == "serial" || command == "variant" ||
          command == "start");
  }
}

void happyPath() {
  sensors::beginSensors();
  sensors::beginSensors();
  CHECK(mock::tasks.size() == 2);
  CHECK(mock::tasks[0].core == 0 && mock::tasks[0].stack >= 4096);
  CHECK(mock::tasks[1].core == 1 && mock::tasks[1].stack >= 6144);
  mock::run("scd41-service", 16000);
  auto value = sensors::readSensorSnapshot(mock::now);
  CHECK(value.air.health == Health::Healthy && value.air.validSamples == 3);
  CHECK(value.air.errors == 0 && value.air.recoveryAttempts == 0);
  CHECK(mock::wireBeginCalls == 1 && mock::wireSda == 17 && mock::wireScl == 18);
  CHECK(mock::wireClock == 100000 && mock::wireTimeout == 50);
  CHECK(mock::commands == std::vector<std::string>({"stop", "serial", "variant", "start"}));
  radarContinuously();
  mock::run("radar-service", 2500);
  value = sensors::readSensorSnapshot(mock::now);
  CHECK(value.radar.health == Health::Healthy && value.radar.counters.validFrames >= 20);
  CHECK(value.radar.counters.invalidFrames == 0);
  CHECK(mock::uartRx == 44 && mock::uartTx == -1 && mock::uartBaud == 256000);
  CHECK(mock::uartRxBuffer == 4096);
  CHECK(value.radar.workerStackFree == 2048 && value.air.workerStackFree == 2048);
  mock::uartError(UART_FIFO_OVF_ERROR);
  mock::uartError(UART_PARITY_ERROR);
  value = sensors::readSensorSnapshot(mock::now);
  CHECK(value.radar.uartErrors == 2 && value.radar.uartFifoOverflow == 1 &&
        value.radar.uartParityErrors == 1);
  CHECK(value.radar.lastError == UART_PARITY_ERROR);
}

void allocationIndependence() {
  mock::failTask = "radar-service";
  sensors::beginSensors();
  CHECK(mock::tasks.size() == 1 && mock::tasks[0].name == "scd41-service");
  mock::run("scd41-service", 6000);
  const auto value = sensors::readSensorSnapshot(mock::now);
  CHECK(value.radar.health == Health::Offline && value.radar.initializationFailures == 1);
  CHECK(value.air.health == Health::Healthy && value.air.validSamples == 1);
}

void uartOwnershipIndependence() {
  mock::uartRx = 7;
  sensors::beginSensors();
  mock::run("radar-service", 100);
  auto value = sensors::readSensorSnapshot(mock::now);
  CHECK(value.radar.health == Health::Offline && value.radar.initializationFailures == 1);
  CHECK(value.radar.lastError == int32_t(sensors::ServiceError::ResourceBusy));
  CHECK(mock::uartBeginCalls == 0 && mock::uartRx == 7);
  mock::run("scd41-service", 6000);
  value = sensors::readSensorSnapshot(mock::now);
  CHECK(value.air.health == Health::Healthy);
}

void airOwnershipIndependence() {
  mock::busInitialized = true;
  sensors::beginSensors();
  mock::run("scd41-service", 31000);
  auto value = sensors::readSensorSnapshot(mock::now);
  CHECK(value.air.health == Health::Recovering && value.air.initializationFailures == 2);
  CHECK(value.air.recoveryAttempts == 1 && mock::wireBeginCalls == 0);
  CHECK(value.air.lastError == int32_t(sensors::ServiceError::ResourceBusy));
  CHECK(mock::commands.empty());
  radarContinuously();
  mock::run("radar-service", 2500);
  value = sensors::readSensorSnapshot(mock::now);
  CHECK(value.radar.health == Health::Healthy && value.radar.counters.validFrames >= 20);
}

void airProbeRecovery() {
  mock::wireProbeResult = 2;
  mock::onAdvance = [] { if (mock::elapsed() >= 30000) mock::wireProbeResult = 0; };
  sensors::beginSensors();
  mock::run("scd41-service", 36000);
  const auto value = sensors::readSensorSnapshot(mock::now);
  CHECK(value.air.health == Health::Healthy && value.air.validSamples == 1);
  CHECK(value.air.errors == 1 && value.air.nackErrors == 1);
  CHECK(value.air.initializationFailures == 1 && value.air.recoveryAttempts == 1);
  CHECK(mock::probeTimes.size() == 2 && mock::probeTimes[1] - mock::probeTimes[0] == 30000);
  CHECK(mock::wireBeginCalls == 1);
}

void airCommunicationRecovery(bool wrap) {
  if (wrap) mock::now = mock::scenarioStart = UINT32_MAX - 8000;
  bool sawRecovering = false;
  mock::readyError = []() -> int16_t {
    return mock::elapsed() >= 6000 && mock::elapsed() <= 7000 ? 0x020B : 0;
  };
  mock::onAdvance = [&] {
    if (mock::elapsed() >= 7500 && mock::elapsed() <= 36000) {
      const auto value = sensors::readSensorSnapshot(mock::now);
      CHECK(value.air.health == Health::Recovering && value.air.validSamples == 1);
      CHECK(value.air.hasSample && !sensors::airFresh(value.air, mock::now));
      sawRecovering = true;
    }
  };
  sensors::beginSensors();
  mock::run("scd41-service", 44000);
  const auto value = sensors::readSensorSnapshot(mock::now);
  CHECK(sawRecovering && value.air.health == Health::Healthy);
  CHECK(value.air.validSamples == 2 && value.air.errors == 3 && value.air.crcErrors == 3);
  CHECK(value.air.recoveryAttempts == 1 && value.air.initializationFailures == 0);
  CHECK(value.air.lastError == 0 && value.air.consecutiveErrors == 0);
  CHECK(mock::stopTimes.size() == 2 && uint32_t(mock::stopTimes[1] - mock::scenarioStart) == 37000);
}

void airSampleTimeout() {
  bool sawStale = false, sawRecovering = false;
  mock::onAdvance = [&] {
    if (mock::elapsed() >= 6000) mock::neverReady = mock::elapsed() < 60000;
    const auto value = sensors::readSensorSnapshot(mock::now);
    if (mock::elapsed() >= 21500 && mock::elapsed() < 25500) {
      CHECK(value.air.health == Health::Stale && value.air.validSamples == 1);
      sawStale = true;
    }
    if (mock::elapsed() >= 26000 && mock::elapsed() <= 55000) {
      CHECK(value.air.health == Health::Recovering && value.air.validSamples == 1);
      CHECK(value.air.lastError == int32_t(sensors::ServiceError::SampleTimeout));
      sawRecovering = true;
    }
  };
  sensors::beginSensors();
  mock::run("scd41-service", 62000);
  const auto value = sensors::readSensorSnapshot(mock::now);
  CHECK(sawStale && sawRecovering && value.air.health == Health::Healthy);
  CHECK(value.air.validSamples == 2 && value.air.errors == 0 && value.air.recoveryAttempts == 1);
  CHECK(mock::stopTimes.size() == 2 && mock::stopTimes[1] == 55500);
}

void errorClassification() {
  unsigned call = 0;
  mock::readyError = [&]() -> int16_t {
    const int16_t codes[] = {0x020B, 0x020F, 0x010C};
    return call < 3 ? codes[call++] : 0;
  };
  sensors::beginSensors();
  mock::run("scd41-service", 39000);
  const auto value = sensors::readSensorSnapshot(mock::now);
  CHECK(value.air.health == Health::Healthy && value.air.validSamples == 1);
  CHECK(value.air.errors == 3 && value.air.crcErrors == 1 && value.air.shortReads == 1 &&
        value.air.nackErrors == 1 && value.air.otherIoErrors == 0);
  CHECK(value.air.recoveryAttempts == 1);
}

void variantRecovery() {
  mock::variant = 0;
  mock::onAdvance = [] { if (mock::elapsed() >= 30000) mock::variant = 0x1000; };
  sensors::beginSensors();
  mock::run("scd41-service", 36000);
  const auto value = sensors::readSensorSnapshot(mock::now);
  CHECK(value.air.health == Health::Healthy && value.air.validSamples == 1);
  CHECK(value.air.initializationFailures == 1 && value.air.recoveryAttempts == 1);
  CHECK(mock::commands == std::vector<std::string>(
    {"stop", "serial", "variant", "stop", "serial", "variant", "start"}));
}

void invalidMeasurements() {
  mock::fixtureCo2 = 0;
  bool sawInvalid = false;
  mock::onAdvance = [&] {
    if (mock::elapsed() >= 6000 && mock::elapsed() < 20000) {
      const auto value = sensors::readSensorSnapshot(mock::now);
      CHECK(!value.air.hasSample && value.air.validSamples == 0);
      CHECK(value.air.invalidSamples >= 1 && value.air.errors == 0);
      CHECK(value.air.lastError == int32_t(sensors::ServiceError::InvalidSample));
      sawInvalid = true;
    }
    if (mock::elapsed() >= 30000) mock::fixtureCo2 = 1600;
  };
  sensors::beginSensors();
  mock::run("scd41-service", 57000);
  const auto value = sensors::readSensorSnapshot(mock::now);
  CHECK(sawInvalid && value.air.health == Health::Healthy && value.air.validSamples == 1);
  CHECK(value.air.invalidSamples == 3 && value.air.errors == 0 && value.air.recoveryAttempts == 1);
}

void measurementFailureRecovery() {
  unsigned failures = 0;
  mock::measurementError = [&]() -> int16_t {
    if (failures < 3) { ++failures; return 0x020B; }
    return 0;
  };
  sensors::beginSensors();
  mock::run("scd41-service", 43000);
  const auto value = sensors::readSensorSnapshot(mock::now);
  CHECK(value.air.health == Health::Healthy && value.air.validSamples == 1);
  CHECK(value.air.errors == 3 && value.air.crcErrors == 3 && value.air.recoveryAttempts == 1);
  CHECK(value.air.lastError == 0 && value.air.consecutiveErrors == 0);
}

void radarStaleRecovery() {
  bool sawStale = false;
  mock::onAdvance = [&] {
    const uint32_t elapsed = mock::elapsed();
    if ((elapsed <= 1000 || elapsed >= 4500) && elapsed % 100 == 0) addRadarFrame();
    if (elapsed >= 3500 && elapsed <= 4400) {
      const auto value = sensors::readSensorSnapshot(mock::now);
      CHECK(value.radar.health == Health::Stale && value.radar.hasSample);
      CHECK(!sensors::radarFresh(value.radar, mock::now));
      sawStale = true;
    }
  };
  sensors::beginSensors();
  mock::run("radar-service", 6000);
  const auto value = sensors::readSensorSnapshot(mock::now);
  CHECK(sawStale && value.radar.health == Health::Healthy);
  CHECK(value.radar.counters.validFrames >= 20 && value.radar.counters.invalidFrames == 0);
  CHECK(mock::uartBeginCalls == 1);
}

void freshnessBoundaries() {
  sensors::RadarSnapshot radar;
  radar.health = Health::Healthy;
  radar.hasSample = true;
  radar.sample.receivedAtMs = UINT32_MAX - 999;
  CHECK(sensors::radarFresh(radar, 1000));
  CHECK(!sensors::radarFresh(radar, 1001));
  radar.sample.receivedAtMs = 2001;
  CHECK(sensors::radarFresh(radar, 2000));
  sensors::AirSnapshot air;
  air.health = Health::Healthy;
  air.hasSample = true;
  air.sampledAtMs = UINT32_MAX - 999;
  CHECK(sensors::airFresh(air, 14000));
  CHECK(!sensors::airFresh(air, 14001));
  air.health = Health::Recovering;
  CHECK(!sensors::airFresh(air, 1000));
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("expected one scenario name");
    const std::string name = argv[1];
    if (name == "happy") happyPath();
    else if (name == "allocation_independence") allocationIndependence();
    else if (name == "uart_ownership") uartOwnershipIndependence();
    else if (name == "i2c_ownership") airOwnershipIndependence();
    else if (name == "address_recovery") airProbeRecovery();
    else if (name == "communication_recovery") airCommunicationRecovery(false);
    else if (name == "recovery_wrap") airCommunicationRecovery(true);
    else if (name == "sample_timeout") airSampleTimeout();
    else if (name == "error_classification") errorClassification();
    else if (name == "variant_recovery") variantRecovery();
    else if (name == "invalid_measurements") invalidMeasurements();
    else if (name == "measurement_failure") measurementFailureRecovery();
    else if (name == "radar_stale_recovery") radarStaleRecovery();
    else if (name == "freshness") freshnessBoundaries();
    else throw std::runtime_error("unknown scenario: " + name);
    verifyCommandAllowlist();
    std::cout << "PASS service " << name << " (offline fixtures)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
