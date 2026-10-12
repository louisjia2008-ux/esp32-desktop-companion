#include "network/recovering_websocket_client.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using Client = network::RecoveringWebSocketsClient;
using Stage = Client::Stage;

class Probe : public Client {
 public:
  WiFiClient* socket() { return _client.tcp; }
  WSclientsStatus_t state() const { return _client.status; }
  uint8_t heartbeatFailures() const { return _client.pongTimeoutCount; }
  uint32_t lastPing() const { return _client.lastPing; }
  bool pongReceived() const { return _client.pongReceived; }
  bool preservedOptions() const {
    return _client.pingInterval == 10000 && _client.pongTimeout == 3000 &&
        _client.disconnectTimeoutCount == 2 && _reconnectInterval == 100;
  }
  void poisonHeartbeat() {
    _client.pongTimeoutCount = 2;
    _client.lastPing = millis() - 100000;
    _client.pongReceived = false;
  }
  std::string response(bool valid = true) {
    return "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n"
           "Upgrade: websocket\r\nSec-WebSocket-Accept: " +
        (valid ? acceptKey(_client.cKey).stdString() : "wrong") + "\r\n\r\n";
  }
  void open() {
    begin("192.0.2.1", 8765);
    setReconnectInterval(100);
    enableHeartbeat(10000, 3000, 2);
    loop();
    assert(state() == WSC_HEADER);
  }
  void complete() {
    socket()->feed(response());
    loop();
    assert(state() == WSC_CONNECTED);
  }
};

class UpstreamProbe : public WebSocketsClient {
 public:
  WiFiClient* socket() { return _client.tcp; }
};

void upstream_timeout_units() {
  fake_network::reset();
  UpstreamProbe upstream;
  upstream.begin("192.0.2.1", 8765);
  upstream.loop();
  assert(upstream.socket()->timeoutSeconds == 1500);
  assert(upstream.socket()->getTimeout() == 1500000);
  upstream.socket()->feed("HTTP/1.1 101");
  const uint32_t before = millis();
  upstream.loop();
  assert(millis() - before == 1500000);
  assert(fake_network::timedLineReads == 1);
}

void fragmented_upgrade() {
  fake_network::reset();
  Probe client;
  client.open();
  assert(client.socket()->timeoutSeconds == 2);
  assert(client.socket()->getTimeout() == 1500);
  const auto response = client.response();
  size_t offset = 0;
  while (offset < response.size()) {
    const size_t length = std::min(size_t{7}, response.size() - offset);
    client.socket()->feed(response.substr(offset, length));
    const uint32_t before = millis();
    client.loop();
    assert(millis() == before);
    offset += length;
    delay(100);
  }
  assert(client.state() == WSC_CONNECTED);
  assert(client.diagnostics().connections == 1);
  assert(client.diagnostics().upgradeAttempts == 1);
  assert(fake_network::timedLineReads == 0);
  assert(client.diagnostics().maxPollMs == 0);
}

void partial_header_deadline() {
  fake_network::reset();
  Probe client;
  client.open();
  client.socket()->feed("HTTP/1.1 101 Switching Protocols\r");
  client.loop();
  delay(Client::kUpgradeTimeoutMs - 1);
  client.loop();
  assert(client.state() == WSC_HEADER);
  delay(1);
  const auto writes = fake_network::writeCalls;
  client.loop();
  assert(client.state() == WSC_NOT_CONNECTED);
  assert(client.socket() == nullptr);
  assert(client.diagnostics().upgradeTimeouts == 1);
  assert(fake_network::writeCalls == writes);
  assert(fake_network::flushed == 0);
  delay(100);
  client.loop();
  client.complete();
  assert(client.diagnostics().connections == 1);
  assert(client.diagnostics().tcpAttempts == 2);
}

