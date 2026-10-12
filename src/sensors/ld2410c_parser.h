#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sensors {

struct RadarSample {
  uint32_t receivedAtMs = 0;
  uint8_t dataType = 0;
  uint8_t targetState = 0;
  // States 4..6 describe background calibration, not human presence.
  bool presenceKnown = false;
  bool presence = false;
  bool moving = false;
  bool stationary = false;
  uint16_t movingDistanceCm = 0;
  uint16_t stillDistanceCm = 0;
  uint16_t detectionDistanceCm = 0;
  uint8_t movingEnergy = 0;
  uint8_t stillEnergy = 0;
};

struct RadarCounters {
  uint32_t receivedBytes = 0;
  uint32_t validFrames = 0;
  // A candidate with a complete header but invalid length/envelope or timeout.
  uint32_t invalidFrames = 0;
  // Noise and bytes discarded while recovering from an invalid candidate.
  uint32_t discardedBytes = 0;
  // Complete envelopes with an unsupported type, layout, or target state.
  uint32_t unsupportedFrames = 0;
  uint32_t unsupportedBytes = 0;
  uint32_t timedOutFrames = 0;
};

// Pure, allocation-free decoder for HLK LD2410C UART protocol V1.09, tables
// 10..15. No hardware access, configuration commands, or checksum invention:
// the protocol's 0x55 0x00 pair is checked as fixed bytes, not as a CRC.
// One owner must call feed/expire; synchronize copies of latest/counters if
// another task reads them. Counters use ordinary uint32_t rollover semantics.
class Ld2410cParser {
 public:
  static constexpr uint32_t kFrameTimeoutMs = 100;
  static constexpr size_t kMaxPayloadBytes = 64;

  // True only when a complete supported frame updates latest(). nowMs is the
  // receive time; feeding buffered UART bytes together with one time is valid.
  bool feed(uint8_t byte, uint32_t nowMs);
  void expire(uint32_t nowMs);
  bool hasSample() const { return hasSample_; }
  bool isFresh(uint32_t nowMs, uint32_t maxAgeMs = 2000) const;
  const RadarSample& latest() const { return latest_; }
  const RadarCounters& counters() const { return counters_; }

 private:
  bool process(uint32_t nowMs);
  void removePrefix(size_t bytes, bool discarded);
  bool decode(size_t payloadLength, uint32_t nowMs);

  uint8_t buffer_[kMaxPayloadBytes + 10] = {};
  size_t used_ = 0;
  uint32_t lastByteAtMs_ = 0;
  bool hasSample_ = false;
  RadarSample latest_;
  RadarCounters counters_;
};

}  // namespace sensors
