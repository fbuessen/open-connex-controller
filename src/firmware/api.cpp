#include "api.h"
#include "device_info.h"
#include <cmath>
#include <cstring>
#include <cstdio>

namespace firmware {
using namespace connex;
namespace {
void zoneJson(const Zone& z, JsonObject out) {
  char id[32]; formatId("zone_", z.id, id, sizeof id);
  out["id"] = id; out["name"] = z.name;
  JsonObject config = out["configuration"].to<JsonObject>();
  if (z.configuration.hasTarget) config["target_temperature"] = z.configuration.temperature / 10.0;
  else config["target_temperature"] = nullptr;
  config["mode"] = z.configuration.eco ? "eco" : "normal";
  config["eco_offset"] = z.configuration.offset;
  config["unit"] = z.configuration.celsius ? "celsius" : "fahrenheit";
}
ApiReply error(JsonDocument& out, int status, const char* code, const char* message) {
  out.clear(); out["error"]["code"] = code; out["error"]["message"] = message;
  return {status, false, {}};
}
ApiReply result(JsonDocument& out, Result result, int success) {
  switch (result) {
    case Result::Ok: return {success, false, {}};
    case Result::Invalid: return error(out, 422, "invalid_field", "Invalid or unsupported field value");
    case Result::Missing: return error(out, 404, "not_found", "Zone not found");
    case Result::Conflict: return error(out, 409, "conflict", "Zone is busy or capacity is exhausted");
    case Result::Full: return error(out, 429, "queue_full", "Dispatch queue is full");
    case Result::Uncertain: return {0, true, {}};
    default: return error(out, 503, "unavailable", "Controller service is unavailable");
  }
}
bool patchJson(JsonObjectConst object, Patch& patch) {
  if (!object.size()) return false;
  for (JsonPairConst pair : object) {
    const char* key = pair.key().c_str();
    JsonVariantConst v = pair.value();
    if (!strcmp(key, "name")) {
      patch.fields |= Name;
      if (!stringField(v, patch.name, sizeof patch.name)) return false;
    } else if (!strcmp(key, "target_temperature")) {
      patch.fields |= Target;
      if (v.is<bool>() || !v.is<double>()) return false;
      const double value = v.as<double>();
      if (!std::isfinite(value) || value < 10 || value > 86 || value * 2 != std::floor(value * 2)) return false;
      patch.configuration.hasTarget = 1; patch.configuration.temperature = static_cast<uint16_t>(value * 10);
    } else if (!strcmp(key, "eco_offset")) {
      patch.fields |= Offset;
      if (v.is<bool>() || !v.is<int>() || v.as<int>() < -9 || v.as<int>() > 9) return false;
      patch.configuration.offset = v.as<int>();
    } else if (!strcmp(key, "mode")) {
      patch.fields |= Mode;
      char mode[8]; if (!stringField(v, mode, sizeof mode)) return false;
      if (!strcmp(mode, "normal")) patch.configuration.eco = 0;
      else if (!strcmp(mode, "eco")) patch.configuration.eco = 1;
      else return false;
    } else if (!strcmp(key, "unit")) {
      patch.fields |= Unit;
      char unit[12]; if (!stringField(v, unit, sizeof unit)) return false;
      if (!strcmp(unit, "celsius")) patch.configuration.celsius = 1;
      else if (!strcmp(unit, "fahrenheit")) patch.configuration.celsius = 0;
      else return false;
    } else return false;
  }
  return true;
}
}
bool stringField(JsonVariantConst value, char* out, size_t size) {
  if (!value.is<const char*>()) return false;
  JsonString s = value.as<JsonString>();
  if (s.size() >= size || strlen(s.c_str()) != s.size()) return false;
  memcpy(out, s.c_str(), s.size() + 1); return true;
}
ApiReply api(Controller& controller, const char* method, const char* path,
             JsonVariantConst input, JsonDocument& out, uint32_t now, bool networkReady) {
  out.clear();
  const bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
  const bool patch = !strcmp(method, "PATCH"), del = !strcmp(method, "DELETE");
  if (!strcmp(path, "/api/v1/device")) {
    if (!get) return error(out, 405, "method_not_allowed", "Method not allowed");
    char id[40]; formatId("controller_", controller.snapshot().deviceId, id, sizeof id);
    out["id"] = id; out["firmware_version"] = kVersion;
    out["health"] = controller.healthy() && networkReady ? "ok" : "degraded";
    return {200, false, {}};
  }
  if (!controller.storageReady()) return error(out, 503, "storage_unavailable", "Storage recovery required");
  if (!strcmp(path, "/api/v1/zones")) {
    if (get) {
      auto zones = out["zones"].to<JsonArray>();
      for (const auto& z : controller.snapshot().zones) if (z.active) zoneJson(z, zones.add<JsonObject>());
      return {200, false, {}};
    }
    if (!post) return error(out, 405, "method_not_allowed", "Method not allowed");
    char name[65];
    if (!input.is<JsonObjectConst>() || input.size() != 1 || !stringField(input["name"], name, sizeof name))
      return error(out, 422, "invalid_field", "Supply only a nonempty name");
    uint64_t id = 0;
    ApiReply reply = result(out, controller.create(name, id), 201);
    if (reply.status == 201) {
      zoneJson(*controller.find(id), out.to<JsonObject>());
      char identifier[32]; formatId("zone_", id, identifier, sizeof identifier);
      snprintf(reply.location, sizeof reply.location, "/api/v1/zones/%s", identifier);
    }
    return reply;
  }
  constexpr const char* base = "/api/v1/zones/";
  if (strncmp(path, base, strlen(base))) return error(out, 404, "not_found", "Resource not found");
  const char* tail = path + strlen(base);
  if (strlen(tail) >= 40) return error(out, 404, "not_found", "Resource not found");
  char identifier[40]; strcpy(identifier, tail);
  char* suffix = strchr(identifier, '/');
  if (suffix) *suffix++ = 0;
  uint64_t id = 0;
  if (!parseZoneId(identifier, id) || (suffix && strcmp(suffix, "pairing")))
    return error(out, 404, "not_found", "Resource not found");
  const Zone* zone = controller.find(id);
  if (!zone) return result(out, Result::Missing, 200);
  if (suffix) {
    if (del) return result(out, controller.cancelPairing(id), 204);
    if (!post) return error(out, 405, "method_not_allowed", "Method not allowed");
    if (!input.is<JsonObjectConst>()) return error(out, 422, "invalid_field", "Expected an object");
    uint32_t seconds = 30;
    if (input.size()) {
      if (input.size() != 1 || !input["duration_seconds"].is<unsigned>() || input["duration_seconds"].is<bool>())
        return error(out, 422, "invalid_field", "Invalid pairing duration");
      seconds = input["duration_seconds"].as<unsigned>();
    }
    return result(out, controller.pair(id, seconds, now), 202);
  }
  if (get) { zoneJson(*zone, out.to<JsonObject>()); return {200, false, {}}; }
  if (del) return result(out, controller.remove(id), 204);
  if (patch) {
    Patch values;
    if (!input.is<JsonObjectConst>() || !patchJson(input.as<JsonObjectConst>(), values))
      return error(out, 422, "invalid_field", "Unknown field or invalid value");
    ApiReply reply = result(out, controller.patch(id, values, now), values.controls() ? 202 : 200);
    if (reply.status == 200) zoneJson(*controller.find(id), out.to<JsonObject>());
    return reply;
  }
  return error(out, 405, "method_not_allowed", "Method not allowed");
}
}