void upgrade_rollover() {
  fake_network::reset(UINT32_MAX - 2000);
  Probe client;
  client.open();
  client.socket()->feed("HTTP/1.1 101");
  client.loop();
  delay(4999);
  client.loop();
  assert(client.state() == WSC_HEADER);
  delay(1);
  client.loop();
  assert(client.diagnostics().upgradeTimeouts == 1);
  assert(client.state() == WSC_NOT_CONNECTED);
}

void header_limits() {
  fake_network::reset();
  Probe client;
  client.open();
  client.socket()->feed(std::string(Client::kMaxHeaderLineBytes + 1, 'x'));
  client.loop();
  assert(client.state() == WSC_HEADER);  // Per-poll budget is finite.
  assert(client.socket()->available() == 1);
  client.loop();
  assert(client.state() == WSC_NOT_CONNECTED);
  assert(client.diagnostics().protocolFailures == 1);
  delay(100);
  client.loop();
  std::string oversized;
  while (oversized.size() <= Client::kMaxHeaderBytes) oversized += "Ignored: x\r\n";
  client.socket()->feed(oversized);
  for (int i = 0; i < 20 && client.socket() != nullptr; ++i) client.loop();
  assert(client.socket() == nullptr);
  assert(client.diagnostics().protocolFailures == 2);
  assert(fake_network::timedLineReads == 0);
}

void rejected_upgrade() {
  fake_network::reset();
  Probe client;
  client.open();
  client.socket()->feed(client.response(false));
  const auto writes = fake_network::writeCalls;
  client.loop();
  assert(client.state() == WSC_NOT_CONNECTED);
  assert(client.diagnostics().protocolFailures == 1);
  assert(client.diagnostics().connections == 0);
  assert(fake_network::writeCalls == writes);
  delay(100);
  client.loop();
  client.complete();
  assert(client.diagnostics().lastError == 0);
}

void heartbeat_and_frames() {
  fake_network::reset();
  Probe client;
  std::vector<WStype_t> events;
  std::string text;
  client.onEvent([&](WStype_t type, uint8_t* data, size_t length) {
    events.push_back(type);
    if (type == WStype_TEXT) text.assign(reinterpret_cast<char*>(data), length);
  });
  client.open();
  client.poisonHeartbeat();
  client.complete();
  assert(client.heartbeatFailures() == 0);
  assert(client.pongReceived());
  assert(client.lastPing() == millis());
  delay(3001);
  client.loop();
  assert(client.state() == WSC_CONNECTED);
  assert(client.heartbeatFailures() == 0);
  client.socket()->feed(std::string("\x81\x05hello", 7));
  client.loop();
  assert(text == "hello");
  delay(7000);
  client.loop();
  assert(!client.pongReceived());
  client.socket()->feed(std::string("\x8a\x00", 2));
  client.loop();
  assert(client.pongReceived());
  assert(client.heartbeatFailures() == 0);
  assert(events.front() == WStype_CONNECTED);
  assert(events.back() == WStype_PONG);
}

void fresh_restart_ownership() {
  fake_network::reset();
  Probe client;
  client.open();
  client.setAuthorization("test-only-authorization");
  client.setExtraHeaders("X-Test: retained");
  const auto initialWrites = fake_network::writeCalls;
  delay(Client::kFreshRestartMs);
  client.loop();
  assert(client.diagnostics().freshRestarts == 1);
  assert(client.socket() == nullptr);
  assert(fake_network::created == fake_network::deleted);
  assert(fake_network::flushed == 0);
  assert(fake_network::writeCalls == initialWrites);
  assert(client.preservedOptions());
  delay(100);
  client.loop();
  assert(client.state() == WSC_HEADER);
  const auto& request = fake_network::sent.back();
  assert(request.find("Authorization: test-only-authorization") != std::string::npos);
  assert(request.find("X-Test: retained") != std::string::npos);
  client.complete();
  delay(Client::kFreshRestartMs);
  client.socket()->feed(std::string("\x8a\x00", 2));
  client.loop();
  assert(client.diagnostics().freshRestarts == 1);
}

