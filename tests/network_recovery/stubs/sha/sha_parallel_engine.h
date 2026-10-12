#pragma once
#include <cstddef>
constexpr int SHA1 = 0;
extern "C" void esp_sha(int, const unsigned char*, size_t, unsigned char*);
