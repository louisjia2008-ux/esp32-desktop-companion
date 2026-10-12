#pragma once

namespace sensors {
// Opt-in, single-sensor diagnostics on I2C controller 1. Does not use touch Wire.
void beginScd41Diagnostics();
}  // namespace sensors
