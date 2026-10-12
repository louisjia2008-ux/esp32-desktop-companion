#pragma once
#include <stddef.h>
#include <stdint.h>

namespace sensors {
// Initialize on setup before either diagnostic task is started.
bool beginDiagnosticLog();
// Worker/report tasks only: serializes and paces each line globally.
size_t writeDiagnosticLine(const uint8_t* bytes, size_t length);
}
