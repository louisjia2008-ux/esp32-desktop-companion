#pragma once

#include <stddef.h>
#include <stdint.h>
#include <cstring>

namespace storage {

constexpr size_t kCacheHeaderBytes = 40;
constexpr size_t kCacheChunkHeaderBytes = 20;
constexpr size_t kCacheChunkBytes = 12 * 1024;
constexpr size_t kCacheJpegBytes = 14 * 1024;
constexpr uint32_t kCacheMaximumBytes = 128UL * 1024 * 1024;
constexpr uint32_t kCacheMaximumDurationSeconds = 600;

inline uint16_t little16(const uint8_t* p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline uint32_t little32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint64_t little64(const uint8_t* p) {
  return uint64_t(little32(p)) | (uint64_t(little32(p + 4)) << 32);
}
inline void put32(uint8_t* p, uint32_t n) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(n >> (8 * i));
}
inline void put64(uint8_t* p, uint64_t n) {
  put32(p, uint32_t(n)); put32(p + 4, uint32_t(n >> 32));
}
// Incremental CRC-32/ISO-HDLC, identical to Python zlib.crc32.
inline uint32_t cacheCrc32(uint32_t previous, const uint8_t* p, size_t size) {
  uint32_t crc = ~previous;
  for (size_t i = 0; i < size; ++i) {
    crc ^= p[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((0U - (crc & 1U)) & 0xedb88320U);
  }
  return ~crc;
}
inline uint64_t wallpaperKey(const char* text) {
  uint64_t key = 0;
  for (size_t i = 0; i < 16; ++i) {
    const char c = text[i];
    key = (key << 4) | uint8_t(c <= '9' ? c - '0' : c - 'a' + 10);
  }
  return key;
}

struct CacheHeader {
  uint8_t fps = 0;
  uint32_t frames = 0;
  uint32_t bytes = 0;
  uint64_t key = 0;
  uint32_t payloadCrc = 0;
};

inline bool decodeCacheHeader(const uint8_t* data, size_t size,
                              CacheHeader& header) {
  if (size != kCacheHeaderBytes || memcmp(data, "DWC1", 4) != 0 ||
      little16(data + 4) != 1 || little16(data + 6) != kCacheHeaderBytes ||
      little16(data + 8) != 320 || little16(data + 10) != 480 ||
      little16(data + 14) != 0 ||
      cacheCrc32(0, data, 36) != little32(data + 36)) return false;
  const uint16_t fps = little16(data + 12);
  const uint32_t frames = little32(data + 16);
  const uint32_t bytes = little32(data + 20);
  if ((fps != 6 && fps != 8 && fps != 10 && fps != 12) ||
      frames == 0 || frames > uint32_t(fps) * kCacheMaximumDurationSeconds ||
      bytes <= kCacheHeaderBytes ||
      bytes > kCacheMaximumBytes) return false;
  header.fps = uint8_t(fps);
  header.frames = frames;
  header.bytes = bytes;
  header.key = little64(data + 24);
  header.payloadCrc = little32(data + 32);
  return true;
}

inline bool validCacheJpeg(const uint8_t* data, size_t size) {
  return size > 4 && size <= kCacheJpegBytes && data[0] == 0xff &&
         data[1] == 0xd8 && data[size - 2] == 0xff && data[size - 1] == 0xd9;
}

struct CacheChunk {
  uint64_t transferKey = 0;
  uint32_t offset = 0;
  uint32_t crc = 0;
  const uint8_t* data = nullptr;
  size_t size = 0;
};

inline bool decodeCacheChunk(const uint8_t* bytes, size_t size,
                             CacheChunk& chunk) {
  if (size <= kCacheChunkHeaderBytes ||
      size > kCacheChunkHeaderBytes + kCacheChunkBytes ||
      memcmp(bytes, "DWS1", 4) != 0) return false;
  chunk.transferKey = little64(bytes + 4);
  chunk.offset = little32(bytes + 12);
  chunk.crc = little32(bytes + 16);
  chunk.data = bytes + kCacheChunkHeaderBytes;
  chunk.size = size - kCacheChunkHeaderBytes;
  return chunk.offset <= kCacheMaximumBytes &&
         chunk.size <= kCacheMaximumBytes - chunk.offset &&
         cacheCrc32(0, chunk.data, chunk.size) == chunk.crc;
}

}  // namespace storage
