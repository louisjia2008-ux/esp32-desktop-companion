#include "wifi_manager.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class TestFailure : public std::runtime_error {
 public:
  explicit TestFailure(const std::string& message)
      : std::runtime_error(message) {}
};

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                       \
      throw TestFailure(std::string(__FILE__) + ":" +                       \
                        std::to_string(__LINE__) + ": CHECK failed: " +      \
                        #condition);                                          \
    }                                                                         \
  } while (false)

constexpr char kNamespace[] = "devcomp";
constexpr char kSavedSsid[] = "HomeNet";
constexpr char kSavedPassword[] = "SavedPass99";
constexpr char kBridgeHost[] = "192.168.1.50";

uint32_t after(uint32_t start, uint32_t elapsed) {
  return start + elapsed;
}

void resetRuntime(bool clearPreferences = true) {
  fake_arduino::resetPinsAndClock();
  Serial.clear();
  ESP.reset();
  fake_esp_system::reset();
  fake_esp_wifi::reset();
  fake_wifi::reset();
  if (clearPreferences) {
    fake_preferences::reset();
  } else {
    fake_preferences::resetRuntime();
  }
}

void seedLegacyConfiguration(const std::string& ssid = kSavedSsid,
                             const std::string& password = kSavedPassword) {
  fake_preferences::setString(kNamespace, "ssid", ssid);
  fake_preferences::setString(kNamespace, "pass", password);
  fake_preferences::setString(kNamespace, "bridge_host", kBridgeHost);
  fake_preferences::setUInt(kNamespace, "bridge_port", 8765);
}

void beginAt(WifiManager& manager, uint32_t nowMs) {
  fake_arduino::setMillis(nowMs);
  manager.begin(nowMs);
}

void loopAt(WifiManager& manager, uint32_t nowMs) {
  fake_arduino::setMillis(nowMs);
  manager.loop(nowMs);
}

void enterSetupAfterStartupTimeout(WifiManager& manager,
                                   uint32_t startedAt = 0) {
  loopAt(manager, after(startedAt, 10000));
  CHECK(manager.state() == WifiManager::State::RETRY_WAIT);
  loopAt(manager, after(startedAt, 12000));
  CHECK(manager.state() == WifiManager::State::CONNECTING);
  loopAt(manager, after(startedAt, 22000));
  CHECK(manager.state() == WifiManager::State::RETRY_WAIT);
  loopAt(manager, after(startedAt, 26000));
  CHECK(manager.state() == WifiManager::State::CONNECTING);
  loopAt(manager, after(startedAt, 35999));
  CHECK(!manager.setupMode());
  loopAt(manager, after(startedAt, 36000));
  CHECK(manager.setupMode());
}

std::string extractCsrfToken(const std::string& html) {
  const std::string marker = "name='token' value='";
  const size_t begin = html.find(marker);
  CHECK(begin != std::string::npos);
  const size_t valueBegin = begin + marker.size();
  const size_t end = html.find('\'', valueBegin);
  CHECK(end != std::string::npos);
  CHECK(end > valueBegin);
  return html.substr(valueBegin, end - valueBegin);
}

class PortalSession {
 public:
  explicit PortalSession(std::vector<fake_wifi::Network> networks,
                         uint32_t startedAt = 0)
      : startedAt_(startedAt) {
    resetRuntime();
    seedLegacyConfiguration();
    manager_ = std::make_unique<WifiManager>();
    beginAt(*manager_, startedAt_);
    enterSetupAfterStartupTimeout(*manager_, startedAt_);
    fake_wifi::completeScan(std::move(networks));
    loopAt(*manager_, after(startedAt_, 36001));
    server_ = fake_web_server::latest();
    CHECK(server_ != nullptr);
    CHECK(server_->active());
    CHECK(server_->dispatch(HTTP_GET, "/"));
    CHECK(server_->response().status == 200);
    CHECK(server_->response().body.find(kSavedPassword) == std::string::npos);
    CHECK(server_->response().body.find("name='new_password'") !=
          std::string::npos);
    token_ = extractCsrfToken(server_->response().body);
  }

