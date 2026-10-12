#include "recovering_websocket_client.h"

#include <algorithm>

namespace network {

RecoveringWebSocketsClient::~RecoveringWebSocketsClient() {
  // The upstream destructor calls its graceful disconnect(). Stop the socket
  // first so neither that path nor our cleanup writes into a dead connection.
  started_ = false;
  // A containing owner's members may already have been destroyed.
  onEvent(nullptr);
  clientDisconnect(&_client);
}

void RecoveringWebSocketsClient::begin(const char* host, uint16_t port,
                                      const char* url, const char* protocol) {
  // Copies also make begin(savedHost_.c_str(), ...) safe during recovery.
  const String newHost(host);
  const String newUrl(url);
  const String newProtocol(protocol);
  if (_client.tcp != nullptr) clientDisconnect(&_client);
  savedHost_ = newHost;
  savedUrl_ = newUrl;
  savedProtocol_ = newProtocol;
  WebSocketsClient::begin(savedHost_.c_str(), port, savedUrl_.c_str(),
                          savedProtocol_.c_str());
  started_ = port != 0;
  unconnectedSinceMs_ = millis();
  lineBytes_ = 0;
  headerBytes_ = 0;
  setStage(Stage::Waiting, unconnectedSinceMs_);
}

void RecoveringWebSocketsClient::disconnect() {
  started_ = false;
  clientDisconnect(&_client);
}

const char* RecoveringWebSocketsClient::stageName(Stage stage) {
  switch (stage) {
    case Stage::Waiting: return "WAITING";
    case Stage::TcpConnect: return "TCP_CONNECT";
    case Stage::HttpUpgrade: return "HTTP_UPGRADE";
    case Stage::Connected: return "CONNECTED";
  }
  return "UNKNOWN";
}

void RecoveringWebSocketsClient::setStage(Stage stage, uint32_t nowMs) {
  if (diagnostics_.stage != stage) {
    diagnostics_.stage = stage;
    diagnostics_.stageStartedMs = nowMs;
  }
}

void RecoveringWebSocketsClient::resetHeartbeat(uint32_t nowMs) {
  _client.lastPing = nowMs;
  _client.pongReceived = true;
  _client.pongTimeoutCount = 0;
  ++diagnostics_.heartbeatResets;
}

void RecoveringWebSocketsClient::clientDisconnect(WSclient_t* client) {
  // Upstream cleanup flushes a connected socket, and graceful disconnect sends
  // a close frame. Recovery must never wait for either on an unresponsive peer.
  if (client->tcp != nullptr) client->tcp->stop();
  const bool wasConnected = client->status == WSC_CONNECTED;
  WebSocketsClient::clientDisconnect(client);
  lineBytes_ = 0;
  headerBytes_ = 0;
  const uint32_t nowMs = millis();
  if (wasConnected) unconnectedSinceMs_ = nowMs;
  setStage(Stage::Waiting, nowMs);
}

void RecoveringWebSocketsClient::runCbEvent(WStype_t type, uint8_t* payload,
                                           size_t length) {
  const uint32_t nowMs = millis();
  if (type == WStype_CONNECTED) {
    ++diagnostics_.connections;
    diagnostics_.lastError = NoError;
    resetHeartbeat(nowMs);
    unconnectedSinceMs_ = nowMs;
    setStage(Stage::Connected, nowMs);
  } else if (type == WStype_DISCONNECTED) {
    ++diagnostics_.disconnects;
    setStage(Stage::Waiting, nowMs);
  }
  WebSocketsClient::runCbEvent(type, payload, length);
}

void RecoveringWebSocketsClient::startUpgrade() {
  headerStartedMs_ = static_cast<uint32_t>(_lastHeaderSent);
  lineBytes_ = 0;
  headerBytes_ = 0;
  ++diagnostics_.upgradeAttempts;
  // WiFiClient::setTimeout takes SECONDS on Arduino-ESP32 2.0.17, whereas
  // Stream::setTimeout takes milliseconds. Upstream passes 1500 to the former,
  // accidentally giving readStringUntil() a 25-minute timeout. Set both units
  // explicitly; incremental upgrade reads never invoke a Stream timed read.
  _client.tcp->setTimeout(2);
  _client.tcp->Stream::setTimeout(WEBSOCKETS_TCP_TIMEOUT);
  resetHeartbeat(millis());
  setStage(Stage::HttpUpgrade, millis());
}

void RecoveringWebSocketsClient::pollUpgrade() {
  if (_client.tcp == nullptr || !_client.tcp->connected()) {
    clientDisconnect(&_client);
    return;
  }
  if (static_cast<uint32_t>(millis() - headerStartedMs_) >= kUpgradeTimeoutMs) {
    ++diagnostics_.upgradeTimeouts;
    diagnostics_.lastError = UpgradeTimedOut;
    clientDisconnect(&_client);
    return;
  }
  size_t budget = kHeaderBytesPerPoll;
  while (budget-- > 0 && _client.status == WSC_HEADER &&
         _client.tcp != nullptr && _client.tcp->available() > 0) {
    const int value = _client.tcp->read();
    if (value < 0) break;
    if (++headerBytes_ > kMaxHeaderBytes ||
        (value != '\n' && lineBytes_ >= kMaxHeaderLineBytes) || value == 0) {
      ++diagnostics_.protocolFailures;
      diagnostics_.lastError = HeaderTooLarge;
      clientDisconnect(&_client);
      return;
    }
    if (value == '\n') {
      line_[lineBytes_] = '\0';
      String headerLine(line_);
      lineBytes_ = 0;
      headerLine.trim();
      // The upstream rejection path writes an error string before closing.
      // Reject the same failed upgrade predicates first, avoiding that write
      // into a potentially stalled peer. Successful handshakes still pass
      // through the original parser, including its Sec-WebSocket-Accept check.
      if (headerLine.length() == 0 &&
          (_client.cCode != 101 || !_client.cIsUpgrade || !_client.cIsWebsocket ||
           _client.cAccept.length() == 0 ||
           _client.cAccept != acceptKey(_client.cKey))) {
        ++diagnostics_.protocolFailures;
        diagnostics_.lastError = UpgradeRejected;
        clientDisconnect(&_client);
        return;
      }
      handleHeader(&_client, &headerLine);
      if (_client.status == WSC_NOT_CONNECTED) {
        ++diagnostics_.protocolFailures;
        diagnostics_.lastError = UpgradeRejected;
      }
    } else {
      line_[lineBytes_++] = static_cast<char>(value);
    }
  }
}

void RecoveringWebSocketsClient::restartTransport(uint32_t nowMs) {
  ++diagnostics_.freshRestarts;
  diagnostics_.lastError = FreshTransportRestart;
  // begin() owns the TCP pointer. Clean it up before begin() clears it, and
  // retain authorization headers without printing or otherwise exposing them.
  const String base64Authorization = _client.base64Authorization;
  const String plainAuthorization = _client.plainAuthorization;
  const uint16_t port = _port;
  const unsigned long previousFailureMs = _lastConnectionFail;
  const bool wasWaiting = _client.status == WSC_NOT_CONNECTED;
  clientDisconnect(&_client);
  WebSocketsClient::begin(savedHost_.c_str(), port, savedUrl_.c_str(),
                          savedProtocol_.c_str());
  _client.base64Authorization = base64Authorization;
  _client.plainAuthorization = plainAuthorization;
  unconnectedSinceMs_ = nowMs;
  // A waiting transport already has a scheduled retry, including when a failed
  // connect left its TCP allocation for cleanup. Do not postpone that retry
  // by starting another full backoff interval at every 90-second guard. An
  // active stalled upgrade that we just closed starts its normal backoff now.
  _lastConnectionFail = wasWaiting ? previousFailureMs : nowMs;
  setStage(Stage::Waiting, nowMs);
}

void RecoveringWebSocketsClient::finishPoll(uint32_t startedMs) {
  diagnostics_.maxPollMs = std::max(diagnostics_.maxPollMs,
      static_cast<uint32_t>(millis() - startedMs));
}

void RecoveringWebSocketsClient::loop() {
  if (!started_) return;
  const uint32_t startedMs = millis();
  if (_client.status != WSC_CONNECTED &&
      static_cast<uint32_t>(startedMs - unconnectedSinceMs_) >= kFreshRestartMs) {
    restartTransport(startedMs);
  }
  if (_client.status == WSC_HEADER) {
    pollUpgrade();
    finishPoll(startedMs);
    return;
  }

  // A failed upstream connect leaves its allocated TCP object for the next
  // poll. Release it now, matching upstream ownership and callback semantics.
  if (_client.status == WSC_NOT_CONNECTED && _client.tcp != nullptr) {
    clientDisconnect(&_client);
  }
  const bool attempt = _client.status == WSC_NOT_CONNECTED &&
      static_cast<uint32_t>(millis() - static_cast<uint32_t>(_lastConnectionFail))
          >= static_cast<uint32_t>(_reconnectInterval);
  uint32_t tcpStartedMs = 0;
  if (attempt) {
    ++diagnostics_.tcpAttempts;
    tcpStartedMs = millis();
    setStage(Stage::TcpConnect, tcpStartedMs);
  }
  WebSocketsClient::loop();
  if (attempt) {
    diagnostics_.lastTcpMs = static_cast<uint32_t>(millis() - tcpStartedMs);
    diagnostics_.maxTcpMs = std::max(diagnostics_.maxTcpMs,
                                    diagnostics_.lastTcpMs);
    if (_client.status != WSC_HEADER && _client.status != WSC_CONNECTED) {
      ++diagnostics_.tcpFailures;
      diagnostics_.lastError = TcpConnectFailed;
      setStage(Stage::Waiting, millis());
    }
  }
  if (_client.status == WSC_HEADER) {
    startUpgrade();
    pollUpgrade();
  } else if (_client.status == WSC_CONNECTED) {
    setStage(Stage::Connected, millis());
  }
  finishPoll(startedMs);
}

}  // namespace network
