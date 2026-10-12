#pragma once

#include <Arduino.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace fake_preferences {

using Value = std::vector<uint8_t>;
using Namespace = std::map<std::string, Value>;
using Storage = std::map<std::string, Namespace>;

inline Storage storage;
inline bool beginSucceeds = true;
inline bool putSucceeds = true;
inline size_t putBytesCalls = 0;
inline size_t clearCalls = 0;

inline Value bytesForString(const std::string& value) {
  return Value(value.begin(), value.end());
}

inline Value bytesForUInt(uint32_t value) {
  Value result(sizeof(value));
  std::memcpy(result.data(), &value, sizeof(value));
  return result;
}

inline void reset() {
  storage.clear();
  beginSucceeds = true;
  putSucceeds = true;
  putBytesCalls = 0;
  clearCalls = 0;
}

inline void resetRuntime() {
  beginSucceeds = true;
  putSucceeds = true;
  putBytesCalls = 0;
  clearCalls = 0;
}

inline void setString(const std::string& nameSpace, const std::string& key,
                      const std::string& value) {
  storage[nameSpace][key] = bytesForString(value);
}

inline void setUInt(const std::string& nameSpace, const std::string& key,
                    uint32_t value) {
  storage[nameSpace][key] = bytesForUInt(value);
}

inline Storage snapshot() { return storage; }

}  // namespace fake_preferences

class Preferences {
 public:
  bool begin(const char* nameSpace, bool readOnly = false) {
    (void)readOnly;
    if (!fake_preferences::beginSucceeds || nameSpace == nullptr) {
      ready_ = false;
      return false;
    }
    namespace_ = nameSpace;
    ready_ = true;
    return true;
  }

  size_t getBytesLength(const char* key) const {
    const auto* value = find(key);
    return value == nullptr ? 0 : value->size();
  }

  size_t getBytes(const char* key, void* destination, size_t capacity) const {
    const auto* value = find(key);
    if (value == nullptr || destination == nullptr) {
      return 0;
    }
    const size_t copied = std::min(capacity, value->size());
    std::memcpy(destination, value->data(), copied);
    return copied;
  }

  size_t putBytes(const char* key, const void* source, size_t length) {
    ++fake_preferences::putBytesCalls;
    if (!ready_ || !fake_preferences::putSucceeds || key == nullptr ||
        (source == nullptr && length != 0)) {
      return 0;
    }
    const auto* begin = static_cast<const uint8_t*>(source);
    fake_preferences::storage[namespace_][key] =
        fake_preferences::Value(begin, begin + length);
    return length;
  }

  bool isKey(const char* key) const { return find(key) != nullptr; }

  String getString(const char* key, const String& defaultValue = String()) const {
    const auto* value = find(key);
    if (value == nullptr) {
      return defaultValue;
    }
    return String(std::string(value->begin(), value->end()));
  }

  uint32_t getUInt(const char* key, uint32_t defaultValue = 0) const {
    const auto* value = find(key);
    if (value == nullptr || value->size() != sizeof(uint32_t)) {
      return defaultValue;
    }
    uint32_t result = 0;
    std::memcpy(&result, value->data(), sizeof(result));
    return result;
  }

  bool clear() {
    ++fake_preferences::clearCalls;
    if (!ready_) {
      return false;
    }
    fake_preferences::storage[namespace_].clear();
    return true;
  }

 private:
  const fake_preferences::Value* find(const char* key) const {
    if (!ready_ || key == nullptr) {
      return nullptr;
    }
    const auto namespaceFound = fake_preferences::storage.find(namespace_);
    if (namespaceFound == fake_preferences::storage.end()) {
      return nullptr;
    }
    const auto valueFound = namespaceFound->second.find(key);
    return valueFound == namespaceFound->second.end() ? nullptr
                                                       : &valueFound->second;
  }

  std::string namespace_;
  bool ready_ = false;
};
