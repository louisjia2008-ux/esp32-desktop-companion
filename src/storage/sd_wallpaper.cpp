#include "sd_wallpaper.h"

#include <SD_MMC.h>
#include <esp_heap_caps.h>

#include "sd_card.h"

namespace storage {
namespace {
const char* const kFiles[] = {"/companion-wallpaper/wallpaper0.dwc",
                             "/companion-wallpaper/wallpaper1.dwc"};
const char* const kRecords[] = {"/companion-wallpaper/active0.bin",
                               "/companion-wallpaper/active1.bin"};
const char kPartial[] = "/companion-wallpaper/staging.part";
constexpr size_t kRecordBytes = 28;

void idText(uint64_t key, char* output) {
  snprintf(output, 17, "%016llx", (unsigned long long)key);
}
}

SdWallpaper::~SdWallpaper() {
  if (buffer_) heap_caps_free(buffer_);
}

void SdWallpaper::begin() {
  if (!beginSdCard()) return;
  if (!SD_MMC.exists("/companion-wallpaper") &&
      !SD_MMC.mkdir("/companion-wallpaper")) return;
  buffer_ = static_cast<uint8_t*>(heap_caps_malloc(
      kCacheJpegBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (buffer_ == nullptr) return;
  available_ = true;
  // Only our interrupted temporary upload is removed. Other SD contents stay
  // untouched; no format, directory traversal or caller-supplied paths.
  SD_MMC.remove(kPartial);
  CacheHeader header;
  uint32_t generation = 0, crc = 0;
  for (int slot = 0; slot < 2; ++slot) {
    if (readRecord(slot, generation, header, crc) &&
        (activeSlot_ < 0 || int32_t(generation - generation_) > 0)) {
      activeSlot_ = slot;
      generation_ = generation;
      activeHeader_ = header;
      activeCrc_ = crc;
    }
  }
  if (activeSlot_ >= 0) {
    if (!startValidation(SD_MMC.open(kFiles[activeSlot_], FILE_READ),
                         activeCrc_, true)) {
      activeSlot_ = -1;
    }
  }
  Serial.printf("SD_CACHE initialized=true boot_validation=%s\n",
                validating_ ? "true" : "false");
}

void SdWallpaper::reply(const char* state, const char* reason) {
  strlcpy(ack_.state, state, sizeof(ack_.state));
  strlcpy(ack_.reason, reason, sizeof(ack_.reason));
  pendingAck_ = ack_;
  ackPending_ = true;
}

bool SdWallpaper::popAck(CacheAck& ack) {
  if (!ackPending_) return false;
  ack = pendingAck_;
  ackPending_ = false;
  return true;
}

void SdWallpaper::beginUpload(const char* transferId, const char* wallpaperId,
                              uint32_t bytes, uint32_t crc, uint32_t now) {
  if (uploading_ && strcmp(ack_.transferId, transferId) == 0 &&
      strcmp(ack_.wallpaperId, wallpaperId) == 0 && ack_.bytes == bytes &&
      expectedCrc_ == crc) {
    lastUploadActivity_ = now;
    reply("ready");
    return;
  }
  if (validating_ || candidateReady_ || uploading_) {
    CacheAck saved = ack_;
    ack_ = {};
    strlcpy(ack_.transferId, transferId, sizeof(ack_.transferId));
    strlcpy(ack_.wallpaperId, wallpaperId, sizeof(ack_.wallpaperId));
    ack_.bytes = bytes;
    reply("error", "busy");
    ack_ = saved;
    return;
  }
  ack_ = {};
  strlcpy(ack_.transferId, transferId, sizeof(ack_.transferId));
  strlcpy(ack_.wallpaperId, wallpaperId, sizeof(ack_.wallpaperId));
  ack_.bytes = bytes;
  if (!available_) { reply("error", "sd_unavailable"); return; }
  if (bytes <= kCacheHeaderBytes || bytes > kCacheMaximumBytes) {
    reply("error", "size_limit"); return;
  }
  if (ready_ && strcmp(activeId_, wallpaperId) == 0 &&
      activeHeader_.bytes == bytes && activeCrc_ == crc) {
    ack_.offset = bytes;
    reply("cached");
    Serial.printf("SD_CACHE reuse id=%s bytes=%lu no_upload=true\n",
                  wallpaperId, (unsigned long)bytes);
    return;
  }
  candidateSlot_ = activeSlot_ == 0 ? 1 : 0;
  cleanupOld(candidateSlot_);
  const uint64_t total = SD_MMC.totalBytes(), used = SD_MMC.usedBytes();
  if (used > total || uint64_t(bytes) + 4096 > total - used) {
    reply("error", "sd_full"); return;
  }
  SD_MMC.remove(kPartial);
  uploadFile_ = SD_MMC.open(kPartial, FILE_WRITE);
  if (!uploadFile_) { reply("error", "sd_write_failed"); return; }
  expectedCrc_ = crc;
  uploadedCrc_ = 0;
  lastChunkSize_ = 0;
  uploading_ = true;
  lastUploadActivity_ = now;
  reply("ready");
  Serial.printf("SD_CACHE upload_begin id=%s bytes=%lu old_preserved=%s\n",
                wallpaperId, (unsigned long)bytes, ready_ ? "true" : "false");
}

void SdWallpaper::acceptChunk(const uint8_t* packet, size_t length,
                              uint32_t now) {
  CacheChunk chunk;
  if (!decodeCacheChunk(packet, length, chunk)) {
    if (uploading_) { ++crcErrors_; failTransfer("chunk_crc_or_bounds"); }
    return;
  }
  if (!uploading_ || chunk.transferKey != wallpaperKey(ack_.transferId)) return;
  if (lastChunkSize_ != 0 && chunk.offset == lastChunkOffset_ &&
      chunk.size == lastChunkSize_ && chunk.crc == lastChunkCrc_) {
    lastUploadActivity_ = now;
    reply("progress");
    return;
  }
  if (chunk.offset != ack_.offset || chunk.size > ack_.bytes - ack_.offset) {
    failTransfer("chunk_offset"); return;
  }
  if (uploadFile_.write(chunk.data, chunk.size) != chunk.size) {
    failTransfer("sd_write_failed"); return;
  }
  uploadedCrc_ = cacheCrc32(uploadedCrc_, chunk.data, chunk.size);
  lastChunkOffset_ = chunk.offset;
  lastChunkCrc_ = chunk.crc;
  lastChunkSize_ = chunk.size;
  ack_.offset += chunk.size;
  lastUploadActivity_ = now;
  reply("progress");
}

void SdWallpaper::finishUpload(const char* transferId) {
  if (strcmp(ack_.transferId, transferId) != 0) return;
  if (validating_ || candidateReady_) { reply("verifying"); return; }
  if (strcmp(ack_.state, "complete") == 0 || strcmp(ack_.state, "cached") == 0) {
    reply(ack_.state); return;
  }
  if (!uploading_ || ack_.offset != ack_.bytes || uploadedCrc_ != expectedCrc_) {
    ++crcErrors_; failTransfer("file_crc_or_incomplete"); return;
  }
  uploadFile_.flush();
  uploadFile_.close();
  uploading_ = false;
  if (!startValidation(SD_MMC.open(kPartial, FILE_READ), expectedCrc_, false)) {
    failTransfer("invalid_package_header"); return;
  }
  reply("verifying");
}

bool SdWallpaper::startValidation(File file, uint32_t expectedCrc, bool boot) {
  uint8_t bytes[kCacheHeaderBytes];
  if (!file || file.read(bytes, sizeof(bytes)) != sizeof(bytes) ||
      !decodeCacheHeader(bytes, sizeof(bytes), validationHeader_) ||
      file.size() != validationHeader_.bytes ||
      (!boot && (validationHeader_.key != wallpaperKey(ack_.wallpaperId) ||
                 validationHeader_.bytes != ack_.bytes))) return false;
  validationFile_ = file;
  expectedCrc_ = expectedCrc;
  validationCrc_ = cacheCrc32(0, bytes, sizeof(bytes));
  validationBodyCrc_ = 0;
  validationFrame_ = 0;
  validating_ = true;
  bootValidation_ = boot;
  return true;
}

void SdWallpaper::tick(uint32_t now) {
  if (uploading_ && now - lastUploadActivity_ > 15000) {
    failTransfer("upload_timeout");
  }
  if (!validating_) return;
  uint8_t lengthBytes[4];
  if (validationFile_.read(lengthBytes, 4) != 4) {
    failTransfer("truncated_index"); return;
  }
  const uint32_t length = little32(lengthBytes);
  if (length <= 4 || length > kCacheJpegBytes ||
      validationFile_.position() > validationHeader_.bytes ||
      length > validationHeader_.bytes - validationFile_.position() ||
      validationFile_.read(buffer_, length) != length ||
      !validCacheJpeg(buffer_, length)) {
    failTransfer("invalid_jpeg"); return;
  }
  validationCrc_ = cacheCrc32(validationCrc_, lengthBytes, 4);
  validationCrc_ = cacheCrc32(validationCrc_, buffer_, length);
  validationBodyCrc_ = cacheCrc32(validationBodyCrc_, lengthBytes, 4);
  validationBodyCrc_ = cacheCrc32(validationBodyCrc_, buffer_, length);
  if (++validationFrame_ != validationHeader_.frames) return;
  if (validationFile_.position() != validationHeader_.bytes ||
      validationBodyCrc_ != validationHeader_.payloadCrc ||
      validationCrc_ != expectedCrc_) {
    ++crcErrors_; failTransfer("sd_readback_crc"); return;
  }
  validating_ = false;
  if (bootValidation_) {
    bootValidation_ = false;
    activeFile_ = validationFile_;
    validationFile_ = File();
    activeFile_.seek(kCacheHeaderBytes);
    activeHeader_ = validationHeader_;
    idText(activeHeader_.key, activeId_);
    frameIndex_ = 0;
    nextFrameAt_ = now;
    ready_ = true;
    Serial.printf("SD_CACHE boot_ready id=%s frames=%lu crc_verified=true\n",
                  activeId_, (unsigned long)activeHeader_.frames);
    return;
  }
  validationFile_.close();
  if (!SD_MMC.rename(kPartial, kFiles[candidateSlot_])) {
    failTransfer("sd_rename_failed"); return;
  }
  validationFile_ = SD_MMC.open(kFiles[candidateSlot_], FILE_READ);
  if (!validationFile_ || !validationFile_.seek(kCacheHeaderBytes)) {
    failTransfer("sd_open_failed"); return;
  }
  candidateReady_ = true;
  Serial.printf("SD_CACHE validated id=%s bytes=%lu crc_verified=true\n",
                ack_.wallpaperId, (unsigned long)ack_.bytes);
}

bool SdWallpaper::readFrame(File& file, const CacheHeader& header,
                            uint32_t sequence, SdWallpaperFrame& frame) {
  uint8_t lengthBytes[4];
  if (!file || file.read(lengthBytes, 4) != 4) return false;
  const uint32_t length = little32(lengthBytes);
  if (length <= 4 || length > kCacheJpegBytes ||
      file.position() > header.bytes || length > header.bytes - file.position() ||
      file.read(buffer_, length) != length || !validCacheJpeg(buffer_, length))
    return false;
  frame.jpeg = buffer_;
  frame.length = length;
  frame.sequence = sequence;
  idText(header.key, frame.wallpaperId);
  return true;
}

bool SdWallpaper::readCandidate(SdWallpaperFrame& frame) {
  return candidateReady_ && validationFile_.seek(kCacheHeaderBytes) &&
         readFrame(validationFile_, validationHeader_, 0, frame);
}

bool SdWallpaper::acceptCandidate(uint32_t now) {
  if (!candidateReady_) return false;
  uint32_t nextGeneration = generation_ + 1;
  if (nextGeneration == 0) nextGeneration = 1;
  if (!writeRecord(candidateSlot_, nextGeneration, validationHeader_, expectedCrc_)) {
    failTransfer("sd_record_failed"); return false;
  }
  const int previousSlot = activeSlot_;
  activeFile_.close();
  activeFile_ = validationFile_;
  validationFile_ = File();
  activeHeader_ = validationHeader_;
  activeSlot_ = candidateSlot_;
  generation_ = nextGeneration;
  activeCrc_ = expectedCrc_;
  idText(activeHeader_.key, activeId_);
  frameIndex_ = 1;
  nextFrameAt_ = now + 1000 / activeHeader_.fps;
  ready_ = true;
  candidateReady_ = false;
  ++uploadsCommitted_;
  ++framesPlayed_;
  reply("complete");
  Serial.printf("SD_CACHE committed id=%s bytes=%lu generation=%lu "
                "first_frame_decoded=true\n", activeId_,
                (unsigned long)activeHeader_.bytes, (unsigned long)generation_);
  if (previousSlot >= 0 && previousSlot != activeSlot_) cleanupOld(previousSlot);
  return true;
}

void SdWallpaper::rejectCandidate() { failTransfer("first_frame_decode_failed"); }

bool SdWallpaper::nextFrame(uint32_t now, bool paused, SdWallpaperFrame& frame) {
  if (!ready_) return false;
  if (paused) { nextFrameAt_ = now; return false; }
  if (int32_t(now - nextFrameAt_) < 0) return false;
  if (frameIndex_ == activeHeader_.frames) {
    if (!activeFile_.seek(kCacheHeaderBytes)) { ready_ = false; return false; }
    frameIndex_ = 0;
    ++loopsPlayed_;
  }
  if (!readFrame(activeFile_, activeHeader_, frameIndex_, frame)) {
    ready_ = false;
    Serial.println("SD_CACHE playback_error reason=sd_read");
    return false;
  }
  ++frameIndex_;
  ++framesPlayed_;
  nextFrameAt_ = now + 1000 / activeHeader_.fps;
  return true;
}

void SdWallpaper::failTransfer(const char* reason) {
  const bool boot = bootValidation_;
  uploadFile_.close();
  validationFile_.close();
  uploading_ = validating_ = candidateReady_ = bootValidation_ = false;
  if (boot) {
    activeSlot_ = -1;
    ready_ = false;
    Serial.printf("SD_CACHE boot_rejected reason=%s\n", reason);
    return;
  }
  SD_MMC.remove(kPartial);
  if (candidateSlot_ >= 0 && candidateSlot_ != activeSlot_) cleanupOld(candidateSlot_);
  reply("error", reason);
  Serial.printf("SD_CACHE upload_rejected id=%s reason=%s old_preserved=%s\n",
                ack_.wallpaperId, reason, ready_ ? "true" : "false");
}

void SdWallpaper::abortUpload(const char* transferId) {
  if (bootValidation_ || (transferId && strcmp(ack_.transferId, transferId) != 0)) return;
  if (uploading_ || validating_ || candidateReady_) {
    failTransfer("aborted");
    reply("aborted");
  }
}

void SdWallpaper::clear(const char* transferId, const char* wallpaperId) {
  abortUpload();
  ack_ = {};
  strlcpy(ack_.transferId, transferId, sizeof(ack_.transferId));
  strlcpy(ack_.wallpaperId, wallpaperId, sizeof(ack_.wallpaperId));
  if (strcmp(activeId_, wallpaperId) != 0) { reply("error", "id_mismatch"); return; }
  activeFile_.close();
  ready_ = false;
  activeSlot_ = -1;
  activeId_[0] = '\0';
  cleanupOld(0);
  cleanupOld(1);
  reply("cleared");
}

bool SdWallpaper::readRecord(int slot, uint32_t& generation, CacheHeader& header,
                             uint32_t& crc) {
  uint8_t record[kRecordBytes];
  File file = SD_MMC.open(kRecords[slot], FILE_READ);
  if (!file || file.size() != sizeof(record) ||
      file.read(record, sizeof(record)) != sizeof(record) ||
      memcmp(record, "ACT1", 4) != 0 ||
      cacheCrc32(0, record, 24) != little32(record + 24)) return false;
  generation = little32(record + 4);
  const uint64_t key = little64(record + 8);
  crc = little32(record + 16);
  const uint32_t size = little32(record + 20);
  file.close();
  file = SD_MMC.open(kFiles[slot], FILE_READ);
  uint8_t bytes[kCacheHeaderBytes];
  return generation != 0 && file && file.size() == size &&
         file.read(bytes, sizeof(bytes)) == sizeof(bytes) &&
         decodeCacheHeader(bytes, sizeof(bytes), header) &&
         header.key == key && header.bytes == size;
}

bool SdWallpaper::writeRecord(int slot, uint32_t generation,
                              const CacheHeader& header, uint32_t crc) {
  uint8_t record[kRecordBytes] = {};
  memcpy(record, "ACT1", 4);
  put32(record + 4, generation);
  put64(record + 8, header.key);
  put32(record + 16, crc);
  put32(record + 20, header.bytes);
  put32(record + 24, cacheCrc32(0, record, 24));
  // The other slot's pointer is left intact until this one is read back.
  SD_MMC.remove(kRecords[slot]);
  File file = SD_MMC.open(kRecords[slot], FILE_WRITE);
  if (!file || file.write(record, sizeof(record)) != sizeof(record)) return false;
  file.flush(); file.close();
  uint8_t readback[kRecordBytes];
  file = SD_MMC.open(kRecords[slot], FILE_READ);
  return file && file.size() == sizeof(record) &&
         file.read(readback, sizeof(readback)) == sizeof(readback) &&
         memcmp(record, readback, sizeof(record)) == 0;
}

void SdWallpaper::cleanupOld(int slot) {
  if (slot == activeSlot_) return;
  const bool hadFile = SD_MMC.exists(kFiles[slot]);
  bool removed = true;
  if (hadFile) removed = SD_MMC.remove(kFiles[slot]);
  if (SD_MMC.exists(kRecords[slot])) removed = SD_MMC.remove(kRecords[slot]) && removed;
  if (hadFile) Serial.printf("SD_CACHE old_cleanup slot=%d complete=%s\n",
                            slot, removed ? "true" : "false");
}

void SdWallpaper::reportMetrics() const {
  Serial.printf("SD_WALLPAPER_METRICS mounted=%s ready=%s id=%s frames=%lu "
                "loops=%lu commits=%lu crc_errors=%lu uploading=%s validating=%s\n",
                available_ ? "true" : "false", ready_ ? "true" : "false",
                activeId_, (unsigned long)framesPlayed_, (unsigned long)loopsPlayed_,
                (unsigned long)uploadsCommitted_, (unsigned long)crcErrors_,
                uploading_ ? "true" : "false", validating_ ? "true" : "false");
}

}  // namespace storage
