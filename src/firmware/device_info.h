#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <strings.h>

namespace firmware {
constexpr const char* kVersion = "0.2.9";
constexpr const char* hostname = "open-connex";

// Discovery name is friendly; hotspot suffix and public IDs remain device-specific.
inline void formatSetupSsid(uint64_t deviceId, char* out, size_t capacity) {
  snprintf(out, capacity, "OpenConnex-Setup-%08lx", static_cast<unsigned long>(deviceId >> 32));
}
inline bool allowedHttpHost(const char* host, const char* stationIp) {
  if (!strcmp(host, "192.168.4.1") || !strcmp(host, "192.168.4.1:80")) return true;
  char allowed[48];
  for (const char* suffix : {"", ".local", ":80", ".local:80"}) {
    snprintf(allowed, sizeof allowed, "%s%s", hostname, suffix);
    if (!strcasecmp(host, allowed)) return true;
  }
  if (!strcmp(stationIp, "0.0.0.0")) return false;
  snprintf(allowed, sizeof allowed, "%s:80", stationIp);
  return !strcmp(host, stationIp) || !strcmp(host, allowed);
}
}
