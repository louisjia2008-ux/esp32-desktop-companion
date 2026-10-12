#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_INTERNAL = 1;
constexpr int MALLOC_CAP_8BIT = 2;
constexpr int MALLOC_CAP_SPIRAM = 4;
inline bool failPsram = false;
inline bool failAllAllocations = false;
inline void* heap_caps_malloc(size_t bytes, int caps) {
  if (failAllAllocations || (failPsram && (caps & MALLOC_CAP_SPIRAM))) return nullptr;
  return std::malloc(bytes);
}
inline void heap_caps_free(void* pointer) { std::free(pointer); }
