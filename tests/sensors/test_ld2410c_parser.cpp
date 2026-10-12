#include "sensors/ld2410c_parser.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using sensors::Ld2410cParser;
using Bytes = std::vector<uint8_t>;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
  std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #condition); \
} while (false)

// Verbatim protocol examples from V1.09 p.20. These are OFFLINE fixtures,
// not measurements from the user's radar. Do not report them as real readings.
const Bytes kNormal = {
  0xF4, 0xF3, 0xF2, 0xF1, 0x0D, 0x00, 0x02, 0xAA,
  0x02, 0x51, 0x00, 0x00, 0x00, 0x00, 0x3B, 0x00,
  0x00, 0x55, 0x00, 0xF8, 0xF7, 0xF6, 0xF5,
};
const Bytes kEngineering = {
  0xF4, 0xF3, 0xF2, 0xF1, 0x23, 0x00,
  0x01, 0xAA, 0x03, 0x1E, 0x00, 0x3C, 0x00, 0x00, 0x39, 0x00, 0x00,
  0x08, 0x08, 0x3C, 0x22, 0x05, 0x03, 0x03, 0x04, 0x03, 0x06, 0x05,
  0x00, 0x00, 0x39, 0x10, 0x13, 0x06, 0x06, 0x08, 0x04,
  0x60, 0x01, 0x55, 0x00, 0xF8, 0xF7, 0xF6, 0xF5,
};

unsigned feed(Ld2410cParser& parser, const Bytes& bytes, uint32_t at = 1) {
  unsigned updated = 0;
  for (uint8_t byte : bytes) if (parser.feed(byte, at)) ++updated;
  return updated;
}

void officialExamplesAndLittleEndian() {
  Ld2410cParser parser;
  CHECK(!parser.hasSample());
  CHECK(!parser.isFresh(0));
  CHECK(feed(parser, kNormal, 10) == 1);
  CHECK(parser.hasSample());
  CHECK(parser.latest().dataType == 2);
  CHECK(parser.latest().targetState == 2);
  CHECK(parser.latest().presenceKnown && parser.latest().presence);
  CHECK(!parser.latest().moving && parser.latest().stationary);
  CHECK(parser.latest().movingDistanceCm == 81);
  CHECK(parser.latest().movingEnergy == 0);
  CHECK(parser.latest().stillDistanceCm == 0);
  CHECK(parser.latest().stillEnergy == 59);
  CHECK(parser.latest().detectionDistanceCm == 0);
  CHECK(parser.latest().receivedAtMs == 10);
  CHECK(feed(parser, kEngineering, 20) == 1);
  CHECK(parser.latest().dataType == 1);
  CHECK(parser.latest().moving && parser.latest().stationary);
  CHECK(parser.latest().movingDistanceCm == 30);
  CHECK(parser.latest().movingEnergy == 60);
  CHECK(parser.latest().stillEnergy == 57);
  CHECK(parser.counters().validFrames == 2);
  CHECK(parser.counters().invalidFrames == 0);
  CHECK(parser.counters().discardedBytes == 0);
  CHECK(parser.counters().receivedBytes == 68);

  Bytes larger = kNormal;
  larger[9] = 0x34; larger[10] = 0x12;
  larger[12] = 0xCD; larger[13] = 0xAB;
  larger[15] = 0x78; larger[16] = 0x56;
  CHECK(feed(parser, larger, 30) == 1);
  CHECK(parser.latest().movingDistanceCm == 0x1234);
  CHECK(parser.latest().stillDistanceCm == 0xABCD);
  CHECK(parser.latest().detectionDistanceCm == 0x5678);
}

void everyFragmentBoundaryAndConcatenation() {
  for (size_t split = 1; split < kNormal.size(); ++split) {
    Ld2410cParser parser;
    for (size_t i = 0; i < split; ++i) CHECK(!parser.feed(kNormal[i], 50));
    CHECK(!parser.hasSample());
    for (size_t i = split; i < kNormal.size(); ++i) {
      CHECK(parser.feed(kNormal[i], 70) == (i == kNormal.size() - 1));
    }
    CHECK(parser.latest().receivedAtMs == 70);
    CHECK(parser.counters().validFrames == 1);
  }
  Ld2410cParser parser;
  Bytes joined = kNormal;
  joined.insert(joined.end(), kEngineering.begin(), kEngineering.end());
  joined.insert(joined.end(), kNormal.begin(), kNormal.end());
  CHECK(feed(parser, joined) == 3);
  CHECK(parser.counters().validFrames == 3);
  CHECK(parser.counters().discardedBytes == 0);
}

