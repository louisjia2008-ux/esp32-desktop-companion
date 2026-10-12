#pragma once
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#define FILE_READ "r"
#define FILE_WRITE "w"
inline bool failRecordWrites = false;
inline bool corruptStagingReads = false;

class File {
  struct Handle {
    std::fstream stream;
    std::string path;
    bool writable;
    Handle(const std::string& p, bool w) : path(p), writable(w) {
      stream.open(path, std::ios::binary | (w ? std::ios::out | std::ios::trunc : std::ios::in));
    }
  };
  std::shared_ptr<Handle> handle_;
 public:
  File() = default;
  File(const std::string& path, bool writable) : handle_(std::make_shared<Handle>(path, writable)) {}
  explicit operator bool() const { return handle_ && handle_->stream.is_open(); }
  size_t read(uint8_t* data, size_t length) {
    if (!*this) return 0;
    handle_->stream.read(reinterpret_cast<char*>(data), length);
    const size_t count = handle_->stream.gcount();
    if (corruptStagingReads && count > 4 && handle_->path.find("staging.part") != std::string::npos)
      data[3] ^= 1;
    return count;
  }
  size_t write(const uint8_t* data, size_t length) {
    if (!*this || (failRecordWrites && handle_->path.find("active") != std::string::npos)) return 0;
    handle_->stream.write(reinterpret_cast<const char*>(data), length);
    return handle_->stream ? length : 0;
  }
  bool seek(uint32_t offset) {
    if (!*this) return false;
    handle_->stream.clear();
    if (handle_->writable) handle_->stream.seekp(offset); else handle_->stream.seekg(offset);
    return bool(handle_->stream);
  }
  uint32_t position() {
    return uint32_t(handle_->writable ? handle_->stream.tellp() : handle_->stream.tellg());
  }
  uint32_t size() { return *this ? uint32_t(std::filesystem::file_size(handle_->path)) : 0; }
  void flush() { if (*this) handle_->stream.flush(); }
  void close() { handle_.reset(); }
};