  WebServer::Arguments baseArguments() const {
    return {{"token", token_},
            {"network_choice", "manual"},
            {"ssid", kSavedSsid},
            {"password_action", "keep"},
            {"new_password", ""},
            {"host", kBridgeHost},
            {"port", "8765"}};
  }

  WifiManager& manager() { return *manager_; }
  WebServer& server() { return *server_; }
  uint32_t startedAt() const { return startedAt_; }

 private:
  uint32_t startedAt_ = 0;
  std::unique_ptr<WifiManager> manager_;
  WebServer* server_ = nullptr;
  std::string token_;
};

void testStartupTimeoutRetainsSetupAccessPoint() {
  resetRuntime();
  seedLegacyConfiguration();
  WifiManager manager;
  beginAt(manager, 0);

  CHECK(manager.state() == WifiManager::State::CONNECTING);
  CHECK(fake_wifi::beginCalls.size() == 1);
  CHECK(fake_wifi::modeValue == WIFI_STA);
  CHECK(!fake_wifi::softAPActive);

  enterSetupAfterStartupTimeout(manager);

  WebServer* server = fake_web_server::latest();
  CHECK(server != nullptr);
  CHECK(manager.setupMode());
  CHECK(fake_wifi::softAPActive);
  CHECK(fake_wifi::modeValue == WIFI_AP_STA);
  CHECK(server->active());
  CHECK(server->beginCalls() == 1);
  CHECK(fake_wifi::scanNetworksCalls == 1);
  CHECK(fake_wifi::scanCompleteResult == WIFI_SCAN_RUNNING);
  CHECK(!fake_wifi::disconnectCalls.empty());
  CHECK(fake_wifi::disconnectCalls.back().wifiOff);
  CHECK(!fake_wifi::disconnectCalls.back().eraseAccessPoint);
}

void testSetupRetryCyclesWithAttachedClientAndClosesApOnSuccess() {
  resetRuntime();
  seedLegacyConfiguration();
  WifiManager manager;
  beginAt(manager, 0);
  enterSetupAfterStartupTimeout(manager);
  fake_wifi::completeScan({{kSavedSsid, -48, WIFI_AUTH_WPA2_PSK}});
  loopAt(manager, 36001);

  WebServer* server = fake_web_server::latest();
  CHECK(server != nullptr);
  fake_wifi::softAPStations = 1;
  const size_t startsBeforeSetupRetry = fake_wifi::beginCalls.size();
  loopAt(manager, 66000);
  CHECK(fake_wifi::beginCalls.size() == startsBeforeSetupRetry + 1);
  CHECK(fake_wifi::beginCalls.back().ssid == kSavedSsid);
  CHECK(fake_wifi::beginCalls.back().password == kSavedPassword);
  CHECK(fake_wifi::softAPActive);
  CHECK(server->active());
  CHECK(fake_wifi::modeValue == WIFI_AP_STA);

  loopAt(manager, 75999);
  CHECK(fake_wifi::disconnectCalls.back().wifiOff);
  loopAt(manager, 76000);
  CHECK(!fake_wifi::disconnectCalls.back().wifiOff);
  CHECK(!fake_wifi::disconnectCalls.back().eraseAccessPoint);
  CHECK(fake_wifi::softAPActive);
  CHECK(server->active());

  loopAt(manager, 106000);
  CHECK(fake_wifi::beginCalls.size() == startsBeforeSetupRetry + 2);
  fake_wifi::stationStatus = WL_CONNECTED;
  loopAt(manager, 106001);

  CHECK(manager.connected());
  CHECK(!manager.setupMode());
  CHECK(!fake_wifi::softAPActive);
  CHECK(fake_wifi::softAPDisconnectCalls == 1);
  CHECK(!server->active());
  CHECK(server->stopCalls() == 1);
  CHECK(fake_wifi::modeValue == WIFI_STA);
}

