#pragma once
#include <Arduino.h>
#include <mbedtls/sha256.h>

namespace storage {
struct OtaAck {
  char transferId[17] = {};
  const char* state = "idle";
  const char* reason = "";
  uint32_t offset = 0;
};

// All methods run on the Arduino loop thread. Only a physical UI choice calls
// approve(); receiving a network packet can never start a flash write.
class OtaReceiver {
 public:
  enum class State : uint8_t { Idle, Offered, Receiving, Complete, Failed };
  OtaReceiver();
  ~OtaReceiver();
  bool offer(const char* id, const char* version, uint32_t size, const char* sha, uint32_t now);
  bool approve(uint32_t now);
  void cancel(const char* reason = "cancelled");
  void disconnected();
  void acceptChunk(const uint8_t* data, size_t size, uint32_t now);
  void commit(const char* id, uint32_t now);
  void abort(const char* id);
  void tick(uint32_t now);
  bool popAck(OtaAck& ack);
  bool offered() const { return state_ == State::Offered; }
  bool active() const { return state_ == State::Receiving || state_ == State::Complete; }
  bool busy() const { return offered() || active(); }
  bool rebootDue(uint32_t now) const { return state_ == State::Complete && now - completedAt_ >= 1500; }
  const char* id() const { return id_; }
  const char* version() const { return version_; }
  uint32_t bytes() const { return expected_; }
  uint8_t percent() const { return expected_ ? static_cast<uint8_t>(uint64_t(offset_) * 100 / expected_) : 0; }
  State state() const { return state_; }
 private:
  void acknowledge(const char* state, const char* reason = "");
  State state_ = State::Idle;
  char id_[17] = {}, version_[25] = {}, expectedSha_[65] = {};
  uint64_t key_ = 0;
  uint32_t expected_ = 0, offset_ = 0, lastAt_ = 0, completedAt_ = 0;
  uint32_t lastOffset_ = 0, lastSize_ = 0, lastCrc_ = 0;
  mbedtls_sha256_context sha_;
  OtaAck ack_;
  bool ackPending_ = false;
};
}  // namespace storage
