#include "device_info.h"
#include <assert.h>

int main() {
  using namespace firmware;
  assert(!strcmp(hostname, "open-connex"));
  char ssid[32];
  // Keep device identities fictional in test fixtures.
  formatSetupSsid(0xabcdef0123456789ULL, ssid, sizeof ssid);
  assert(!strcmp(ssid, "OpenConnex-Setup-abcdef01"));
  assert(strlen(ssid) == 25); // Fits the existing 32-byte buffer and Wi-Fi SSID limit.
  formatSetupSsid(0x0000123456789abcULL, ssid, sizeof ssid);
  assert(!strcmp(ssid, "OpenConnex-Setup-00001234"));
  for (const char* host : {"open-connex", "open-connex.local", "open-connex:80",
                           "open-connex.local:80", "OPEN-CONNEX.LOCAL", "192.168.4.1",
                           "192.168.4.1:80", "192.168.1.20", "192.168.1.20:80"})
    assert(allowedHttpHost(host, "192.168.1.20"));
  for (const char* host : {"connex-abcdef0123456789.local", "open-connex.local.attacker.test",
                           "open-connex.local:8080", "open-connex.local@attacker.test",
                           "192.168.1.21", "", "0.0.0.0"})
    assert(!allowedHttpHost(host, "192.168.1.20"));
  assert(!allowedHttpHost("0.0.0.0", "0.0.0.0"));
  assert(!allowedHttpHost("0.0.0.0:80", "0.0.0.0"));
  assert(allowedHttpHost("open-connex.local", "0.0.0.0"));
  puts("PASS: hostname, setup SSID, HTTP Host allowlist");
}