void testKeepPasswordPersistsAndRestartPendingPausesRadioWork() {
  {
    PortalSession session({{kSavedSsid, -51, WIFI_AUTH_WPA2_PSK}});
    auto arguments = session.baseArguments();
    arguments["network_choice"] = "0";
    const size_t startsBeforeSave = fake_wifi::beginCalls.size();
    const uint32_t saveAt = std::numeric_limits<uint32_t>::max() - 300U;
    fake_arduino::setMillis(saveAt);
    CHECK(session.server().dispatch(HTTP_POST, "/save", arguments));
    CHECK(session.server().response().status == 200);
    CHECK(fake_preferences::putBytesCalls == 1);

    loopAt(session.manager(), after(saveAt, 749));
    CHECK(ESP.restartCalls == 0);
    CHECK(fake_wifi::beginCalls.size() == startsBeforeSave);
    loopAt(session.manager(), after(saveAt, 750));
    CHECK(ESP.restartCalls == 1);
    CHECK(fake_wifi::beginCalls.size() == startsBeforeSave);
  }

  resetRuntime(false);
  WifiManager rebooted;
  beginAt(rebooted, 0);
  CHECK(fake_wifi::beginCalls.size() == 1);
  CHECK(fake_wifi::beginCalls.front().ssid == kSavedSsid);
  CHECK(fake_wifi::beginCalls.front().password == kSavedPassword);
  CHECK(rebooted.bridgeHost() == kBridgeHost);
  CHECK(rebooted.bridgePort() == 8765);
}

void testScanDefersRetryAndQueuedScanNeverOverlapsStationAttempt() {
  resetRuntime();
  seedLegacyConfiguration();
  WifiManager manager;
  beginAt(manager, 0);
  enterSetupAfterStartupTimeout(manager);

  const size_t stationStartsBeforeDue = fake_wifi::beginCalls.size();
  CHECK(fake_wifi::scanCompleteResult == WIFI_SCAN_RUNNING);
  loopAt(manager, 66000);
  CHECK(fake_wifi::beginCalls.size() == stationStartsBeforeDue);
  CHECK(fake_wifi::scanNetworksCalls == 1);

  fake_wifi::completeScan({{kSavedSsid, -50, WIFI_AUTH_WPA2_PSK}});
  loopAt(manager, 66001);
  CHECK(fake_wifi::beginCalls.size() == stationStartsBeforeDue + 1);
  CHECK(fake_wifi::scanNetworksCalls == 1);

  WebServer* server = fake_web_server::latest();
  CHECK(server != nullptr);
  fake_arduino::setMillis(66002);
  CHECK(server->dispatch(HTTP_GET, "/scan"));
  CHECK(server->response().status == 303);
  CHECK(fake_wifi::scanNetworksCalls == 1);
  CHECK(server->dispatch(HTTP_GET, "/"));
  CHECK(server->response().status == 200);
  CHECK(server->response().body.find("Scan queued") != std::string::npos);
  CHECK(server->response().body.find("http-equiv='refresh'") !=
        std::string::npos);

  loopAt(manager, 76001);
  CHECK(fake_wifi::scanNetworksCalls == 1);
  CHECK(fake_wifi::beginCalls.size() == stationStartsBeforeDue + 1);
  loopAt(manager, 76002);
  CHECK(fake_wifi::scanNetworksCalls == 2);
  CHECK(fake_wifi::scanCompleteResult == WIFI_SCAN_RUNNING);
  CHECK(fake_wifi::beginCalls.size() == stationStartsBeforeDue + 1);
  CHECK(fake_wifi::softAPActive);
}

void testSetupTimeoutRechecksConnectedBeforeDisconnecting() {
  resetRuntime();
  seedLegacyConfiguration();
  WifiManager manager;
  beginAt(manager, 0);
  enterSetupAfterStartupTimeout(manager);
  fake_wifi::completeScan({{kSavedSsid, -50, WIFI_AUTH_WPA2_PSK}});
  loopAt(manager, 36001);
  loopAt(manager, 66000);

  WebServer* server = fake_web_server::latest();
  CHECK(server != nullptr);
  const size_t disconnectsBeforeBoundary = fake_wifi::disconnectCalls.size();
  fake_wifi::scriptedStatuses = {WL_DISCONNECTED, WL_CONNECTED};
  loopAt(manager, 76000);

  CHECK(manager.connected());
  CHECK(fake_wifi::disconnectCalls.size() == disconnectsBeforeBoundary);
  CHECK(!fake_wifi::softAPActive);
  CHECK(!server->active());
  CHECK(fake_wifi::modeValue == WIFI_STA);
}

