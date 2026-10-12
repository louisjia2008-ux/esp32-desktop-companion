#include "ld2410c_parser.h"

#include <string.h>

namespace sensors {
namespace {
constexpr uint8_t kHeader[] = {0xF4, 0xF3, 0xF2, 0xF1};
constexpr uint8_t kFooter[] = {0xF8, 0xF7, 0xF6, 0xF5};
constexpr size_t kNormalPayloadLength = 13;

uint16_t readLe16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) |
         (static_cast<uint16_t>(bytes[1]) << 8);
}
}  // namespace

void Ld2410cParser::removePrefix(size_t bytes, bool discarded) {
  if (discarded) counters_.discardedBytes += static_cast<uint32_t>(bytes);
  used_ -= bytes;
  memmove(buffer_, buffer_ + bytes, used_);
}

void Ld2410cParser::expire(uint32_t nowMs) {
  // Unsigned subtraction remains correct across millis() rollover.
  if (used_ == 0 ||
      static_cast<uint32_t>(nowMs - lastByteAtMs_) < kFrameTimeoutMs) return;
  if (used_ >= sizeof(kHeader)) {
    ++counters_.invalidFrames;
    ++counters_.timedOutFrames;
  }
  removePrefix(used_, true);
}

bool Ld2410cParser::isFresh(uint32_t nowMs, uint32_t maxAgeMs) const {
  return hasSample_ &&
         static_cast<uint32_t>(nowMs - latest_.receivedAtMs) <= maxAgeMs;
}

bool Ld2410cParser::feed(uint8_t byte, uint32_t nowMs) {
  expire(nowMs);
  ++counters_.receivedBytes;
  // process() normally consumes/rejects at capacity. Retain this guard so a
  // future accepted layout cannot make a malformed stream overrun memory.
  if (used_ == sizeof(buffer_)) {
    ++counters_.invalidFrames;
    removePrefix(1, true);
  }
  buffer_[used_++] = byte;
  lastByteAtMs_ = nowMs;
  return process(nowMs);
}

bool Ld2410cParser::process(uint32_t nowMs) {
  bool updated = false;
  while (used_ != 0) {
    const size_t prefixLength = used_ < sizeof(kHeader) ? used_ : sizeof(kHeader);
    if (memcmp(buffer_, kHeader, prefixLength) != 0) {
      removePrefix(1, true);
      continue;
    }
    if (used_ < 6) break;
    const size_t payloadLength = readLe16(buffer_ + 4);
    if (payloadLength < kNormalPayloadLength || payloadLength > kMaxPayloadBytes) {
      ++counters_.invalidFrames;
      removePrefix(1, true);
      continue;
    }
    // Basic-information frames have exactly 13 bytes, including type and
    // fixed markers. Reject bad normal lengths before waiting for more bytes.
    if (used_ >= 7 && buffer_[6] == 0x02 && payloadLength != kNormalPayloadLength) {
      ++counters_.invalidFrames;
      removePrefix(1, true);
      continue;
    }
    const size_t frameLength = payloadLength + 10;
    if (used_ < frameLength) break;
    if (buffer_[7] != 0xAA || buffer_[6 + payloadLength - 2] != 0x55 ||
        buffer_[6 + payloadLength - 1] != 0x00 ||
        memcmp(buffer_ + 6 + payloadLength, kFooter, sizeof(kFooter)) != 0) {
      ++counters_.invalidFrames;
      // Search every remaining byte: a truncated candidate may contain the
      // beginning (or entirety) of the next genuine frame.
      removePrefix(1, true);
      continue;
    }
    if (decode(payloadLength, nowMs)) {
      ++counters_.validFrames;
      updated = true;
    } else {
      ++counters_.unsupportedFrames;
      counters_.unsupportedBytes += static_cast<uint32_t>(frameLength);
    }
    removePrefix(frameLength, false);
  }
  return updated;
}

bool Ld2410cParser::decode(size_t payloadLength, uint32_t nowMs) {
  const uint8_t* payload = buffer_ + 6;
  if (payload[0] == 0x01) {
    // Table 15 adds two maximum-gate indices, their N+1 energy arrays,
    // light level and OUT state. Gate indices are 0..8. Do not interpret
    // another firmware's engineering layout as a normal frame.
    if (payloadLength < 19 || payload[11] > 8 || payload[12] > 8 ||
        payloadLength != static_cast<size_t>(19 + payload[11] + payload[12])) {
      return false;
    }
  } else if (payload[0] != 0x02) {
    return false;
  }
  if (payload[2] > 6) return false;

  RadarSample sample;
  sample.receivedAtMs = nowMs;
  sample.dataType = payload[0];
  sample.targetState = payload[2];
  sample.presenceKnown = sample.targetState <= 3;
  sample.presence = sample.presenceKnown && sample.targetState != 0;
  sample.moving = sample.presenceKnown && (sample.targetState & 1) != 0;
  sample.stationary = sample.presenceKnown && (sample.targetState & 2) != 0;
  sample.movingDistanceCm = readLe16(payload + 3);
  sample.movingEnergy = payload[5];
  sample.stillDistanceCm = readLe16(payload + 6);
  sample.stillEnergy = payload[8];
  sample.detectionDistanceCm = readLe16(payload + 9);
  latest_ = sample;
  hasSample_ = true;
  return true;
}

}  // namespace sensors
