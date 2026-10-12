#pragma once

#include "touch_driver.h"

namespace touch {

// Maps the already-validated native AXS15231B coordinates into the logical
// display orientation without changing the proven I2C transaction.
bool toDisplayCoordinates(const TouchPoint& nativePoint,
                          TouchPoint& displayPoint);

}  // namespace touch
