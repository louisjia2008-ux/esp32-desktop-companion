#pragma once

#include <stdint.h>

namespace sensors {
// This is an explicit, opt-in receive-only experiment on the documented
// candidate P1 RX route. It does not establish the SC board's TX mapping.
void beginRadarDiagnostics();
void serviceRadarDiagnostics(uint32_t nowMs);
}  // namespace sensors
