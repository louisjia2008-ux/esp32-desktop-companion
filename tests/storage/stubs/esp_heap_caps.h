#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_INTERNAL = 1;
constexpr int MALLOC_CAP_8BIT = 2;
inline void* heap_caps_malloc(size_t bytes, int) { return std::malloc(bytes); }
inline void heap_caps_free(void* pointer) { std::free(pointer); }