void targetStatesAndFreshness() {
  Ld2410cParser parser;
  Bytes packet = kNormal;
  for (uint8_t state = 0; state <= 6; ++state) {
    packet[8] = state;
    CHECK(feed(parser, packet, 100 + state) == 1);
    const auto& sample = parser.latest();
    CHECK(sample.targetState == state);
    CHECK(sample.presenceKnown == (state <= 3));
    CHECK(sample.presence == (state >= 1 && state <= 3));
    CHECK(sample.moving == (state == 1 || state == 3));
    CHECK(sample.stationary == (state == 2 || state == 3));
  }
  CHECK(parser.isFresh(2106));
  CHECK(!parser.isFresh(2107));
  CHECK(parser.isFresh(106, 0));
  CHECK(!parser.isFresh(107, 0));
  packet[8] = 7;
  CHECK(feed(parser, packet, 200) == 0);
  CHECK(parser.counters().unsupportedFrames == 1);
  CHECK(parser.latest().receivedAtMs == 106);
  CHECK(parser.counters().validFrames == 7);
}

void noiseAndOverlappingHeaderPrefixes() {
  Ld2410cParser parser;
  const Bytes noise = {0x00, 0xF4, 0xF3, 0xF4, 0xF3, 0xF2, 0xAA, 0xF4};
  CHECK(feed(parser, noise) == 0);
  CHECK(feed(parser, kNormal) == 1);
  CHECK(parser.counters().discardedBytes == noise.size());
  CHECK(parser.counters().invalidFrames == 0);
  CHECK(parser.counters().receivedBytes == noise.size() + kNormal.size());
}

void invalidLengthAndRecovery() {
  for (uint16_t length : {0u, 1u, 12u, 14u, 64u, 65u, 65535u}) {
    Ld2410cParser parser;
    Bytes bad = kNormal;
    bad[4] = static_cast<uint8_t>(length);
    bad[5] = static_cast<uint8_t>(length >> 8);
    CHECK(feed(parser, bad) == 0);
    CHECK(feed(parser, kNormal) == 1);
    CHECK(parser.counters().invalidFrames == 1);
    CHECK(parser.counters().discardedBytes == bad.size());
    CHECK(parser.counters().validFrames == 1);
  }
}

void malformedMarkersAndFooter() {
  for (size_t index : {7u, 17u, 18u, 19u, 20u, 21u, 22u}) {
    Ld2410cParser parser;
    Bytes bad = kNormal;
    bad[index] ^= 1;
    CHECK(feed(parser, bad) == 0);
    CHECK(!parser.hasSample());
    CHECK(feed(parser, kNormal) == 1);
    CHECK(parser.counters().invalidFrames == 1);
    CHECK(parser.counters().discardedBytes == bad.size());
  }
  // The next complete frame starts before a corrupt candidate's claimed end.
  Ld2410cParser parser;
  Bytes truncated(kNormal.begin(), kNormal.begin() + 11);
  truncated.insert(truncated.end(), kNormal.begin(), kNormal.end());
  CHECK(feed(parser, truncated) == 1);
  CHECK(parser.counters().invalidFrames == 1);
  CHECK(parser.counters().discardedBytes == 11);
}

void timeoutAndClockWraparound() {
  Ld2410cParser parser;
  const Bytes truncated(kNormal.begin(), kNormal.begin() + 12);
  CHECK(feed(parser, truncated, 100) == 0);
  parser.expire(199);
  CHECK(parser.counters().invalidFrames == 0);
  parser.expire(200);
  CHECK(parser.counters().invalidFrames == 1);
  CHECK(parser.counters().timedOutFrames == 1);
  parser.expire(1000);
  CHECK(parser.counters().invalidFrames == 1);
  CHECK(feed(parser, kNormal, 1000) == 1);
  CHECK(parser.counters().discardedBytes == truncated.size());

  Ld2410cParser wrap;
  const uint32_t nearWrap = std::numeric_limits<uint32_t>::max() - 49;
  CHECK(feed(wrap, truncated, nearWrap) == 0);
  wrap.expire(49);  // 99 ms elapsed.
  CHECK(wrap.counters().timedOutFrames == 0);
  // feed() itself expires a 100 ms old fragment before appending fresh bytes.
  CHECK(feed(wrap, kNormal, 50) == 1);
  CHECK(wrap.counters().timedOutFrames == 1);
  CHECK(feed(wrap, kNormal, std::numeric_limits<uint32_t>::max() - 10) == 1);
  CHECK(wrap.isFresh(89, 100));
  CHECK(!wrap.isFresh(90, 100));

  Ld2410cParser prefix;
  CHECK(feed(prefix, {0xF4, 0xF3}, 1) == 0);
  prefix.expire(101);
  CHECK(prefix.counters().discardedBytes == 2);
  CHECK(prefix.counters().invalidFrames == 0);
  CHECK(feed(prefix, kNormal, 102) == 1);
}

