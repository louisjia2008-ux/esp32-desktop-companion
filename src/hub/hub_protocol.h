#pragma once
#include <ArduinoJson.h>
#include <Arduino.h>
#include "hub_model.h"
namespace hub {
bool parsePage(JsonObject root,Page& page);
bool parseResult(JsonObject root,Result& result);
String encodeAction(const Action& action);
}
