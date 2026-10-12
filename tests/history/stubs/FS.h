#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
inline size_t ioBytes = 0;
inline bool failOpens = false;
inline bool failReads = false;
inline bool failReadbackOnce = false;
inline bool failWrites = false;
inline size_t partialWriteBytes = SIZE_MAX;

class File {
  struct Handle {
    std::fstream stream;
    std::string path;
    bool writable;
    Handle(const std::string& p, const std::string& mode) : path(p), writable(mode != "r") {
      auto flags = std::ios::binary;
      if (mode == "r") flags |= std::ios::in;
      else if (mode == "a") flags |= std::ios::out | std::ios::app;
      else flags |= std::ios::out | std::ios::trunc;
      if (!failOpens) stream.open(path, flags);
    }
  };
  std::shared_ptr<Handle> handle_;
 public:
  File() = default;
  File(const std::string& path, const std::string& mode)
      : handle_(std::make_shared<Handle>(path, mode)) {}
  explicit operator bool() const { return handle_ && handle_->stream.is_open(); }
  size_t read(uint8_t* data, size_t length) {
    if (!*this || failReads) return 0;
    if (failReadbackOnce && handle_->stream.tellg() >= 32 && length == 32) {
      failReadbackOnce = false;
      return 0;
    }
    handle_->stream.read(reinterpret_cast<char*>(data), std::streamsize(length));
    const size_t got = size_t(handle_->stream.gcount());
    ioBytes += got;
    return got;
  }
  size_t write(const uint8_t* data, size_t length) {
    if (!*this || failWrites) return 0;
    const size_t count = std::min(length, partialWriteBytes);
    handle_->stream.write(reinterpret_cast<const char*>(data), std::streamsize(count));
    ioBytes += count;
    return handle_->stream ? count : 0;
  }
  bool seek(uint32_t offset) {
    if (!*this) return false;
    handle_->stream.clear();
    handle_->stream.seekg(offset);
    return bool(handle_->stream);
  }
  uint32_t size() { return *this ? uint32_t(std::filesystem::file_size(handle_->path)) : 0; }
  void flush() { if (*this) handle_->stream.flush(); }
  void close() { handle_.reset(); }
};
