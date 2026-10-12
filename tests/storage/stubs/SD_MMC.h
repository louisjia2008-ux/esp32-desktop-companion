#pragma once
#include "FS.h"

constexpr int CARD_NONE = 0;
constexpr int SDMMC_FREQ_DEFAULT = 20000;
class FakeSd {
 public:
  std::string root;
  bool formatted = false;
  std::string path(const char* name) { return root + name; }
  bool setPins(int clk, int cmd, int d0) { return clk == 12 && cmd == 11 && d0 == 13; }
  bool begin(const char*, bool oneBit, bool format, int, int) {
    formatted = format; return oneBit && !format;
  }
  void end() {}
  int cardType() { return 1; }
  uint64_t cardSize() { return 1004535808; }
  uint64_t totalBytes() { return 983957504; }
  uint64_t usedBytes() { return 1024; }
  bool exists(const char* name) { return std::filesystem::exists(path(name)); }
  bool mkdir(const char* name) { return std::filesystem::create_directory(path(name)); }
  bool remove(const char* name) { return std::filesystem::remove(path(name)); }
  bool rename(const char* from, const char* to) {
    std::error_code error;
    if (exists(to)) return false;
    std::filesystem::rename(path(from), path(to), error);
    return !error;
  }
  File open(const char* name, const char* mode) { return File(path(name), std::string(mode) == FILE_WRITE); }
};
inline FakeSd SD_MMC;
