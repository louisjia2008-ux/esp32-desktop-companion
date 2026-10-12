#include "sd_card.h"

#include <Arduino.h>
#include <SD_MMC.h>

#include "board_config.h"

namespace storage {
namespace {
bool mounted = false;
uint64_t capacity = 0;
}

bool beginSdCard() {
  if (mounted) return true;
  if (!SD_MMC.setPins(board::kSdClock, board::kSdCommand, board::kSdData0) ||
      !SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5)) {
    Serial.printf("SD_CARD mounted=false clk=%d cmd=%d d0=%d format=false\n",
                  board::kSdClock, board::kSdCommand, board::kSdData0);
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    SD_MMC.end();
    Serial.println("SD_CARD mounted=false reason=no_card");
    return false;
  }
  mounted = true;
  capacity = SD_MMC.cardSize();
  Serial.printf("SD_CARD mounted=true capacity_bytes=%llu total_bytes=%llu "
                "used_bytes=%llu clk=%d cmd=%d d0=%d mode=1bit\n",
                (unsigned long long)capacity,
                (unsigned long long)SD_MMC.totalBytes(),
                (unsigned long long)SD_MMC.usedBytes(), board::kSdClock,
                board::kSdCommand, board::kSdData0);
  return true;
}

bool sdCardMounted() { return mounted; }
uint64_t sdCardCapacityBytes() { return capacity; }
}