void fresh_restart_rollover() {
  fake_network::reset(UINT32_MAX - 2000);
  Probe client;
  client.open();
  // Once the stale header is cleaned up, no TCP attempt should occur before
  // the full configured retry interval; avoid host unsigned-long width effects
  // by exercising our wrap-safe guard at its exact boundary.
  delay(Client::kFreshRestartMs - 1);
  client.loop();
  assert(client.diagnostics().freshRestarts == 0);
  assert(client.diagnostics().upgradeTimeouts == 1);
  delay(1);
  client.loop();
  assert(client.diagnostics().freshRestarts == 1);
  assert(client.socket() == nullptr);
  assert(client.preservedOptions());
  delay(100);
  client.loop();
  client.complete();
  assert(client.diagnostics().connections == 1);
}

void waiting_restart_keeps_retry_deadline_at(uint32_t initialClock, bool retryDue) {
  fake_network::reset(initialClock);
  Probe client;
  client.begin("192.0.2.1", 8765);
  client.setReconnectInterval(30000);
  fake_network::connectResults.push_back({false, 0});
  delay(29000);
  client.loop();
  client.loop();
  assert(client.socket() == nullptr);
  assert(client.diagnostics().tcpAttempts == 1);
  fake_network::connectResults.push_back({false, 0});
  delay(retryDue ? 30000 : 35000);
  client.loop();
  client.loop();
  assert(client.socket() == nullptr);
  assert(client.diagnostics().tcpAttempts == 2);
  // The next retry is at t=89 s (already due) or t=94 s (4 s remaining).
  // In either case the fresh guard at t=90 s must not move it to t=120 s.
  delay(retryDue ? 31000 : 26000);
  client.loop();
  assert(client.diagnostics().freshRestarts == 1);
  if (!retryDue) {
    assert(client.diagnostics().tcpAttempts == 2);
    assert(client.socket() == nullptr);
    delay(3999);
    client.loop();
    assert(client.diagnostics().tcpAttempts == 2);
    delay(1);
    client.loop();
  }
  assert(client.diagnostics().tcpAttempts == 3);
  assert(client.state() == WSC_HEADER);
  client.complete();
}

void waiting_restart_pending_retry() {
  waiting_restart_keeps_retry_deadline_at(1000, false);
}

void waiting_restart_due_retry() {
  waiting_restart_keeps_retry_deadline_at(1000, true);
}

void waiting_restart_retry_rollover() {
  waiting_restart_keeps_retry_deadline_at(UINT32_MAX - 40000, false);
}

void waiting_restart_failed_socket() {
  fake_network::reset();
  Probe client;
  client.begin("192.0.2.1", 8765);
  client.setReconnectInterval(30000);
  // Complete a failed TCP attempt just before the guard; leave its allocated
  // socket pending. Recovery must preserve its actual failure timestamp.
  delay(89900);
  fake_network::connectResults.push_back({false, 0});
  client.loop();
  assert(client.socket() != nullptr);
  assert(client.state() == WSC_NOT_CONNECTED);
  delay(100);
  client.loop();
  assert(client.diagnostics().freshRestarts == 1);
  assert(client.socket() == nullptr);
  assert(fake_network::created == fake_network::deleted);
  delay(29899);
  client.loop();
  assert(client.diagnostics().tcpAttempts == 1);
  delay(1);
  client.loop();
  assert(client.diagnostics().tcpAttempts == 2);
  client.complete();
}

void destructor_callback_suppressed() {
  fake_network::reset();
  size_t disconnectCallbacks = 0;
  {
    Probe client;
    client.onEvent([&](WStype_t type, uint8_t*, size_t) {
      if (type == WStype_DISCONNECTED) ++disconnectCallbacks;
    });
    client.open();
    client.complete();
  }
  assert(disconnectCallbacks == 0);
  assert(fake_network::created == fake_network::deleted);
  assert(fake_network::flushed == 0);
}

