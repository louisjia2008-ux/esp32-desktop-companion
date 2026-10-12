#pragma once
#include <cstdint>
struct esp_partition_t { uint32_t size=0x640000; };
using esp_ota_img_states_t = int;
constexpr int ESP_OK=0,ESP_OTA_IMG_PENDING_VERIFY=1;
inline esp_partition_t testPartition;
inline int testBootState=2;
inline const esp_partition_t* esp_ota_get_next_update_partition(const void*) { return &testPartition; }
inline const esp_partition_t* esp_ota_get_running_partition() { return &testPartition; }
inline int esp_ota_get_state_partition(const esp_partition_t*,int* state) { *state=testBootState;return ESP_OK; }