void testConnectedDropKeepsRetryingWithoutReturningToSetup() {
  resetRuntime();
  seedLegacyConfiguration();
  WifiManager manager;
  beginAt(manager, 0);
  fake_wifi::stationStatus = WL_CONNECTED;
  loopAt(manager, 100);
  CHECK(manager.connected());

  fake_wifi::stationStatus = WL_DISCONNECTED;
  loopAt(manager, 200);
  CHECK(manager.state() == WifiManager::State::RETRY_WAIT);
  for (uint32_t elapsed = 1000; elapsed <= 200000; elapsed += 1000) {
    loopAt(manager, 200 + elapsed);
    CHECK(!manager.setupMode());
    CHECK(!fake_wifi::softAPActive);
    CHECK(fake_wifi::modeValue == WIFI_STA);
  }
  CHECK(fake_wifi::beginCalls.size() >= 6);
  CHECK(!fake_web_server::latest()->active());
}

void testNoCredentialsNeverStartsAutomaticStationRetry() {
  resetRuntime();
  WifiManager manager;
  beginAt(manager, 0);
  CHECK(manager.setupMode());
  CHECK(fake_wifi::beginCalls.empty());
  CHECK(fake_wifi::softAPActive);

  fake_wifi::completeScan({{"CafeOpen", -55, WIFI_AUTH_OPEN}});
  loopAt(manager, 1);
  for (uint32_t nowMs = 30000; nowMs <= 180000; nowMs += 30000) {
    loopAt(manager, nowMs);
  }
  CHECK(fake_wifi::beginCalls.empty());
  CHECK(manager.setupMode());
  CHECK(fake_wifi::softAPActive);
}

void testSetupRetryDeadlineSurvivesUint32Wrap() {
  resetRuntime();
  seedLegacyConfiguration();
  const uint32_t startedAt =
      std::numeric_limits<uint32_t>::max() - 20000U;
  WifiManager manager;
  beginAt(manager, startedAt);
  enterSetupAfterStartupTimeout(manager, startedAt);
  fake_wifi::completeScan({{kSavedSsid, -50, WIFI_AUTH_WPA2_PSK}});
  loopAt(manager, after(startedAt, 36001));

  const size_t startsBeforeDue = fake_wifi::beginCalls.size();
  loopAt(manager, after(startedAt, 65999));
  CHECK(fake_wifi::beginCalls.size() == startsBeforeDue);
  loopAt(manager, after(startedAt, 66000));
  CHECK(fake_wifi::beginCalls.size() == startsBeforeDue + 1);
  CHECK(fake_wifi::beginCalls.back().atMs == after(startedAt, 66000));
  CHECK(manager.setupMode());
  CHECK(fake_wifi::softAPActive);
}

void expectRejectedWithoutPersistence(
    PortalSession& session, WebServer::Arguments arguments) {
  const fake_preferences::Storage before = fake_preferences::snapshot();
  const size_t putsBefore = fake_preferences::putBytesCalls;
  CHECK(session.server().dispatch(HTTP_POST, "/save", std::move(arguments)));
  CHECK(session.server().response().status == 400);
  CHECK(fake_preferences::snapshot() == before);
  CHECK(fake_preferences::putBytesCalls == putsBefore);
  loopAt(session.manager(), after(session.startedAt(), 37001));
  CHECK(ESP.restartCalls == 0);
}

