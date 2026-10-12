#pragma once
#include "FreeRTOS.h"
#include <Arduino.h>
#include <stdexcept>
#include <string>
using TaskFunction_t = void (*)(void*);
struct FakeTask {
  TaskFunction_t entry = nullptr;
  void* context = nullptr;
  int core = -1;
  UBaseType_t priority = 0;
  uint32_t stackSize = 0;
  std::string name;
};
using TaskHandle_t = FakeTask*;
namespace fake_task {
struct Finished {};
inline FakeTask task;
inline bool creationSucceeds = true;
inline size_t creationCalls = 0;
inline size_t delays = 0;
inline size_t budget = 0;
inline void reset() {
  task = FakeTask{};
  creationSucceeds = true;
  creationCalls = delays = budget = 0;
}
inline void run(size_t samples) {
  delays = 0;
  budget = samples;
  if (task.entry == nullptr || samples == 0) throw std::runtime_error("no task");
  try { task.entry(task.context); } catch (const Finished&) { return; }
  throw std::runtime_error("task unexpectedly returned");
}
}
inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry,
    const char* name, uint32_t stackSize, void* context, UBaseType_t priority,
    TaskHandle_t* output, BaseType_t core) {
  ++fake_task::creationCalls;
  if (!fake_task::creationSucceeds) return pdFALSE;
  fake_task::task = {entry, context, core, priority, stackSize, name};
  *output = &fake_task::task;
  return pdPASS;
}
inline void vTaskDelay(TickType_t ticks) {
  fake_arduino::nowMs += ticks;
  if (++fake_task::delays == fake_task::budget) throw fake_task::Finished{};
}
