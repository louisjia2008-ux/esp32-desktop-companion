#include "touch_transform.h"

#include "board_config.h"

namespace touch {

bool toDisplayCoordinates(const TouchPoint& nativePoint,
                          TouchPoint& displayPoint) {
  switch (board::kDisplayRotation) {
    case 1:
      displayPoint.x = nativePoint.y;
      displayPoint.y =
          static_cast<uint16_t>(board::kPanelWidth - 1 - nativePoint.x);
      break;
    case 2:
      displayPoint.x =
          static_cast<uint16_t>(board::kPanelWidth - 1 - nativePoint.x);
      displayPoint.y =
          static_cast<uint16_t>(board::kPanelHeight - 1 - nativePoint.y);
      break;
    case 3:
      displayPoint.x =
          static_cast<uint16_t>(board::kPanelHeight - 1 - nativePoint.y);
      displayPoint.y = nativePoint.x;
      break;
    default:
      displayPoint = nativePoint;
      break;
  }
  return displayPoint.x < board::kDisplayWidth &&
         displayPoint.y < board::kDisplayHeight;
}

}  // namespace touch