void testPasswordValidationRejectsAmbiguityWithoutChangingNvs() {
  struct RejectionCase {
    const char* name;
    std::vector<fake_wifi::Network> networks;
    std::function<void(WebServer::Arguments&)> modify;
  };
  const std::vector<RejectionCase> cases = {
      {"missing action",
       {{kSavedSsid, -50, WIFI_AUTH_WPA2_PSK}},
       [](auto& arguments) { arguments.erase("password_action"); }},
      {"invalid action",
       {{kSavedSsid, -50, WIFI_AUTH_WPA2_PSK}},
       [](auto& arguments) { arguments["password_action"] = "legacy"; }},
      {"keep with new input",
       {{kSavedSsid, -50, WIFI_AUTH_WPA2_PSK}},
       [](auto& arguments) { arguments["new_password"] = "NewPass123"; }},
      {"keep for different manual SSID",
       {{kSavedSsid, -50, WIFI_AUTH_WPA2_PSK}},
       [](auto& arguments) { arguments["ssid"] = "OtherNet"; }},
      {"blank replacement for saved manual SSID",
       {{kSavedSsid, -50, WIFI_AUTH_WPA2_PSK}},
       [](auto& arguments) { arguments["password_action"] = "replace"; }},
      {"keep on explicitly open saved SSID",
       {{kSavedSsid, -50, WIFI_AUTH_OPEN}},
       [](auto& arguments) { arguments["network_choice"] = "0"; }},
      {"blank replacement for protected SSID",
       {{"ProtectedNet", -50, WIFI_AUTH_WPA2_PSK}},
       [](auto& arguments) {
         arguments["network_choice"] = "0";
         arguments["password_action"] = "replace";
       }},
      {"blank replacement for mixed SSID",
       {{"MixedNet", -50, WIFI_AUTH_OPEN},
        {"MixedNet", -60, WIFI_AUTH_WPA2_PSK}},
       [](auto& arguments) {
         arguments["network_choice"] = "0";
         arguments["password_action"] = "replace";
       }},
      {"password on explicitly open SSID",
       {{"CafeOpen", -50, WIFI_AUTH_OPEN}},
       [](auto& arguments) {
         arguments["network_choice"] = "0";
         arguments["password_action"] = "replace";
         arguments["new_password"] = "ShouldNotSave";
       }},
  };

  for (const auto& rejection : cases) {
    try {
      PortalSession session(rejection.networks);
      auto arguments = session.baseArguments();
      rejection.modify(arguments);
      expectRejectedWithoutPersistence(session, std::move(arguments));
    } catch (const std::exception& error) {
      throw TestFailure(std::string(rejection.name) + ": " + error.what());
    }
  }
}

void testForgedKeepWithoutStoredPasswordIsRejected() {
  resetRuntime();
  WifiManager manager;
  beginAt(manager, 0);
  fake_wifi::completeScan({{"ProtectedNet", -50, WIFI_AUTH_WPA2_PSK}});
  loopAt(manager, 1);
  WebServer* server = fake_web_server::latest();
  CHECK(server != nullptr);
  CHECK(server->dispatch(HTTP_GET, "/"));
  const std::string token = extractCsrfToken(server->response().body);
  const fake_preferences::Storage before = fake_preferences::snapshot();
  WebServer::Arguments arguments = {
      {"token", token},
      {"network_choice", "0"},
      {"ssid", ""},
      {"password_action", "keep"},
      {"new_password", ""},
      {"host", kBridgeHost},
      {"port", "8765"},
  };
  CHECK(server->dispatch(HTTP_POST, "/save", arguments));
  CHECK(server->response().status == 400);
  CHECK(fake_preferences::snapshot() == before);
  CHECK(fake_preferences::putBytesCalls == 0);
}

void verifySavedCredentialsAfterReboot(
    std::vector<fake_wifi::Network> networks,
    const std::function<void(WebServer::Arguments&)>& modify,
    const std::string& expectedSsid, const std::string& expectedPassword) {
  {
    PortalSession session(std::move(networks));
    auto arguments = session.baseArguments();
    modify(arguments);
    fake_arduino::setMillis(65999);
    const size_t stationStartsBeforeSave = fake_wifi::beginCalls.size();
    CHECK(session.server().dispatch(HTTP_POST, "/save", arguments));
    CHECK(session.server().response().status == 200);
    CHECK(fake_preferences::putBytesCalls == 1);
    loopAt(session.manager(), 66000);
    CHECK(ESP.restartCalls == 0);
    CHECK(fake_wifi::beginCalls.size() == stationStartsBeforeSave);
  }

  resetRuntime(false);
  WifiManager rebooted;
  beginAt(rebooted, 0);
  CHECK(fake_wifi::beginCalls.size() == 1);
  CHECK(fake_wifi::beginCalls.front().ssid == expectedSsid);
  CHECK(fake_wifi::beginCalls.front().password == expectedPassword);
}

