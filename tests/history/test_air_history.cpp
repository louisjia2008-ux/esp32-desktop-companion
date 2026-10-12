#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <assert.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>
#include "sensors/air_history.h"
#include "storage/air_history.h"

namespace storage {
bool fakeMounted = true;
bool sdCardMounted() { return fakeMounted; }
}
namespace {
using namespace sensors;
constexpr uint32_t kEpoch = 1791763200u;  // 2026-10-12 00:00:00 UTC.
HistoryClock clockAt(uint32_t epoch = kEpoch, int32_t offset = 0) {
  HistoryClock c; c.valid = true; c.utcEpochSeconds = epoch; c.utcOffsetSeconds = offset; return c;
}
AirSnapshot airAt(uint32_t now, uint32_t samples = 1, uint16_t co2 = 800) {
  AirSnapshot air; air.health = Health::Healthy; air.hasSample = true;
  air.sampledAtMs = now; air.validSamples = samples; air.co2Ppm = co2;
  air.temperatureC = 24.5f; air.humidityRh = 50.25f; return air;
}
AirHistoryRecord recordAt(uint16_t minute, uint16_t co2 = 800) {
  AirHistoryRecord r; r.dayOrdinal = int32_t(kEpoch / 86400);
  r.utcEpochSeconds = kEpoch + uint32_t(minute) * 60;
  r.minuteOfDay = minute; r.sampleCount = 12; r.co2Ppm = co2;
  r.temperatureCentiC = 2450; r.humidityCentiRh = 5025;
  r.discontinuity = false; return r;
}
std::string slotPath(int32_t day) {
  return SD_MMC.root + "/sensor-history/day" + std::to_string(day % 7) + ".ah1";
}
void append(const std::string& path, const uint8_t* bytes, size_t count) {
  std::ofstream f(path, std::ios::binary | std::ios::app);
  f.write(reinterpret_cast<const char*>(bytes), std::streamsize(count));
}
std::vector<uint8_t> readAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}
void seed(int32_t day, unsigned records = 0) {
  std::filesystem::create_directory(SD_MMC.root + "/sensor-history");
  uint8_t bytes[32]; encodeAirHistoryHeader(day, bytes);
  std::ofstream f(slotPath(day), std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<char*>(bytes), 32); f.close();
  for (unsigned i = 0; i < records; ++i) {
    auto r = recordAt(uint16_t(i % 1440)); r.dayOrdinal = day;
    r.utcEpochSeconds = uint32_t(day) * 86400 + uint32_t(r.minuteOfDay) * 60;
    encodeAirHistoryRecord(r, bytes); append(slotPath(day), bytes, 32);
  }
}
void poll(storage::AirHistory& history, uint32_t epoch, const AirSnapshot& air = {},
          bool io = true, uint32_t now = UINT32_MAX) {
  if (now == UINT32_MAX) now = (epoch - kEpoch) * 1000;
  fakeMillis = now;
  SensorSnapshot snapshot; snapshot.air = air;
  ioBytes = 0;
  history.poll(now, snapshot, clockAt(epoch), io);
  assert(ioBytes <= kAirHistoryIoBytes);
}
void settle(storage::AirHistory& h, uint32_t epoch = kEpoch, unsigned polls = 500) {
  for (unsigned i = 0; i < polls; ++i) poll(h, epoch);
}
void writeMinute(storage::AirHistory& h, uint32_t epoch, uint32_t sample = 1) {
  poll(h, epoch, airAt((epoch - kEpoch) * 1000, sample));
  poll(h, epoch + 60);
  settle(h, epoch + 60, 12);
}
void resetFs() {
  char path[] = "/tmp/air-history-unit-XXXXXX";
  assert(mkdtemp(path)); SD_MMC.root = path;
}
void codec() {
  uint8_t bytes[32]; int32_t day = 0;
  encodeAirHistoryHeader(int32_t(kEpoch / 86400), bytes);
  assert(decodeAirHistoryHeader(bytes, day)); assert(day == int32_t(kEpoch / 86400));
  bytes[19] ^= 1; assert(!decodeAirHistoryHeader(bytes, day));
  auto r = recordAt(1439, 1900); r.discontinuity = true;
  encodeAirHistoryRecord(r, bytes); AirHistoryRecord got;
  assert(decodeAirHistoryRecord(bytes, r.dayOrdinal, got));
  assert(got.co2Ppm == 1900 && got.minuteOfDay == 1439 && got.discontinuity);
  for (unsigned at = 0; at < 32; ++at) {
    bytes[at] ^= 0x40; assert(!decodeAirHistoryRecord(bytes, r.dayOrdinal, got)); bytes[at] ^= 0x40;
  }
  r.minuteOfDay = 1440; encodeAirHistoryRecord(r, bytes);
  assert(!decodeAirHistoryRecord(bytes, r.dayOrdinal, got));
  r = recordAt(0); r.sampleCount = 0; encodeAirHistoryRecord(r, bytes);
  assert(!decodeAirHistoryRecord(bytes, r.dayOrdinal, got));
}
void calendar() {
  int32_t day; uint16_t minute, year; uint8_t month, dom;
  assert(historyDay(clockAt(), day, minute)); assert(minute == 0);
  historyCivilDate(day, year, month, dom); assert(year == 2026 && month == 10 && dom == 12);
  assert(historyDay(clockAt(kEpoch, 28800), day, minute)); assert(minute == 480);
  assert(historyDay(clockAt(1709164800), day, minute));
  historyCivilDate(day, year, month, dom); assert(year == 2024 && month == 2 && dom == 29);
  auto c = clockAt(); c.valid = false; assert(!historyDay(c, day, minute));
  assert(!historyDay(clockAt(0), day, minute));
  assert(!historyDay(clockAt(kEpoch, 60000), day, minute));
}
void aggregate() {
  AirHistoryAccumulator a; AirHistoryCounters count;
  a.observe(0, airAt(0, 1, 800), clockAt(), count);
  a.observe(5000, airAt(5000, 2, 1000), clockAt(kEpoch + 5), count);
  a.observe(5000, airAt(5000, 2, 1000), clockAt(kEpoch + 5), count);
  a.observe(60000, {}, clockAt(kEpoch + 60), count);
  AirHistoryRecord r; assert(a.pop(r)); assert(!a.pop(r));
  assert(r.sampleCount == 2 && r.co2Ppm == 900 && r.temperatureCentiC == 2450);
  assert(count.acceptedSamples == 2 && count.recordedMinutes == 1);
}
void freshness() {
  AirHistoryAccumulator a; AirHistoryCounters count;
  auto stale = airAt(0); a.observe(16000, stale, clockAt(kEpoch + 16), count);
  assert(count.acceptedSamples == 0);
  a.observe(20000, airAt(20000, 2), clockAt(kEpoch + 20), count);
  a.observe(40000, {}, clockAt(kEpoch + 40), count);
  a.observe(45000, airAt(45000, 3), clockAt(kEpoch + 45), count);
  a.observe(60000, {}, clockAt(kEpoch + 60), count);
  AirHistoryRecord r; assert(a.pop(r)); assert(r.discontinuity && r.sampleCount == 2);
}
void pollGap() {
  AirHistoryAccumulator a; AirHistoryCounters count;
  a.observe(0, airAt(0, 1), clockAt(), count);
  a.observe(30000, airAt(30000, 7), clockAt(kEpoch + 30), count);
  a.observe(60000, {}, clockAt(kEpoch + 60), count);
  AirHistoryRecord r; assert(a.pop(r));
  assert(r.sampleCount == 2 && r.discontinuity);
}
void clockChanges() {
  AirHistoryAccumulator a; AirHistoryCounters count;
  a.observe(UINT32_MAX - 10000, airAt(UINT32_MAX - 10000), clockAt(kEpoch + 50), count);
  a.observe(4999, airAt(4999, 2), clockAt(kEpoch + 65), count);
  a.observe(5999, airAt(5999, 3), clockAt(kEpoch + 30), count);
  a.observe(6999, airAt(6999, 4), clockAt(kEpoch + 31, 3600), count);
  assert(count.clockDiscontinuities == 2);
  AirHistoryRecord r; assert(a.pop(r)); assert(r.minuteOfDay == 0);
  assert(a.pop(r)); assert(r.minuteOfDay == 1);
  assert(a.pop(r)); assert(r.discontinuity);
}
void midnight() {
  AirHistoryAccumulator a; AirHistoryCounters count;
  a.observe(0, airAt(0), clockAt(kEpoch + 86398), count);
  a.observe(5000, airAt(5000, 2), clockAt(kEpoch + 86403), count);
  a.observe(65000, {}, clockAt(kEpoch + 86463), count);
  AirHistoryRecord before, after; assert(a.pop(before) && a.pop(after));
  assert(before.minuteOfDay == 1439 && after.minuteOfDay == 0);
  assert(after.dayOrdinal == before.dayOrdinal + 1);
}
void queueBound() {
  AirHistoryAccumulator a; AirHistoryCounters count;
  for (uint32_t m = 0; m < 25; ++m)
    a.observe(m * 60000, airAt(m * 60000, m + 1), clockAt(kEpoch + m * 60), count);
  assert(count.droppedMinutes == 8); AirHistoryRecord r; unsigned n = 0;
  while (a.pop(r)) { if (n == 0) assert(r.discontinuity); ++n; }
  assert(n == 16);
}
void plotGaps() {
  AirHistoryBucket buckets[kAirHistoryBuckets]; AirHistoryPlot p; p.reset(buckets);
  for (uint16_t m = 0; m < 10; ++m) p.add(recordAt(m));
  assert(buckets[0].minuteCount == 5 && buckets[1].connectFromPrevious);
  p.gap(); p.add(recordAt(10)); assert(!buckets[2].connectFromPrevious);
  p.add(recordAt(12)); assert(buckets[2].discontinuous);
  p.add(recordAt(15)); assert(!buckets[3].connectFromPrevious);
  assert(!buckets[200].valid);
}
void plotDuplicates() {
  AirHistoryBucket buckets[kAirHistoryBuckets]; AirHistoryPlot p; p.reset(buckets);
  p.add(recordAt(42, 800)); p.add(recordAt(42, 1600));
  assert(p.validMinutes() == 1 && buckets[8].minuteCount == 1 && buckets[8].co2Ppm == 800);
  p.add(recordAt(43)); p.add(recordAt(44)); p.add(recordAt(45));
  assert(!buckets[9].connectFromPrevious);
  p.add(recordAt(1)); p.add(recordAt(42));
  assert(p.validMinutes() == 5 && buckets[8].minuteCount == 3);
}
void roundtrip() {
  { storage::AirHistory h; h.begin(); settle(h); writeMinute(h, kEpoch);
    assert(h.counters().writtenMinutes == 1 && h.view().validMinutes == 1);
    assert(h.view().buckets[0].co2Ppm == 800); }
  storage::AirHistory reboot; reboot.begin(); settle(reboot, kEpoch + 120);
  assert(reboot.view().state == HistoryState::Ready && reboot.view().validMinutes == 1);
  assert(reboot.view().buckets[0].humidityCentiRh == 5025);
}
void retention() {
  storage::AirHistory h; h.begin(); settle(h);
  for (uint32_t d = 0; d < 9; ++d) writeMinute(h, kEpoch + d * 86400, d + 1);
  assert(h.counters().writtenMinutes == 9);
  unsigned files = 0; for (auto& f : std::filesystem::directory_iterator(SD_MMC.root + "/sensor-history"))
    if (f.is_regular_file()) ++files;
  assert(files == 7); h.requestDay(6); settle(h, kEpoch + 8 * 86400 + 120);
  assert(h.view().validMinutes == 1 && h.view().daysAgo == 6);
  assert(h.view().dayOrdinal == int32_t(kEpoch / 86400) + 2);
}
void protectedFile(bool truncated) {
  const int32_t day = int32_t(kEpoch / 86400);
  std::filesystem::create_directory(SD_MMC.root + "/sensor-history");
  const std::string path = slotPath(day);
  if (truncated) { uint8_t header[32]; encodeAirHistoryHeader(day, header); append(path, header, 13); }
  else { const uint8_t unknown[] = {1,2,3,4}; append(path, unknown, sizeof(unknown)); }
  const auto before = readAll(path);
  storage::AirHistory h; h.begin(); settle(h); writeMinute(h, kEpoch);
  assert(readAll(path) == before && h.counters().protectedFiles == 1);
  assert(h.counters().writtenMinutes == 0 && h.counters().droppedMinutes == 1);
  assert(h.view().state == HistoryState::StorageError);
}
void tornTail() {
  const int32_t day = int32_t(kEpoch / 86400); seed(day, 1);
  const uint8_t torn[] = {1,2,3,4,5,6,7}; append(slotPath(day), torn, sizeof(torn));
  storage::AirHistory h; h.begin(); settle(h, kEpoch + 60); writeMinute(h, kEpoch + 60, 2);
  assert(h.counters().writtenMinutes == 1 && h.counters().tornTails >= 1);
  assert(std::filesystem::file_size(slotPath(day)) == 128);
  storage::AirHistory reboot; reboot.begin(); settle(reboot, kEpoch + 180);
  assert(reboot.view().validMinutes == 2 && reboot.counters().invalidRecords == 1);
  assert(reboot.view().buckets[0].discontinuous);
}
void corruptRecord() {
  const int32_t day = int32_t(kEpoch / 86400); seed(day, 12);
  auto bytes = readAll(slotPath(day)); bytes[32 + 5 * 32 + 12] ^= 1;
  { std::ofstream f(slotPath(day), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())); }
  storage::AirHistory h; h.begin(); settle(h, kEpoch + 900);
  assert(h.view().validMinutes == 11 && h.counters().invalidRecords == 1);
  assert(!h.view().buckets[1].connectFromPrevious);
  assert(readAll(slotPath(day)) == bytes);
}
void deferredIo() {
  storage::AirHistory h; h.begin(); settle(h);
  for (uint32_t m = 0; m < 20; ++m) {
    poll(h, kEpoch + m * 60, airAt(m * 60000, m + 1), false);
    assert(ioBytes == 0);
  }
  assert(h.counters().writtenMinutes == 0 && h.counters().droppedMinutes == 3);
  settle(h, kEpoch + 20 * 60);
  assert(h.counters().writtenMinutes == 16);
}
void writeRetry() {
  storage::AirHistory h; h.begin(); settle(h); seed(int32_t(kEpoch / 86400));
  poll(h, kEpoch, airAt(0)); poll(h, kEpoch + 60);
  failWrites = true; settle(h, kEpoch + 60, 5);
  assert(h.counters().ioErrors > 0 && h.counters().writtenMinutes == 0);
  failWrites = false; settle(h, kEpoch + 91);
  assert(h.counters().writtenMinutes == 1);
}
void readRetry() {
  seed(int32_t(kEpoch / 86400), 20); storage::AirHistory h; h.begin();
  for (unsigned i = 0; i < 8; ++i) poll(h, kEpoch);
  failReads = true; poll(h, kEpoch); assert(h.counters().ioErrors == 1);
  failReads = false; settle(h, kEpoch + 31);
  assert(h.view().validMinutes == 20);
}
void readbackRetry() {
  storage::AirHistory h; h.begin(); settle(h);
  poll(h, kEpoch, airAt(0)); poll(h, kEpoch + 60);
  poll(h, kEpoch + 60);  // prepare header
  failReadbackOnce = true; poll(h, kEpoch + 60);
  const std::string path = slotPath(int32_t(kEpoch / 86400));
  assert(std::filesystem::file_size(path) == 64 && h.counters().writtenMinutes == 0);
  settle(h, kEpoch + 91);
  assert(h.counters().writtenMinutes == 1 && std::filesystem::file_size(path) == 64);
}
void directoryRetry() {
  SD_MMC.failMkdir = true; storage::AirHistory h; h.begin(); settle(h);
  assert(h.view().state == HistoryState::StorageError);
  SD_MMC.failMkdir = false; settle(h, kEpoch + 31);
  assert(h.view().state == HistoryState::Ready);
  writeMinute(h, kEpoch + 32); assert(h.counters().writtenMinutes == 1);
}
void rollbackCollision() {
  const int32_t day = int32_t(kEpoch / 86400); seed(day + 7, 1);
  const auto before = readAll(slotPath(day));
  storage::AirHistory h; h.begin(); settle(h); writeMinute(h, kEpoch);
  assert(readAll(slotPath(day)) == before && h.counters().droppedMinutes == 1);
  assert(h.view().state == HistoryState::StorageError);
}
void torn31() {
  const int32_t day = int32_t(kEpoch / 86400); seed(day);
  uint8_t bytes[32]; auto r = recordAt(42, 454);
  r.utcEpochSeconds = 1791734400u; r.temperatureCentiC = 2400;
  r.humidityCentiRh = 4500; r.uptimeMs = 60000;
  encodeAirHistoryRecord(r, bytes);
  // Fixture whose final CRC byte is zero; zero padding would accidentally
  // promote the uncommitted 31-byte tail into an apparently valid record.
  assert(bytes[31] == 0); append(slotPath(day), bytes, 31);
  storage::AirHistory h; h.begin(); settle(h, kEpoch + 43 * 60);
  writeMinute(h, kEpoch + 43 * 60);
  storage::AirHistory reboot; reboot.begin(); settle(reboot, kEpoch + 45 * 60);
  assert(reboot.view().validMinutes == 1 && reboot.counters().invalidRecords == 1);
  assert(!reboot.view().buckets[8].valid || reboot.view().buckets[8].minuteCount == 1);
}
void partialWriteRetry() {
  seed(int32_t(kEpoch / 86400)); storage::AirHistory h; h.begin(); settle(h);
  poll(h, kEpoch, airAt(0)); poll(h, kEpoch + 60); poll(h, kEpoch + 60);
  partialWriteBytes = 13; poll(h, kEpoch + 60);
  assert(h.counters().writtenMinutes == 0 && h.counters().ioErrors == 1);
  partialWriteBytes = SIZE_MAX; settle(h, kEpoch + 91);
  assert(h.counters().writtenMinutes == 1);
  storage::AirHistory reboot; reboot.begin(); settle(reboot, kEpoch + 120);
  assert(reboot.view().validMinutes == 1 && reboot.counters().invalidRecords == 1);
}
void dateRequestDuringLoad() {
  const int32_t day = int32_t(kEpoch / 86400); seed(day, 1000); seed(day - 1, 12);
  storage::AirHistory h; h.begin(); settle(h, kEpoch, 10);
  assert(h.view().state == HistoryState::Loading);
  h.requestDay(1); settle(h);
  assert(h.view().dayOrdinal == day - 1 && h.view().validMinutes == 12);
}
void allocations(bool all) {
  failPsram = true; failAllAllocations = all;
  storage::AirHistory h; h.begin(); settle(h); writeMinute(h, kEpoch);
  if (all) { assert(!h.view().buckets && h.view().state == HistoryState::MemoryUnavailable); }
  else { assert(h.view().buckets && h.view().validMinutes == 1); }
  assert(h.counters().writtenMinutes == 1);
}
void noClock() {
  storage::AirHistory h; h.begin(); SensorSnapshot s; s.air = airAt(0);
  HistoryClock invalid; h.poll(0, s, invalid, true);
  assert(h.view().state == HistoryState::WaitingForClock && h.counters().acceptedSamples == 0);
  settle(h); writeMinute(h, kEpoch); assert(h.counters().writtenMinutes == 1);
}
void noSd() {
  storage::fakeMounted = false; storage::AirHistory h; h.begin();
  settle(h); writeMinute(h, kEpoch);
  assert(h.view().state == HistoryState::SdUnavailable && h.counters().writtenMinutes == 0);
}
void recordLimit() {
  seed(int32_t(kEpoch / 86400), kAirHistoryMaxRecords);
  const auto before = readAll(slotPath(int32_t(kEpoch / 86400)));
  storage::AirHistory h; h.begin(); settle(h); writeMinute(h, kEpoch + 23 * 3600);
  assert(h.counters().droppedMinutes == 1 && h.counters().writtenMinutes == 0);
  assert(readAll(slotPath(int32_t(kEpoch / 86400))) == before);
}
void rebootMinute() {
  seed(int32_t(kEpoch / 86400), 43);
  storage::AirHistory h; h.begin(); settle(h, kEpoch + 42 * 60);
  writeMinute(h, kEpoch + 42 * 60);
  assert(h.counters().writtenMinutes == 1 && h.view().validMinutes == 43);
  assert(h.view().buckets[8].minuteCount == 3 && h.view().buckets[8].discontinuous);
}
}
int main(int argc, char** argv) {
  assert(argc == 2); resetFs(); const std::string s = argv[1];
  if (s == "codec") codec(); else if (s == "calendar") calendar();
  else if (s == "aggregate") aggregate(); else if (s == "freshness") freshness();
  else if (s == "clock_changes") clockChanges(); else if (s == "midnight") midnight();
  else if (s == "queue_bound") queueBound(); else if (s == "plot_gaps") plotGaps();
  else if (s == "plot_duplicates") plotDuplicates(); else if (s == "roundtrip") roundtrip();
  else if (s == "retention") retention(); else if (s == "protected_file") protectedFile(false);
  else if (s == "truncated_header") protectedFile(true); else if (s == "torn_tail") tornTail();
  else if (s == "corrupt_record") corruptRecord(); else if (s == "deferred_io") deferredIo();
  else if (s == "write_retry") writeRetry(); else if (s == "read_retry") readRetry();
  else if (s == "allocation_fallback") allocations(false);
  else if (s == "allocation_failure") allocations(true); else if (s == "no_clock") noClock();
  else if (s == "sd_unavailable") noSd(); else if (s == "record_limit") recordLimit();
  else if (s == "reboot_minute") rebootMinute();
  else if (s == "readback_retry") readbackRetry();
  else if (s == "directory_retry") directoryRetry();
  else if (s == "rollback_collision") rollbackCollision();
  else if (s == "torn31") torn31();
  else if (s == "partial_write_retry") partialWriteRetry();
  else if (s == "date_request_during_load") dateRequestDuringLoad();
  else if (s == "poll_gap") pollGap();
  else assert(false);
  std::filesystem::remove_all(SD_MMC.root); std::cout << s << ": PASS\n";
}
