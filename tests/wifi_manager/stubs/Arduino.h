#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#define F(value) value
#define PROGMEM

constexpr int LOW = 0;
constexpr int HIGH = 1;
constexpr int INPUT_PULLUP = 2;

class String {
 public:
  String() = default;
  String(const char* value) : value_(value == nullptr ? "" : value) {}
  String(const std::string& value) : value_(value) {}
  String(std::string&& value) : value_(std::move(value)) {}
  String(char value) : value_(1, value) {}

  template <typename Integer,
            typename = std::enable_if_t<std::is_integral_v<Integer> &&
                                        !std::is_same_v<Integer, char>>>
  String(Integer value) : value_(std::to_string(value)) {}

  size_t length() const { return value_.length(); }
  bool isEmpty() const { return value_.empty(); }
  void clear() { value_.clear(); }
  void reserve(size_t capacity) { value_.reserve(capacity); }
  const char* c_str() const { return value_.c_str(); }
  size_t write(uint8_t c) { value_ += static_cast<char>(c); return 1; }
  size_t write(const uint8_t* data, size_t size) {
    value_.append(reinterpret_cast<const char*>(data), size); return size;
  }

  char& operator[](size_t index) { return value_[index]; }
  char operator[](size_t index) const { return value_[index]; }

  String& operator+=(const String& other) {
    value_ += other.value_;
    return *this;
  }
  String& operator+=(const char* other) {
    if (other != nullptr) {
      value_ += other;
    }
    return *this;
  }
  String& operator+=(char other) {
    value_ += other;
    return *this;
  }

  friend String operator+(String left, const String& right) {
    left += right;
    return left;
  }
  friend String operator+(String left, const char* right) {
    left += right;
    return left;
  }
  friend String operator+(const char* left, const String& right) {
    String result(left);
    result += right;
    return result;
  }

  friend bool operator==(const String& left, const String& right) {
    return left.value_ == right.value_;
  }
  friend bool operator!=(const String& left, const String& right) {
    return !(left == right);
  }
  friend bool operator==(const String& left, const char* right) {
    return left.value_ == (right == nullptr ? "" : right);
  }
  friend bool operator==(const char* left, const String& right) {
    return right == left;
  }
  friend bool operator!=(const String& left, const char* right) {
    return !(left == right);
  }
  friend bool operator!=(const char* left, const String& right) {
    return !(right == left);
  }

  const std::string& stdString() const { return value_; }

 private:
  std::string value_;
};

inline std::ostream& operator<<(std::ostream& stream, const String& value) {
  return stream << value.c_str();
}

namespace fake_arduino {

inline uint32_t nowMs = 0;
inline std::map<int, int> pinValues;

inline void resetPinsAndClock() {
  nowMs = 0;
  pinValues.clear();
}

inline void setMillis(uint32_t value) { nowMs = value; }
inline void setPin(int pin, int value) { pinValues[pin] = value; }

}  // namespace fake_arduino

inline uint32_t millis() { return fake_arduino::nowMs; }
inline void delay(uint32_t durationMs) { fake_arduino::nowMs += durationMs; }
inline void pinMode(int pin, int mode) {
  (void)mode;
  if (fake_arduino::pinValues.find(pin) == fake_arduino::pinValues.end()) {
    fake_arduino::pinValues[pin] = HIGH;
  }
}
inline int digitalRead(int pin) {
  const auto found = fake_arduino::pinValues.find(pin);
  return found == fake_arduino::pinValues.end() ? HIGH : found->second;
}

class FakeSerial {
 public:
  void clear() { lines.clear(); }

  void println(const char* value) {
    lines.emplace_back(value == nullptr ? "" : value);
  }
  void println(const String& value) { lines.push_back(value.stdString()); }
  void println() { lines.emplace_back(); }

  int printf(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    va_list copied;
    va_copy(copied, arguments);
    const int size = std::vsnprintf(nullptr, 0, format, copied);
    va_end(copied);
    if (size < 0) {
      va_end(arguments);
      return size;
    }
    std::vector<char> buffer(static_cast<size_t>(size) + 1);
    std::vsnprintf(buffer.data(), buffer.size(), format, arguments);
    va_end(arguments);
    lines.emplace_back(buffer.data(), static_cast<size_t>(size));
    return size;
  }

  std::vector<std::string> lines;
};

inline FakeSerial Serial;

class FakeEspClass {
 public:
  uint64_t getEfuseMac() const { return efuseMac; }
  void restart() { ++restartCalls; }
  void reset() {
    efuseMac = 0xA1B2C3D4ULL;
    restartCalls = 0;
  }

  uint64_t efuseMac = 0xA1B2C3D4ULL;
  int restartCalls = 0;
};

inline FakeEspClass ESP;

#if !defined(__APPLE__)
inline size_t strlcpy(char* destination, const char* source, size_t size) {
  const size_t sourceLength = std::strlen(source);
  if (size > 0) {
    const size_t copied = sourceLength >= size ? size - 1 : sourceLength;
    std::memcpy(destination, source, copied);
    destination[copied] = '\0';
  }
  return sourceLength;
}
#endif