void testReplaceAndExplicitOpenClearPersistExactPasswords() {
  verifySavedCredentialsAfterReboot(
      {{"OtherNet", -47, WIFI_AUTH_WPA2_PSK}},
      [](auto& arguments) {
        arguments["network_choice"] = "0";
        arguments["password_action"] = "replace";
        arguments["new_password"] = "Replacement42";
      },
      "OtherNet", "Replacement42");

  verifySavedCredentialsAfterReboot(
      {{kSavedSsid, -47, WIFI_AUTH_OPEN}},
      [](auto& arguments) {
        arguments["network_choice"] = "0";
        arguments["password_action"] = "replace";
        arguments["new_password"] = "";
      },
      kSavedSsid, "");
}

void writeBrowserFixture(const std::filesystem::path& path,
                         const std::string& html) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  CHECK(output.is_open());
  output.write(html.data(), static_cast<std::streamsize>(html.size()));
  CHECK(output.good());
}

void testOptionalBrowserFixtureExport() {
  const char* requestedDirectory = std::getenv("WIFI_TEST_FIXTURE_DIR");
  if (requestedDirectory == nullptr || requestedDirectory[0] == '\0') {
    return;
  }
  const std::filesystem::path outputDirectory(requestedDirectory);
  std::error_code directoryError;
  std::filesystem::create_directories(outputDirectory, directoryError);
  CHECK(!directoryError);
  CHECK(std::filesystem::is_directory(outputDirectory));

  std::string savedPasswordPage;
  {
    PortalSession session(
        {{kSavedSsid, -43, WIFI_AUTH_WPA2_PSK},
         {"CafeOpen", -61, WIFI_AUTH_OPEN},
         {"MixedLab", -68, WIFI_AUTH_OPEN},
         {"MixedLab", -71, WIFI_AUTH_WPA2_WPA3_PSK}});
    savedPasswordPage = session.server().response().body;
    CHECK(savedPasswordPage.find("Use saved password") != std::string::npos);
    CHECK(savedPasswordPage.find(kSavedPassword) == std::string::npos);
  }
  writeBrowserFixture(outputDirectory / "saved-password-portal.html",
                      savedPasswordPage);

  std::string queuedScanPage;
  {
    resetRuntime();
    seedLegacyConfiguration();
    WifiManager manager;
    beginAt(manager, 0);
    enterSetupAfterStartupTimeout(manager);
    fake_wifi::completeScan({{kSavedSsid, -48, WIFI_AUTH_WPA2_PSK}});
    loopAt(manager, 36001);
    loopAt(manager, 66000);
    WebServer* server = fake_web_server::latest();
    CHECK(server != nullptr);
    fake_arduino::setMillis(66001);
    CHECK(server->dispatch(HTTP_GET, "/scan"));
    CHECK(server->dispatch(HTTP_GET, "/"));
    queuedScanPage = server->response().body;
    CHECK(queuedScanPage.find("Scan queued") != std::string::npos);
    CHECK(queuedScanPage.find(kSavedPassword) == std::string::npos);
  }
  writeBrowserFixture(outputDirectory / "queued-scan-portal.html",
                      queuedScanPage);
  std::cout << "Browser fixtures: " << outputDirectory.string() << '\n';
}

using TestFunction = std::function<void()>;

}  // namespace

constexpr char kPairingCode[] = "00112233-44556677-8899AABB-CCDDEEFF";

