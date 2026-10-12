#pragma once

#include <FS.h>
#include "sensors/air_history.h"

namespace storage {

// Owned and polled only by main loop, alongside the existing wallpaper SD owner.
// No callbacks or UI code access SD. FAT operations are synchronous, so the
// byte budget bounds work volume rather than promising a hard latency deadline.
class AirHistory {
 public:
  ~AirHistory();
  void begin();
  void poll(uint32_t nowMs, const sensors::SensorSnapshot& snapshot,
            const sensors::HistoryClock& clock, bool allowIo = true);
  void requestDay(uint8_t daysAgo);
  const sensors::AirHistoryView& view() const { return view_; }
  const sensors::AirHistoryCounters& counters() const { return counters_; }
 private:
  enum class IoStage : uint8_t { ScanHeaders, Idle, LoadRecords, PrepareWrite,
                                 WriteRecord };
  void updateDate(const sensors::HistoryClock& clock);
  void scanHeader();
  void startLoad();
  void loadRecords();
  void prepareWrite();
  void writeRecord();
  void finishWrite();
  void fail(int32_t code);
  void refreshAvailableDays();
  void setSettledState();
  static void slotPath(uint8_t slot, char* output);

  sensors::AirHistoryAccumulator accumulator_;
  sensors::AirHistoryPlot plot_;
  sensors::AirHistoryView view_{};
  sensors::AirHistoryCounters counters_{};
  sensors::AirHistoryBucket* buckets_ = nullptr;
  sensors::AirHistoryRecord pending_{};
  File loadFile_;
  int32_t slotDays_[7] = {-1, -1, -1, -1, -1, -1, -1};
  bool slotProtected_[7] = {};
  bool slotFull_[7] = {};
  uint8_t attemptedRecord_[sensors::kAirHistoryRecordBytes] = {};
  uint32_t loadRemaining_ = 0;
  uint32_t lastRetryAt_ = 0;
  uint32_t writeSize_ = 0;
  uint8_t scanSlot_ = 0;
  uint8_t pendingSlot_ = 0;
  IoStage stage_ = IoStage::ScanHeaders;
  bool initialized_ = false;
  bool mounted_ = false;
  bool directoryReady_ = false;
  bool loadRequested_ = true;
  bool hasPending_ = false;
  bool retryWaiting_ = false;
  bool appendAttempted_ = false;
};

}  // namespace storage