void tcp_failure_cadence() {
  fake_network::reset();
  fake_network::connectResults.push_back({false, 1500});
  Probe client;
  client.begin("192.0.2.1", 8765);
  client.setReconnectInterval(100);
  client.loop();
  assert(client.diagnostics().tcpAttempts == 1);
  assert(client.diagnostics().tcpFailures == 1);
  assert(client.diagnostics().lastTcpMs == 1500);
  assert(client.diagnostics().maxPollMs == 1500);
  assert(fake_network::lastConnectTimeout == 1500);
  client.loop();  // Own and delete the failed TCP allocation once.
  assert(fake_network::deleted == 1);
  for (int i = 0; i < 99; ++i) { delay(1); client.loop(); }
  assert(client.diagnostics().tcpAttempts == 1);
  delay(1);
  client.loop();
  client.complete();
  assert(client.diagnostics().tcpAttempts == 2);
  assert(client.diagnostics().tcpFailures == 1);
}

void disconnect_and_rebegin() {
  fake_network::reset();
  {
    Probe client;
    client.open();
    client.complete();
    const auto writes = fake_network::writeCalls;
    client.disconnect();
    client.loop();
    assert(fake_network::writeCalls == writes);
    assert(fake_network::flushed == 0);
    assert(client.socket() == nullptr);
    assert(fake_network::created == fake_network::deleted);
    client.open();
    client.complete();
    client.begin("192.0.2.2", 8766);
    assert(fake_network::created == fake_network::deleted);
    delay(100);
    client.loop();
    client.complete();
  }
  assert(fake_network::created == fake_network::deleted);
  assert(fake_network::flushed == 0);
}

void repeated_peer_reconnect() {
  fake_network::reset();
  {
    Probe client;
    client.open();
    for (int i = 0; i < 300; ++i) {
      client.complete();
      client.poisonHeartbeat();
      client.socket()->stop();
      client.loop();
      assert(client.socket() == nullptr);
      delay(100);
      client.loop();
      assert(client.state() == WSC_HEADER);
      assert(client.heartbeatFailures() == 0);
    }
    assert(client.diagnostics().connections == 300);
    assert(client.diagnostics().disconnects == 300);
    assert(client.diagnostics().tcpAttempts == 301);
  }
  assert(fake_network::created == fake_network::deleted);
  assert(fake_network::flushed == 0);
}

int main() {
  const std::pair<const char*, void (*)()> tests[] = {
    {"upstream_timeout_units", upstream_timeout_units},
    {"fragmented_upgrade", fragmented_upgrade},
    {"partial_header_deadline", partial_header_deadline},
    {"upgrade_rollover", upgrade_rollover},
    {"header_limits", header_limits},
    {"rejected_upgrade", rejected_upgrade},
    {"heartbeat_and_frames", heartbeat_and_frames},
    {"fresh_restart_ownership", fresh_restart_ownership},
    {"fresh_restart_rollover", fresh_restart_rollover},
    {"waiting_restart_pending_retry", waiting_restart_pending_retry},
    {"waiting_restart_due_retry", waiting_restart_due_retry},
    {"waiting_restart_retry_rollover", waiting_restart_retry_rollover},
    {"waiting_restart_failed_socket", waiting_restart_failed_socket},
    {"destructor_callback_suppressed", destructor_callback_suppressed},
    {"tcp_failure_cadence", tcp_failure_cadence},
    {"disconnect_and_rebegin", disconnect_and_rebegin},
    {"repeated_peer_reconnect", repeated_peer_reconnect},
  };
  for (const auto& test : tests) {
    test.second();
    std::cout << "PASS " << test.first << '\n';
  }
  std::cout << "Actual WebSockets " WEBSOCKETS_VERSION ": 17 host scenarios passed\n";
}
