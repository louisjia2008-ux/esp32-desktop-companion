#pragma once

// OFFLINE host fixtures only. These stubs do not access physical hardware and
// their values must never be reported as actual sensor measurements.
#include <stdint.h>
#include <deque>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mock {
using TaskFunction = void (*)(void*);
struct Task { TaskFunction function; std::string name; uint32_t stack; int core; };
struct Stop {};
extern uint32_t now;
extern uint32_t scenarioStart;
extern uint32_t runStart;
extern uint32_t runDuration;
extern int criticalDepth;
extern bool running;
extern std::string failTask;
extern std::vector<Task> tasks;
extern std::function<void()> onAdvance;
extern std::deque<uint8_t> uartBytes;
extern int8_t uartRx;
extern int8_t uartTx;
extern uint32_t uartBeginCalls;
extern uint32_t uartBaud;
extern uint32_t uartRxBuffer;
extern bool uartValid;
extern std::function<void(int)> uartError;
extern bool busInitialized;
extern bool wireBeginSucceeds;
extern uint32_t wireBeginCalls;
extern uint32_t wireClock;
extern uint16_t wireTimeout;
extern uint8_t wireProbeResult;
extern int wireSda;
extern int wireScl;
extern std::vector<uint32_t> probeTimes;
extern std::vector<std::string> commands;
extern std::vector<uint32_t> stopTimes;
extern uint32_t startAt;
extern uint32_t lastEmissionAt;
extern uint32_t measurements;
extern bool neverReady;
extern uint16_t variant;
extern uint16_t fixtureCo2;
extern std::function<int16_t()> readyError;
extern std::function<int16_t()> measurementError;

inline uint32_t elapsed() { return uint32_t(now - scenarioStart); }
inline void hardwareCall() {
  if (criticalDepth != 0) throw std::runtime_error("hardware called under snapshot lock");
}
void advance(uint32_t milliseconds);
void run(const char* name, uint32_t duration);
}  // namespace mock
