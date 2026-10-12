#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>

#define F(value) value
#define PROGMEM
#define bit(value) (1UL << (value))
#define ESP_ARDUINO_VERSION_VAL(major, minor, patch) \
  (((major) << 16) | ((minor) << 8) | (patch))
#define ESP_ARDUINO_VERSION ESP_ARDUINO_VERSION_VAL(2, 0, 17)
#define ESP_IDF_VERSION_MAJOR 4

class String {
 public:
  String() = default;
  String(const char* value) : value_(value == nullptr ? "" : value) {}
  String(const std::string& value) : value_(value) {}
  String(char value) : value_(1, value) {}
  template <typename T, typename = std::enable_if_t<std::is_integral_v<T> &&
                                                  !std::is_same_v<T, char>>>
  String(T value) : value_(std::to_string(value)) {}
  size_t length() const { return value_.size(); }
  const char* c_str() const { return value_.c_str(); }
  void reserve(size_t size) { value_.reserve(size); }
  char& operator[](size_t index) { return value_[index]; }
  char operator[](size_t index) const { return value_[index]; }
  int indexOf(char c, size_t start = 0) const {
    const auto found = value_.find(c, start);
    return found == std::string::npos ? -1 : static_cast<int>(found);
  }
  int indexOf(const String& s, size_t start = 0) const {
    const auto found = value_.find(s.value_, start);
    return found == std::string::npos ? -1 : static_cast<int>(found);
  }
  String substring(size_t start, size_t end = std::string::npos) const {
    if (start > value_.size()) return String();
    return value_.substr(start, end == std::string::npos ? end : end - start);
  }
  bool startsWith(const String& other) const { return value_.find(other.value_) == 0; }
  bool equalsIgnoreCase(const String& other) const {
    if (length() != other.length()) return false;
    for (size_t i = 0; i < length(); ++i) {
      if (std::tolower(static_cast<unsigned char>(value_[i])) !=
          std::tolower(static_cast<unsigned char>(other.value_[i]))) return false;
    }
    return true;
  }
  void remove(size_t start, size_t size) { value_.erase(start, size); }
  long toInt() const { return std::strtol(value_.c_str(), nullptr, 10); }
  void trim() {
    const auto first = value_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { value_.clear(); return; }
    value_ = value_.substr(first, value_.find_last_not_of(" \t\r\n") - first + 1);
  }
  String& operator+=(const String& s) { value_ += s.value_; return *this; }
  String& operator+=(char c) { value_ += c; return *this; }
  friend String operator+(String left, const String& right) { return left += right; }
  friend bool operator==(const String& left, const String& right) {
    return left.value_ == right.value_;
  }
  friend bool operator!=(const String& left, const String& right) { return !(left == right); }
  const std::string& stdString() const { return value_; }
 private:
  std::string value_;
};

namespace fake_network {
inline uint32_t nowMs = 0;
inline uint32_t randomState = 1;
inline uint32_t timedLineReads = 0;
}
inline uint32_t millis() { return fake_network::nowMs; }
inline uint32_t micros() { return millis() * 1000; }
inline void delay(uint32_t ms) { fake_network::nowMs += ms; }
inline void yield() {}
inline void randomSeed(uint32_t seed) { fake_network::randomState = seed | 1; }
inline long random(long upper) {
  fake_network::randomState = fake_network::randomState * 1664525u + 1013904223u;
  return fake_network::randomState % upper;
}
struct FakeEsp { uint32_t getFreeHeap() const { return 1024 * 1024; } };
inline FakeEsp ESP;

class Stream {
 public:
  virtual ~Stream() = default;
  virtual int available() = 0;
  virtual int read() = 0;
  void setTimeout(unsigned long ms) { timeoutMs_ = ms; }
  unsigned long getTimeout() const { return timeoutMs_; }
  String readStringUntil(char end) {
    ++fake_network::timedLineReads;
    String value;
    while (available() > 0) {
      const int next = read();
      if (next == end) return value;
      if (next < 0) break;
      value += static_cast<char>(next);
    }
    delay(static_cast<uint32_t>(timeoutMs_));
    return value;
  }
  size_t readBytes(uint8_t* out, size_t count) {
    size_t readCount = 0;
    while (readCount < count && available() > 0) {
      const int next = read();
      if (next < 0) break;
      out[readCount++] = static_cast<uint8_t>(next);
    }
    return readCount;
  }
  size_t readBytes(char* out, size_t count) {
    return readBytes(reinterpret_cast<uint8_t*>(out), count);
  }
 private:
  unsigned long timeoutMs_ = 1000;
};
