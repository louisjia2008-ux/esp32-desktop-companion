#pragma once

#include <Arduino.h>
#include <FS.h>

#include "wallpaper_format.h"

namespace storage {

struct SdWallpaperFrame {
  const uint8_t* jpeg = nullptr;
  size_t length = 0;
  uint32_t sequence = 0;
  char wallpaperId[17] = {};
};

struct CacheAck {
  char transferId[17] = {};
  char wallpaperId[17] = {};
  char state[17] = {};
  char reason[49] = {};
  uint32_t offset = 0;
  uint32_t bytes = 0;
};

class SdWallpaper {
 public:
  ~SdWallpaper();
  void begin();
  bool available() const { return available_; }
  bool ready() const { return ready_; }
  // All SD users run in the main loop. Defer optional history I/O while a
  // wallpaper transfer or its validation owns the storage budget.
  bool storageBusy() const { return uploading_ || validating_ || candidateReady_; }
  const char* activeId() const { return activeId_; }
  void beginUpload(const char* transferId, const char* wallpaperId,
                   uint32_t bytes, uint32_t crc, uint32_t now);
  void acceptChunk(const uint8_t* packet, size_t length, uint32_t now);
  void finishUpload(const char* transferId);
  void abortUpload(const char* transferId = nullptr);
  void clear(const char* transferId, const char* wallpaperId);
  void tick(uint32_t now);
  bool candidateAvailable() const { return candidateReady_; }
  bool readCandidate(SdWallpaperFrame& frame);
  bool acceptCandidate(uint32_t now);
  void rejectCandidate();
  bool nextFrame(uint32_t now, bool paused, SdWallpaperFrame& frame);
  bool popAck(CacheAck& ack);
  void reportMetrics() const;

 private:
  void reply(const char* state, const char* reason = "");
  void failTransfer(const char* reason);
  bool startValidation(File file, uint32_t expectedCrc, bool boot);
  bool readFrame(File& file, const CacheHeader& header,
                 uint32_t sequence, SdWallpaperFrame& frame);
  bool writeRecord(int slot, uint32_t generation, const CacheHeader& header,
                   uint32_t crc);
  bool readRecord(int slot, uint32_t& generation, CacheHeader& header,
                  uint32_t& crc);
  void cleanupOld(int slot);

  bool available_ = false;
  bool ready_ = false;
  bool uploading_ = false;
  bool validating_ = false;
  bool bootValidation_ = false;
  bool candidateReady_ = false;
  bool ackPending_ = false;
  int activeSlot_ = -1;
  int candidateSlot_ = -1;
  uint8_t* buffer_ = nullptr;
  File activeFile_;
  File uploadFile_;
  File validationFile_;
  CacheHeader activeHeader_{};
  CacheHeader validationHeader_{};
  char activeId_[17] = {};
  CacheAck ack_{};
  CacheAck pendingAck_{};
  uint32_t generation_ = 0;
  uint32_t activeCrc_ = 0;
  uint32_t expectedCrc_ = 0;
  uint32_t uploadedCrc_ = 0;
  uint32_t validationCrc_ = 0;
  uint32_t validationBodyCrc_ = 0;
  uint32_t validationFrame_ = 0;
  uint32_t lastChunkOffset_ = 0;
  uint32_t lastChunkCrc_ = 0;
  size_t lastChunkSize_ = 0;
  uint32_t lastUploadActivity_ = 0;
  uint32_t nextFrameAt_ = 0;
  uint32_t frameIndex_ = 0;
  uint32_t framesPlayed_ = 0;
  uint32_t loopsPlayed_ = 0;
  uint32_t uploadsCommitted_ = 0;
  uint32_t crcErrors_ = 0;
};

}  // namespace storage
