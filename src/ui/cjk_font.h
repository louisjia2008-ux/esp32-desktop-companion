#pragma once

#include <cstdint>

namespace uifont {

// 16 px Unifont with 6655 common simplified/traditional Chinese characters,
// CJK punctuation and fullwidth forms (about 290 KiB of flash). Kept in its
// own translation unit so the large table is compiled once.
const uint8_t* cjkFont();

}  // namespace uifont