void unsupportedEnvelopesAndEngineeringShape() {
  Ld2410cParser parser;
  CHECK(feed(parser, kNormal, 1) == 1);
  Bytes unknown = kNormal;
  unknown[6] = 0x7F;
  CHECK(feed(parser, unknown, 2) == 0);
  Bytes differentShape = kEngineering;
  differentShape[17] = 7;  // payload[11], requires one fewer energy byte.
  CHECK(feed(parser, differentShape, 3) == 0);
  differentShape[17] = 9;  // Gate indices beyond the documented limit.
  CHECK(feed(parser, differentShape, 4) == 0);
  Bytes shortEngineering = kNormal;
  shortEngineering[6] = 1;
  CHECK(feed(parser, shortEngineering, 5) == 0);
  CHECK(parser.latest().receivedAtMs == 1);
  CHECK(parser.counters().unsupportedFrames == 4);
  CHECK(parser.counters().unsupportedBytes == 23 + 45 + 45 + 23);
  CHECK(parser.counters().invalidFrames == 0);
  CHECK(feed(parser, kEngineering, 6) == 1);

  // Table 15 permits a smaller energy-array shape; verify derived length.
  Bytes smaller(kEngineering.begin(), kEngineering.begin() + 17);
  smaller[4] = 19;
  smaller.insert(smaller.end(), {0, 0, 10, 20, 0x60, 1, 0x55, 0,
                                 0xF8, 0xF7, 0xF6, 0xF5});
  CHECK(feed(parser, smaller, 7) == 1);
  CHECK(parser.counters().validFrames == 3);

  // At the bounded maximum length an unknown envelope is consumed whole.
  Bytes maximum = {0xF4, 0xF3, 0xF2, 0xF1, 64, 0, 0x7F, 0xAA};
  maximum.resize(68, 0);
  maximum.insert(maximum.end(), {0x55, 0, 0xF8, 0xF7, 0xF6, 0xF5});
  CHECK(feed(parser, maximum, 8) == 0);
  CHECK(parser.counters().unsupportedFrames == 5);
  CHECK(feed(parser, kNormal, 9) == 1);
}

void boundedNoiseStressAndLastSampleProtection() {
  Ld2410cParser parser;
  uint32_t random = 0x194C2026;
  for (uint32_t round = 0; round < 10000; ++round) {
    for (uint32_t i = 0; i < 100; ++i) {
      random ^= random << 13;
      random ^= random >> 17;
      random ^= random << 5;
      parser.feed(static_cast<uint8_t>(random), round * 101);
    }
    // Let any accidental header expire, then check deterministic recovery.
    parser.expire(round * 101 + 100);
    CHECK(feed(parser, kNormal, round * 101 + 100) == 1);
  }
  CHECK(parser.counters().validFrames == 10000);
  CHECK(parser.counters().receivedBytes == 1230000);
  CHECK(parser.counters().discardedBytes == 1000000);
  CHECK(parser.latest().stillEnergy == 59);
  CHECK(!parser.isFresh(2000000));
  CHECK(feed(parser, {0xAA, 0x55, 0, 0xFF}, 2000001) == 0);
  CHECK(!parser.isFresh(2000001));
  CHECK(parser.counters().validFrames == 10000);
}
}  // namespace

int main() {
  try {
    officialExamplesAndLittleEndian();
    everyFragmentBoundaryAndConcatenation();
    targetStatesAndFreshness();
    noiseAndOverlappingHeaderPrefixes();
    invalidLengthAndRecovery();
    malformedMarkersAndFooter();
    timeoutAndClockWraparound();
    unsupportedEnvelopesAndEngineeringShape();
    boundedNoiseStressAndLastSampleProtection();
    std::cout << "PASS: 9 offline LD2410C parser test groups; physical sensor NOT_TESTED\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
