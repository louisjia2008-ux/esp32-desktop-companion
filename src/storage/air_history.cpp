#include "air_history.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

#include "sd_card.h"

namespace storage {
namespace {
constexpr int32_t kOpenError = -2001;
constexpr int32_t kReadError = -2002;
constexpr int32_t kWriteError = -2003;
constexpr int32_t kProtectedFile = -2004;
constexpr int32_t kFileLimit = -2005;
constexpr int32_t kDirectoryError = -2006;
constexpr uint32_t kRetryMs = 30000;
constexpr uint32_t kMaxFileBytes = sensors::kAirHistoryHeaderBytes +
    uint32_t(sensors::kAirHistoryMaxRecords) * sensors::kAirHistoryRecordBytes;
const char kDirectory[] = "/sensor-history";
}

AirHistory::~AirHistory() {
  loadFile_.close();
  if (buckets_) heap_caps_free(buckets_);
}

void AirHistory::slotPath(uint8_t slot, char* output) {
  snprintf(output, 40, "/sensor-history/day%u.ah1", unsigned(slot));
}

void AirHistory::begin() {
  if (initialized_) return;
  initialized_ = true;
  const uint32_t started = millis();
  buckets_ = static_cast<sensors::AirHistoryBucket*>(heap_caps_malloc(
      sizeof(sensors::AirHistoryBucket) * sensors::kAirHistoryBuckets,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buckets_) buckets_ = static_cast<sensors::AirHistoryBucket*>(heap_caps_malloc(
      sizeof(sensors::AirHistoryBucket) * sensors::kAirHistoryBuckets,
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  view_.buckets = buckets_;
  plot_.reset(buckets_);
  // SD mounting remains the existing shared SD owner's responsibility. No
  // format, re-mount, partition change or card reset is attempted here.
  mounted_ = sdCardMounted();
  if (mounted_) {
    directoryReady_ = SD_MMC.exists(kDirectory) || SD_MMC.mkdir(kDirectory);
    if (!directoryReady_) fail(kDirectoryError);
  }
  counters_.maxIoMs = uint32_t(millis() - started);
  setSettledState();
}

void AirHistory::requestDay(uint8_t daysAgo) {
  if (daysAgo > 6) daysAgo = 6;
  if (view_.daysAgo == daysAgo && !loadRequested_ &&
      view_.state != sensors::HistoryState::StorageError) return;
  view_.daysAgo = daysAgo;
  loadRequested_ = true;
}

void AirHistory::refreshAvailableDays() {
  view_.availableDayMask = 0;
  if (view_.todayOrdinal < 0) return;
  for (uint8_t ago = 0; ago < 7; ++ago) {
    const int32_t day = view_.todayOrdinal - ago;
    const uint8_t slot = uint8_t(day % 7);
    if (!slotProtected_[slot] && slotDays_[slot] == day)
      view_.availableDayMask |= uint8_t(1u << ago);
  }
}

void AirHistory::updateDate(const sensors::HistoryClock& clock) {
  int32_t today = -1;
  uint16_t minute = 0;
  view_.clockValid = sensors::historyDay(clock, today, minute);
  if (!view_.clockValid) {
    view_.state = sensors::HistoryState::WaitingForClock;
    return;
  }
  if (today != view_.todayOrdinal) {
    view_.todayOrdinal = today;
    loadRequested_ = true;
  }
  const int32_t selected = today - view_.daysAgo;
  if (view_.dayOrdinal != selected) {
    view_.dayOrdinal = selected;
    sensors::historyCivilDate(selected, view_.year, view_.month, view_.day);
    loadRequested_ = true;
  }
  refreshAvailableDays();
}

void AirHistory::fail(int32_t code) {
  ++counters_.ioErrors;
  counters_.lastError = code;
  retryWaiting_ = true;
  lastRetryAt_ = millis();
  loadFile_.close();
  stage_ = scanSlot_ < 7 ? IoStage::ScanHeaders : IoStage::Idle;
  view_.state = sensors::HistoryState::StorageError;
  if (hasPending_ && !appendAttempted_) pending_.discontinuity = true;
}

void AirHistory::setSettledState() {
  if (!view_.clockValid) view_.state = sensors::HistoryState::WaitingForClock;
  else if (!mounted_) view_.state = sensors::HistoryState::SdUnavailable;
  else if (!directoryReady_ || retryWaiting_)
    view_.state = sensors::HistoryState::StorageError;
  else if (view_.dayOrdinal >= 0 &&
           (slotProtected_[uint8_t(view_.dayOrdinal % 7)] ||
            slotDays_[uint8_t(view_.dayOrdinal % 7)] > view_.dayOrdinal ||
            (slotDays_[uint8_t(view_.dayOrdinal % 7)] == view_.dayOrdinal &&
             slotFull_[uint8_t(view_.dayOrdinal % 7)])))
    view_.state = sensors::HistoryState::StorageError;
  else if (!buckets_) view_.state = sensors::HistoryState::MemoryUnavailable;
  else if (scanSlot_ < 7) view_.state = sensors::HistoryState::Starting;
  else if (stage_ == IoStage::LoadRecords || loadRequested_)
    view_.state = sensors::HistoryState::Loading;
  else view_.state = sensors::HistoryState::Ready;
}

void AirHistory::scanHeader() {
  char path[40];
  slotPath(scanSlot_, path);
  if (SD_MMC.exists(path)) {
    File file = SD_MMC.open(path, FILE_READ);
    uint8_t header[sensors::kAirHistoryHeaderBytes];
    int32_t day = -1;
    if (!file) { fail(kOpenError); return; }
    const uint32_t bytes = file.size();
    const size_t got = file.read(header, sizeof(header));
    file.close();
    if (got != sizeof(header) || !sensors::decodeAirHistoryHeader(header, day) ||
        uint8_t(day % 7) != scanSlot_ || bytes > kMaxFileBytes) {
      slotProtected_[scanSlot_] = true;
      ++counters_.protectedFiles;
      counters_.lastError = kProtectedFile;
    } else {
      slotDays_[scanSlot_] = day;
      slotFull_[scanSlot_] = bytes >= kMaxFileBytes;
    }
  }
  ++scanSlot_;
  if (scanSlot_ == 7) stage_ = IoStage::Idle;
  refreshAvailableDays();
}

void AirHistory::startLoad() {
  loadFile_.close();
  plot_.reset(buckets_);
  view_.validMinutes = 0;
  loadRequested_ = false;
  if (!buckets_) return;
  const uint8_t slot = uint8_t(view_.dayOrdinal % 7);
  if (slotProtected_[slot]) {
    view_.state = sensors::HistoryState::StorageError;
    counters_.lastError = kProtectedFile;
    return;
  }
  if (slotDays_[slot] != view_.dayOrdinal) return;
  char path[40];
  slotPath(slot, path);
  loadFile_ = SD_MMC.open(path, FILE_READ);
  if (!loadFile_) { loadRequested_ = true; fail(kOpenError); return; }
  uint8_t header[sensors::kAirHistoryHeaderBytes];
  int32_t day = -1;
  const uint32_t bytes = loadFile_.size();
  if (loadFile_.read(header, sizeof(header)) != sizeof(header) ||
      !sensors::decodeAirHistoryHeader(header, day) || day != view_.dayOrdinal ||
      bytes < sizeof(header) || bytes > kMaxFileBytes) {
    loadRequested_ = true;
    fail(kReadError);
    return;
  }
  loadRemaining_ = bytes - sizeof(header);
  stage_ = IoStage::LoadRecords;
}

void AirHistory::loadRecords() {
  if (loadRequested_) {
    loadFile_.close();
    stage_ = IoStage::Idle;
    return;
  }
  if (!loadRemaining_) {
    loadFile_.close();
    stage_ = IoStage::Idle;
    return;
  }
  uint8_t bytes[sensors::kAirHistoryIoBytes];
  const size_t requested = loadRemaining_ < sizeof(bytes) ? loadRemaining_ : sizeof(bytes);
  const size_t got = loadFile_.read(bytes, requested);
  if (got != requested) { loadRequested_ = true; fail(kReadError); return; }
  loadRemaining_ -= uint32_t(got);
  for (size_t at = 0; at + sensors::kAirHistoryRecordBytes <= got;
       at += sensors::kAirHistoryRecordBytes) {
    sensors::AirHistoryRecord record;
    if (sensors::decodeAirHistoryRecord(bytes + at, view_.dayOrdinal, record)) {
      plot_.add(record);
    } else {
      ++counters_.crcErrors;
      ++counters_.invalidRecords;
      plot_.gap();
    }
  }
  if (got % sensors::kAirHistoryRecordBytes) {
    ++counters_.tornTails;
    plot_.gap();
  }
  view_.validMinutes = plot_.validMinutes();
  if (!loadRemaining_) {
    loadFile_.close();
    stage_ = IoStage::Idle;
  }
}

void AirHistory::prepareWrite() {
  pendingSlot_ = uint8_t(pending_.dayOrdinal % 7);
  char path[40];
  slotPath(pendingSlot_, path);
  if (slotProtected_[pendingSlot_]) {
    ++counters_.droppedMinutes;
    hasPending_ = false;
    counters_.lastError = kProtectedFile;
    stage_ = IoStage::Idle;
    return;
  }
  bool create = !SD_MMC.exists(path);
  uint32_t bytes = 0;
  if (!create) {
    File file = SD_MMC.open(path, FILE_READ);
    if (!file) { fail(kOpenError); return; }
    uint8_t header[sensors::kAirHistoryHeaderBytes];
    int32_t day = -1;
    bytes = file.size();
    const size_t got = file.read(header, sizeof(header));
    file.close();
    if (got != sizeof(header) || !sensors::decodeAirHistoryHeader(header, day) ||
        uint8_t(day % 7) != pendingSlot_ || bytes > kMaxFileBytes) {
      slotProtected_[pendingSlot_] = true;
      ++counters_.protectedFiles;
      fail(kProtectedFile);
      return;
    }
    if (day != pending_.dayOrdinal) {
      // Only an identified module-owned archive at least seven days older may
      // rotate. Clock rollback and delayed queue records never erase newer data.
      if (pending_.dayOrdinal - day < 7) {
        ++counters_.droppedMinutes;
        hasPending_ = false;
        counters_.lastError = kProtectedFile;
        stage_ = IoStage::Idle;
        return;
      }
      create = true;
    } else if (appendAttempted_) {
      // A completed append followed by a failed readback is retried by checking
      // its original offset, never by blindly appending the same minute again.
      File previous = SD_MMC.open(path, FILE_READ);
      uint8_t readback[sensors::kAirHistoryRecordBytes];
      if (!previous) { fail(kOpenError); return; }
      if (bytes >= writeSize_ + sizeof(readback)) {
        if (!previous.seek(writeSize_) ||
            previous.read(readback, sizeof(readback)) != sizeof(readback)) {
          previous.close(); fail(kReadError); return;
        }
        if (memcmp(readback, attemptedRecord_, sizeof(readback)) == 0) {
          previous.close(); finishWrite(); return;
        }
        ++counters_.invalidRecords;
        ++counters_.crcErrors;
      }
      previous.close();
      appendAttempted_ = false;
      pending_.discontinuity = true;
    }
  }
  if (create) {
    File file = SD_MMC.open(path, FILE_WRITE);
    if (!file) { fail(kOpenError); return; }
    uint8_t header[sensors::kAirHistoryHeaderBytes];
    sensors::encodeAirHistoryHeader(pending_.dayOrdinal, header);
    const size_t written = file.write(header, sizeof(header));
    file.flush();
    file.close();
    if (written != sizeof(header)) {
      // An interrupted header cannot be safely identified on a later boot.
      // Leave it in place; this slot stays protected rather than being erased.
      slotProtected_[pendingSlot_] = true;
      ++counters_.protectedFiles;
      fail(kWriteError);
      return;
    }
    slotDays_[pendingSlot_] = pending_.dayOrdinal;
    slotFull_[pendingSlot_] = false;
    bytes = sizeof(header);
    pending_.discontinuity = true;
    refreshAvailableDays();
  }
  if (bytes + sensors::kAirHistoryRecordBytes > kMaxFileBytes) {
    ++counters_.droppedMinutes;
    hasPending_ = false;
    counters_.lastError = kFileLimit;
    slotFull_[pendingSlot_] = true;
    stage_ = IoStage::Idle;
    return;
  }
  const uint8_t partial = uint8_t((bytes - sensors::kAirHistoryHeaderBytes) %
                                  sensors::kAirHistoryRecordBytes);
  if (partial) {
    // Arduino's File has no truncate API. Preserve the torn bytes and pad only
    // the partial record into an invalid CRC slot, then append a fresh segment.
    File prefix = SD_MMC.open(path, FILE_READ);
    uint8_t padded[sensors::kAirHistoryRecordBytes] = {};
    if (!prefix || !prefix.seek(bytes - partial) ||
        prefix.read(padded, partial) != partial) {
      prefix.close(); fail(kReadError); return;
    }
    prefix.close();
    // Force the final CRC byte to disagree even when an all-zero pad would
    // accidentally complete the original CRC (notably a 31-byte torn record).
    padded[31] = uint8_t(sensors::airHistoryCrc(padded, 28) >> 24) ^ 0xffu;
    File file = SD_MMC.open(path, FILE_APPEND);
    if (!file) { fail(kOpenError); return; }
    const size_t needed = sensors::kAirHistoryRecordBytes - partial;
    const size_t written = file.write(padded + partial, needed);
    file.flush();
    file.close();
    if (written != needed) { fail(kWriteError); return; }
    bytes += uint32_t(needed);
    ++counters_.tornTails;
    pending_.discontinuity = true;
  }
  if (bytes + sensors::kAirHistoryRecordBytes > kMaxFileBytes) {
    ++counters_.droppedMinutes;
    hasPending_ = false;
    counters_.lastError = kFileLimit;
    slotFull_[pendingSlot_] = true;
    stage_ = IoStage::Idle;
    return;
  }
  writeSize_ = bytes;
  stage_ = IoStage::WriteRecord;
}

void AirHistory::writeRecord() {
  char path[40];
  slotPath(pendingSlot_, path);
  File file = SD_MMC.open(path, FILE_APPEND);
  if (!file) { fail(kOpenError); return; }
  if (file.size() != writeSize_) { file.close(); fail(kWriteError); return; }
  uint8_t record[sensors::kAirHistoryRecordBytes];
  sensors::encodeAirHistoryRecord(pending_, record);
  const size_t written = file.write(record, sizeof(record));
  file.flush();
  file.close();
  if (written != sizeof(record)) { fail(kWriteError); return; }
  memcpy(attemptedRecord_, record, sizeof(record));
  appendAttempted_ = true;
  File verified = SD_MMC.open(path, FILE_READ);
  uint8_t readback[sensors::kAirHistoryRecordBytes];
  const bool correct = verified && verified.seek(writeSize_) &&
      verified.read(readback, sizeof(readback)) == sizeof(readback) &&
      memcmp(record, readback, sizeof(record)) == 0;
  verified.close();
  if (!correct) { fail(kWriteError); return; }
  finishWrite();
}

void AirHistory::finishWrite() {
  ++counters_.writtenMinutes;
  counters_.lastError = 0;
  if (!loadRequested_ && view_.dayOrdinal == pending_.dayOrdinal && buckets_) {
    plot_.add(pending_);
    view_.validMinutes = plot_.validMinutes();
  }
  hasPending_ = false;
  appendAttempted_ = false;
  stage_ = IoStage::Idle;
}

void AirHistory::poll(uint32_t nowMs, const sensors::SensorSnapshot& snapshot,
                      const sensors::HistoryClock& clock, bool allowIo) {
  if (!initialized_) begin();
  accumulator_.observe(nowMs, snapshot.air, clock, counters_);
  updateDate(clock);
  if (!allowIo || !mounted_) { setSettledState(); return; }
  if (retryWaiting_) {
    if (uint32_t(nowMs - lastRetryAt_) < kRetryMs) { setSettledState(); return; }
    retryWaiting_ = false;
  }
  const uint32_t started = millis();
  if (!directoryReady_) {
    directoryReady_ = SD_MMC.exists(kDirectory) || SD_MMC.mkdir(kDirectory);
    if (!directoryReady_) fail(kDirectoryError);
    const uint32_t elapsed = uint32_t(millis() - started);
    if (elapsed > counters_.maxIoMs) counters_.maxIoMs = elapsed;
    setSettledState();
    return;
  }
  switch (stage_) {
    case IoStage::ScanHeaders: scanHeader(); break;
    case IoStage::LoadRecords: loadRecords(); break;
    case IoStage::PrepareWrite: prepareWrite(); break;
    case IoStage::WriteRecord: writeRecord(); break;
    case IoStage::Idle:
      if (!hasPending_) hasPending_ = accumulator_.pop(pending_);
      if (hasPending_) stage_ = IoStage::PrepareWrite;
      else if (view_.clockValid && loadRequested_) startLoad();
      break;
  }
  const uint32_t elapsed = uint32_t(millis() - started);
  if (elapsed > counters_.maxIoMs) counters_.maxIoMs = elapsed;
  setSettledState();
}

}  // namespace storage
