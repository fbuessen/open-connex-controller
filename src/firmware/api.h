#pragma once
#include "domain.h"
#include <ArduinoJson.h>
namespace firmware {
struct ApiReply { int status = 500; bool uncertain = false; char location[64] = {}; };
// Pure HTTP contract adapter. Caller serializes access to the controller.
ApiReply api(connex::Controller& controller, const char* method, const char* path,
             JsonVariantConst input, JsonDocument& output, uint32_t now, bool networkReady);
bool stringField(JsonVariantConst value, char* out, size_t size);
}
