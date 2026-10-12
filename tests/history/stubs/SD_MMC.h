#pragma once
#include "FS.h"
class FakeSd {
 public:
  std::string root;
  bool failMkdir = false;
  std::string path(const char* name) const { return root + name; }
  bool exists(const char* name) { return std::filesystem::exists(path(name)); }
  bool mkdir(const char* name) {
    return !failMkdir && std::filesystem::create_directory(path(name));
  }
  File open(const char* name, const char* mode) { return File(path(name), mode); }
};
inline FakeSd SD_MMC;
