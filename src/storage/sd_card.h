#pragma once

#include <stdint.h>

namespace storage {
bool beginSdCard();
bool sdCardMounted();
uint64_t sdCardCapacityBytes();
}
