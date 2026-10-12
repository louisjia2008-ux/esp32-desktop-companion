#pragma once
#include "Arduino.h"
#include <deque>
#include <vector>

namespace fake_network {
struct ConnectResult { bool success = true; uint32_t durationMs = 0; };
inline std::deque<ConnectResult> connectResults;
inline uint32_t created = 0, deleted = 0, stopped = 0, flushed = 0;
inline uint32_t connectCalls = 0, lastConnectTimeout = 0;
inline uint32_t writeCalls = 0;
inline std::vector<std::string> sent;
inline void reset(uint32_t clock = 1000) {
  nowMs = clock;
  timedLineReads = 0;
  connectResults.clear();
  created = deleted = stopped = flushed = 0;
  connectCalls = lastConnectTimeout = writeCalls = 0;
  sent.clear();
}
}

class WiFiClient : public Stream {
 public:
  WiFiClient() { ++fake_network::created; }
  ~WiFiClient() override { ++fake_network::deleted; }
  int connect(const char*, uint16_t, int32_t timeoutMs) {
    ++fake_network::connectCalls;
    fake_network::lastConnectTimeout = timeoutMs;
    fake_network::ConnectResult result;
    if (!fake_network::connectResults.empty()) {
      result = fake_network::connectResults.front();
      fake_network::connectResults.pop_front();
    }
    delay(result.durationMs);
    connected_ = result.success;
    return connected_ ? 1 : 0;
  }
  bool connected() const { return connected_; }
  void stop() { connected_ = false; ++fake_network::stopped; }
  void flush() { ++fake_network::flushed; }
  int available() override { return static_cast<int>(incoming_.size()); }
  int read() override {
    if (incoming_.empty()) return -1;
    const uint8_t value = incoming_.front();
    incoming_.pop_front();
    return value;
  }
  int read(uint8_t* output, size_t length) {
    size_t count = 0;
    while (count < length && !incoming_.empty()) output[count++] = read();
    return static_cast<int>(count);
  }
  size_t write(const uint8_t* data, size_t length) {
    ++fake_network::writeCalls;
    if (!connected_) return 0;
    fake_network::sent.emplace_back(reinterpret_cast<const char*>(data), length);
    return length;
  }
  // This is deliberately seconds, matching the pinned ESP32 implementation.
  int setTimeout(uint32_t seconds) {
    timeoutSeconds = seconds;
    Stream::setTimeout(seconds * 1000);
    return 0;
  }
  void setNoDelay(bool) {}
  void feed(const std::string& value) {
    incoming_.insert(incoming_.end(), value.begin(), value.end());
  }
  uint32_t timeoutSeconds = 0;
 private:
  bool connected_ = false;
  std::deque<uint8_t> incoming_;
};

class WiFiServer {};
