#pragma once
#include "FreeRTOS.h"
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

struct FakeQueue {
  size_t capacity;
  size_t itemSize;
  std::deque<std::vector<uint8_t>> items;
};
using QueueHandle_t = FakeQueue*;
namespace fake_queue {
inline bool creationSucceeds = true;
inline size_t deleted = 0;
inline std::vector<std::unique_ptr<FakeQueue>> queues;
inline void reset() {
  queues.clear();
  creationSucceeds = true;
  deleted = 0;
}
}
inline QueueHandle_t xQueueCreate(UBaseType_t capacity, UBaseType_t itemSize) {
  if (!fake_queue::creationSucceeds) return nullptr;
  fake_queue::queues.emplace_back(new FakeQueue{capacity, itemSize, {}});
  return fake_queue::queues.back().get();
}
inline BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t) {
  if (queue->items.size() == queue->capacity) return pdFALSE;
  const auto* bytes = static_cast<const uint8_t*>(item);
  queue->items.emplace_back(bytes, bytes + queue->itemSize);
  return pdTRUE;
}
inline BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t) {
  if (queue->items.empty()) return pdFALSE;
  std::memcpy(item, queue->items.front().data(), queue->itemSize);
  queue->items.pop_front();
  return pdTRUE;
}
inline BaseType_t xQueueReset(QueueHandle_t queue) {
  queue->items.clear();
  return pdPASS;
}
inline void vQueueDelete(QueueHandle_t queue) {
  for (auto& owned : fake_queue::queues) {
    if (owned.get() == queue) { owned.reset(); ++fake_queue::deleted; return; }
  }
}