void testPairingCodePersistsAndSurvivesKeepAndReboot() {
  {
    PortalSession session({{kSavedSsid, -51, WIFI_AUTH_WPA2_PSK}});
    CHECK(!session.manager().hasPairingKey());
    auto arguments = session.baseArguments();
    arguments["pairing"] = kPairingCode;
    CHECK(session.server().dispatch(HTTP_POST, "/save", arguments));
    CHECK(session.server().response().status == 200);
    CHECK(fake_preferences::putBytesCalls == 2);  // configuration + key
  }
  resetRuntime(false);
  {
    WifiManager rebooted;
    beginAt(rebooted, 0);
    CHECK(rebooted.hasPairingKey());
    const uint8_t expected[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                  0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    CHECK(memcmp(rebooted.pairingKey(), expected, sizeof(expected)) == 0);
  }
  // A later save with the field left empty keeps the stored key untouched.
  {
    resetRuntime(false);
    WifiManager manager;
    beginAt(manager, 0);
    enterSetupAfterStartupTimeout(manager);
    fake_wifi::completeScan({{kSavedSsid, -51, WIFI_AUTH_WPA2_PSK}});
    loopAt(manager, 36001);
    WebServer* server = fake_web_server::latest();
    CHECK(server->dispatch(HTTP_GET, "/"));
    CHECK(server->response().body.find("leave empty to keep") != std::string::npos);
    CHECK(server->response().body.find("00112233") == std::string::npos);
    WebServer::Arguments arguments = {
        {"token", extractCsrfToken(server->response().body)},
        {"network_choice", "manual"}, {"ssid", kSavedSsid},
        {"password_action", "keep"}, {"new_password", ""},
        {"host", kBridgeHost}, {"port", "8765"}, {"pairing", ""}};
    const size_t putsBefore = fake_preferences::putBytesCalls;
    CHECK(server->dispatch(HTTP_POST, "/save", arguments));
    CHECK(server->response().status == 200);
    CHECK(fake_preferences::putBytesCalls == putsBefore + 1);
  }
  resetRuntime(false);
  WifiManager again;
  beginAt(again, 0);
  CHECK(again.hasPairingKey());
}

void testInvalidPairingCodeIsRejectedWithoutChangingNvs() {
  PortalSession session({{kSavedSsid, -51, WIFI_AUTH_WPA2_PSK}});
  for (const char* code : {"0011", "zz112233445566778899aabbccddeeff",
                           "00112233445566778899aabbccddeeff00",
                           "00112233 44556677 8899aabb ccddee"}) {
    auto arguments = session.baseArguments();
    arguments["pairing"] = code;
    expectRejectedWithoutPersistence(session, arguments);
  }
  CHECK(!session.manager().hasPairingKey());
}

void pressBoot(WifiManager& manager, uint32_t from, uint32_t heldMs) {
  fake_arduino::setPin(0, LOW);
  for (uint32_t t = from; t <= from + heldMs; t += 100) loopAt(manager, t);
  fake_arduino::setPin(0, HIGH);
  loopAt(manager, from + heldMs + 100);
}

void testBootHoldOpensSetupWithoutErasing() {
  resetRuntime();
  seedLegacyConfiguration();
  WifiManager manager;
  beginAt(manager, 0);
  fake_wifi::stationStatus = WL_CONNECTED;
  loopAt(manager, 100);
  CHECK(manager.connected());

  pressBoot(manager, 1000, 800);  // a short press still does nothing
  CHECK(manager.connected() && !manager.setupMode());

  pressBoot(manager, 3000, 2000);
  CHECK(manager.setupMode() && manager.manualSetupActive());
  CHECK(fake_wifi::softAPActive);
  CHECK(manager.hasSavedNetwork() && manager.bridgeHost() == kBridgeHost);
  CHECK(ESP.restartCalls == 0);

  // The saved network is back, but the hotspot stays open for the window.
  fake_wifi::stationStatus = WL_CONNECTED;
  const size_t startsBefore = fake_wifi::beginCalls.size();
  for (uint32_t t = 6000; t < 6000 + 290000; t += 1000) {
    loopAt(manager, t);
    CHECK(manager.setupMode() && fake_wifi::softAPActive);
  }
  CHECK(fake_wifi::beginCalls.size() == startsBefore);
  // After the window the normal setup retry reconnects and closes the AP.
  for (uint32_t t = 305200; t < 305200 + 60000 && manager.setupMode(); t += 500) {
    loopAt(manager, t);
  }
  CHECK(manager.connected() && !fake_wifi::softAPActive && !manager.manualSetupActive());
}

void testExitSetupKeepsConfigurationAndReconnects() {
  resetRuntime();seedLegacyConfiguration();WifiManager manager;beginAt(manager,0);
  fake_wifi::stationStatus=WL_CONNECTED;loopAt(manager,100);
  const auto before=fake_preferences::snapshot();
  fake_arduino::setMillis(1000);manager.openDeviceSettings(1000);
  CHECK(manager.manualSetupActive()&&fake_wifi::softAPActive);
  fake_arduino::setMillis(1200);manager.closeDeviceSettings(1200);
  CHECK(!manager.manualSetupActive()&&!fake_wifi::softAPActive);
  CHECK(!manager.setupMode());CHECK(manager.hasSavedNetwork());
  CHECK(fake_preferences::snapshot()==before);CHECK(ESP.restartCalls==0);
  fake_wifi::stationStatus=WL_CONNECTED;loopAt(manager,1300);CHECK(manager.connected());
  // Also handle the station already recovering before the user exits.
  fake_arduino::setMillis(1400);manager.openDeviceSettings(1400);
  fake_wifi::stationStatus=WL_CONNECTED;manager.closeDeviceSettings(1500);CHECK(manager.connected());
  CHECK(fake_preferences::snapshot()==before);
  resetRuntime();WifiManager firstBoot;beginAt(firstBoot,0);
  firstBoot.closeDeviceSettings(100);CHECK(!fake_wifi::softAPActive);CHECK(!firstBoot.hasSavedNetwork());
  for(uint32_t at=200;at<2000;at+=100)loopAt(firstBoot,at);
  CHECK(!fake_wifi::softAPActive&&fake_wifi::beginCalls.empty());
}

int main() {
  const std::vector<std::pair<std::string, TestFunction>> tests = {
      {"startup timeout retains setup AP",
       testStartupTimeoutRetainsSetupAccessPoint},
      {"setup retry cycles with attached client and closes AP",
       testSetupRetryCyclesWithAttachedClientAndClosesApOnSuccess},
      {"keep password persists and restart pending pauses radio work",
       testKeepPasswordPersistsAndRestartPendingPausesRadioWork},
      {"scan defers retry and queued scan never overlaps station attempt",
       testScanDefersRetryAndQueuedScanNeverOverlapsStationAttempt},
      {"setup timeout rechecks connected before disconnecting",
       testSetupTimeoutRechecksConnectedBeforeDisconnecting},
      {"connected drop keeps retrying without setup",
       testConnectedDropKeepsRetryingWithoutReturningToSetup},
      {"no credentials never starts station retry",
       testNoCredentialsNeverStartsAutomaticStationRetry},
      {"setup retry deadline survives uint32 wrap",
       testSetupRetryDeadlineSurvivesUint32Wrap},
      {"ambiguous passwords are rejected without NVS changes",
       testPasswordValidationRejectsAmbiguityWithoutChangingNvs},
      {"forged keep without stored password is rejected",
       testForgedKeepWithoutStoredPasswordIsRejected},
      {"replace and explicit open clear persist exact passwords",
       testReplaceAndExplicitOpenClearPersistExactPasswords},
      {"pairing code persists, survives keep and reboot",
       testPairingCodePersistsAndSurvivesKeepAndReboot},
      {"invalid pairing code is rejected without NVS changes",
       testInvalidPairingCodeIsRejectedWithoutChangingNvs},
      {"BOOT hold of 1.5-5 s opens setup without erasing",
       testBootHoldOpensSetupWithoutErasing},
      {"exit device setup closes AP, preserves NVS and reconnects",
       testExitSetupKeepsConfigurationAndReconnects},
      {"optional browser fixture export", testOptionalBrowserFixtureExport},
  };

  size_t passed = 0;
  for (const auto& test : tests) {
    try {
      test.second();
      ++passed;
      std::cout << "PASS " << test.first << '\n';
    } catch (const std::exception& error) {
      std::cerr << "FAIL " << test.first << "\n  " << error.what() << '\n';
    }
  }
  std::cout << passed << '/' << tests.size() << " tests passed\n";
  return passed == tests.size() ? 0 : 1;
}
