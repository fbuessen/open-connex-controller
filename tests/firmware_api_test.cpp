#include "api.h"
#include "json_support.h"
#include "dns.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <random>

using namespace connex;
using namespace firmware;
struct Memory : Storage {
  SaveResult next = SaveResult::Saved;
  SaveResult save(const Snapshot&) override { return next; }
};
struct Entropy : Random { uint64_t n = 1; uint64_t next() override { return n++; } };
struct Fixture {
  Memory storage; Entropy random; Controller controller{storage, random};
  Json input, output;
  uint32_t now = 100;
  Fixture() { assert(controller.initialize() == Result::Ok); controller.setRadioReady(true); }
  ApiReply call(const char* method, const char* path, const char* body = "{}") {
    input.value.clear(); output.value.clear();
    assert(parseJson(input.value, body, strlen(body)));
    return api(controller, method, path, input.value.as<JsonVariantConst>(), output.value, now, true);
  }
};
void contracts() {
  Fixture f;
  assert(f.call("GET", "/api/v1/device").status == 200);
  assert(f.output.value.size() == 3 && f.output.value["health"] == "ok");
  assert(f.call("GET", "/api/v1/zones").status == 200 && f.output.value["zones"].size() == 0);
  assert(f.call("POST", "/api/v1/zones", "{\"name\":\"Living room\"}").status == 201);
  assert(f.output.value.size() == 3 && f.output.value["configuration"].size() == 4);
  assert(f.output.value["configuration"]["target_temperature"] == 20);
  assert(f.output.value["configuration"]["unit"] == "celsius");
  assert(f.output.value["configuration"]["mode"] == "normal");
  assert(f.output.value["configuration"]["eco_offset"] == 0);
  assert(!f.output.value["prefix"].is<JsonVariant>() || f.output.value["prefix"].isNull());
  char id[32], path[64]; strcpy(id, f.output.value["id"].as<const char*>());
  snprintf(path, sizeof path, "/api/v1/zones/%s", id);
  assert(f.call("PATCH", path, "{\"name\":\"Bedroom\"}").status == 200 && f.controller.queued() == 0);
  assert(f.call("PATCH", path, "{\"target_temperature\":21}").status == 202);
  assert(f.output.value.isNull());
  assert(f.call("GET", path).status == 200 && f.output.value["configuration"]["target_temperature"] == 21);
  assert(f.call("PATCH", path, "{\"target_temperature\":21}").status == 202 && f.controller.queued() == 1);
  assert(f.call("DELETE", path).status == 409);
  Command command;
  assert(!f.controller.take(100 + kDispatchGraceMs - 1, command));
  for (f.now = 10100; f.now <= 14100; f.now += 1000) {
    assert(f.controller.take(f.now, command)); f.controller.complete(false);
  }
  char pair[80]; snprintf(pair, sizeof pair, "%s/pairing", path);
  assert(f.call("POST", pair).status == 202);
  assert(f.call("PATCH", path, "{\"name\":\"Busy\"}").status == 409);
  assert(f.call("DELETE", pair).status == 204 && !f.controller.queued());
  assert(f.call("DELETE", pair).status == 204);
  assert(f.call("DELETE", path).status == 204);
  assert(f.call("GET", path).status == 404);
  assert(f.call("GET", "/api/v1/operations").status == 404);
  assert(f.call("POST", "/api/v1/device").status == 405);
}
void newZonePairing() {
  Fixture f; Command c;
  assert(f.call("POST", "/api/v1/zones", "{\"name\":\"New zone\"}").status == 201);
  char path[80]; snprintf(path, sizeof path, "/api/v1/zones/%s/pairing", f.output.value["id"].as<const char*>());
  assert(!f.controller.queued() && !f.controller.take(f.now, c));
  assert(f.call("POST", path).status == 202 && f.output.value.isNull());
  for (unsigned send = 0; send < 6; ++send) {
    assert(f.controller.take(f.now, c) && c.pairing);
    assert(c.bytes[5] == 0 && c.bytes[6] == 200 && c.bytes[7] == 9 && c.bytes[8] == 1);
    assert(c.bytes[2] == send);
    f.controller.complete(false);
    assert(!f.controller.take(f.now + 4999, c));
    f.now += 5000;
  }
  assert(!f.controller.take(f.now, c) && !f.controller.queued()); // 30-second deadline.
}

void standbyAccess() {
  Fixture f;
  assert(f.call("POST", "/api/v1/zones", "{\"name\":\"Living room\"}").status == 201);
  char path[64], pair[80];
  snprintf(path, sizeof path, "/api/v1/zones/%s", f.output.value["id"].as<const char*>());
  snprintf(pair, sizeof pair, "%s/pairing", path);
  assert(f.call("PATCH", path, "{\"target_temperature\":21}").status == 202);
  f.controller.setEnabled(false);
  assert(f.call("GET", "/api/v1/device").status == 200);
  assert(f.output.value.size() == 3 && f.output.value["health"] == "degraded");
  assert(f.call("GET", "/api/v1/zones").status == 200 && f.output.value["zones"].size() == 1);
  assert(f.call("GET", path).status == 200 && f.output.value["configuration"]["target_temperature"] == 21);
  for (const char* body : {"{\"target_temperature\":22}", "{\"mode\":\"eco\"}", "{\"eco_offset\":-2}",
                          "{\"unit\":\"fahrenheit\",\"target_temperature\":70}",
                          "{\"name\":\"Not saved\",\"target_temperature\":22}"})
    assert(f.call("PATCH", path, body).status == 503);
  assert(f.call("GET", path).status == 200 && f.output.value["name"] == "Living room");
  assert(f.output.value["configuration"]["target_temperature"] == 21);
  assert(f.output.value["configuration"]["unit"] == "celsius");
  assert(f.call("POST", pair).status == 503);
  assert(f.call("DELETE", pair).status == 204);
  assert(!f.controller.queued());
  assert(f.call("PATCH", path, "{\"name\":\"Renamed\"}").status == 200);
  f.controller.setEnabled(true);
  assert(f.call("GET", "/api/v1/device").status == 200 && f.output.value["health"] == "ok");
  assert(!f.controller.queued());
  assert(f.call("PATCH", path, "{\"target_temperature\":22}").status == 202);
}

void eightZones() {
  static_assert(kZones == 8, "Firmware supports eight active zones");
  Fixture f;
  char paths[kZones][80];
  // Distinct maximum-length names exercise both JSON allocation and output size.
  for (size_t i = 0; i < kZones; ++i) {
    char name[65]; memset(name, 'A' + i, 64); name[64] = 0;
    char body[90]; snprintf(body, sizeof body, "{\"name\":\"%s\"}", name);
    assert(f.call("POST", "/api/v1/zones", body).status == 201);
    snprintf(paths[i], sizeof paths[i], "/api/v1/zones/%s/pairing", f.output.value["id"].as<const char*>());
  }
  assert(f.call("POST", "/api/v1/zones", "{\"name\":\"Ninth\"}").status == 409);
  assert(f.call("GET", "/api/v1/zones").status == 200);
  assert(f.output.value["zones"].size() == 8 && !f.output.value.overflowed());
  assert(measureJson(f.output.value) < 4096); // HTTP response buffer.
  for (size_t i = 0; i < kZones; ++i) assert(f.call("POST", paths[i]).status == 202);
  bool seen[kZones] = {}; Command command;
  for (size_t i = 0; i < kZones; ++i) {
    assert(f.controller.take(f.now, command) && command.pairing);
    const auto index = command.bytes[4]; assert(index < kZones && !seen[index]);
    seen[index] = true; f.controller.complete(false);
  }
  assert(!f.controller.take(f.now, command));
}

void apiDebounce() {
  Fixture f; Command c;
  assert(f.call("POST", "/api/v1/zones", "{\"name\":\"Living room\"}").status == 201);
  char path[64]; snprintf(path, sizeof path, "/api/v1/zones/%s", f.output.value["id"].as<const char*>());
  f.now = 0;
  for (const char* body : {"{\"target_temperature\":18}", "{\"target_temperature\":18.5}",
                         "{\"target_temperature\":19}", "{\"target_temperature\":19.5}"}) {
    assert(f.call("PATCH", path, body).status == 202 && f.output.value.isNull());
    assert(f.controller.queued() == 1 && !f.controller.take(f.now, c));
    f.now += 1000;
  }
  assert(f.call("GET", path).status == 200 && f.output.value["configuration"]["target_temperature"] == 19.5);
  f.now = 12000;
  assert(f.call("PATCH", path, "{\"mode\":\"off\"}").status == 422);
  assert(f.call("PATCH", path, "{\"name\":\"Renamed\"}").status == 200);
  assert(!f.controller.take(12999, c));
  assert(f.controller.take(13000, c) && c.bytes[6] == 195 && !c.pairing);
  f.controller.complete(false);
  for (uint32_t now = 14000; now <= 17000; now += 1000) {
    assert(!f.controller.take(now - 1, c));
    assert(f.controller.take(now, c) && c.bytes[6] == 195 && !c.pairing);
    f.controller.complete(false);
  }
  assert(!f.controller.take(23000, c));
  assert(!f.controller.take(17000 + kRefreshIntervalMs - 1, c));
  assert(f.controller.take(17000 + kRefreshIntervalMs, c) && c.bytes[6] == 195 && !c.pairing);
  f.controller.complete(false);
  assert(!f.controller.take(18000 + kRefreshIntervalMs, c)); // Hourly refresh is a single send.
}
void rejectedBodies() {
  Fixture f;
  assert(f.call("POST", "/api/v1/zones", "{\"name\":true}").status == 422);
  assert(f.call("POST", "/api/v1/zones", "{\"name\":\"x\\u0000y\"}").status == 422);
  assert(f.call("POST", "/api/v1/zones", "{\"name\":\"x\",\"id\":\"bad\"}").status == 422);
  assert(f.call("POST", "/api/v1/zones", "{\"name\":\"x\"}").status == 201);
  char path[64]; snprintf(path, sizeof path, "/api/v1/zones/%s", f.output.value["id"].as<const char*>());
  for (const char* body : {"{}", "{\"target_temperature\":null}", "{\"target_temperature\":true}",
         "{\"target_temperature\":\"21\"}", "{\"target_temperature\":21.1}", "{\"target_temperature\":31}",
         "{\"mode\":\"off\"}", "{\"eco_offset\":1.5}", "{\"eco_offset\":true}",
         "{\"eco_offset\":10}", "{\"unit\":\"kelvin\"}", "{\"revision\":1}"}) {
    assert(f.call("PATCH", path, body).status == 422);
    assert(!f.controller.queued());
  }
  assert(f.call("PATCH", path, "{\"target_temperature\":86,\"unit\":\"fahrenheit\",\"eco_offset\":9,\"mode\":\"eco\"}").status == 202);
  assert(f.call("PATCH", path, "{\"unit\":\"celsius\"}").status == 422);
  assert(f.call("PATCH", path, "{\"unit\":\"celsius\",\"target_temperature\":10,\"eco_offset\":-9}").status == 202);
  f.storage.next = SaveResult::Uncertain;
  const ApiReply reply = f.call("PATCH", path, "{\"target_temperature\":20}");
  assert(reply.uncertain && reply.status == 0 && !f.controller.storageReady());
  assert(!f.controller.queued());
}
void jsonBounds() {
  Json json;
  for (const char* input : {"[]", "null", "{}junk", "{}{}", "{", "{\"x\":{\"y\":{\"z\":{}}}}"})
    assert(!parseJson(json.value, input, strlen(input)));
  assert(parseJson(json.value, "{} \n", 4));
  const char zero[] = {'{','}',0}; assert(!parseJson(json.value, zero, sizeof zero));
  char large[1025]; memset(large, ' ', sizeof large); assert(!parseJson(json.value, large, sizeof large));
  for (unsigned i = 0; i < 2000; ++i) json.value["huge"].to<JsonArray>().add("a deliberately large JSON allocation");
  // Exercise the allocator directly (ArduinoJson may deduplicate equal strings).
  JsonAllocator allocator; void* block = allocator.allocate(8192);
  assert(block && !allocator.allocate(1));
  assert(!allocator.reallocate(block, 8193)); allocator.deallocate(block);
  block = allocator.allocate(1); assert(block); allocator.deallocate(block);
}
void dnsBounds() {
  uint8_t request[] = {0x12,0x34,1,0,0,1,0,0,0,0,0,0,3,'w','w','w',0,0,1,0,1};
  const uint8_t ip[] = {192,168,4,1}; uint8_t out[512];
  assert(dnsReply(request, sizeof request, ip, out) == sizeof request + 16);
  assert(out[0] == 0x12 && out[1] == 0x34 && out[7] == 1);
  assert(!memcmp(out + sizeof request + 12, ip, 4));
  for (size_t size = 0; size < sizeof request; ++size) assert(!dnsReply(request, size, ip, out));
  request[18] = 28; assert(dnsReply(request, sizeof request, ip, out) == sizeof request && out[7] == 0);
  request[12] = 0xc0; assert(!dnsReply(request, sizeof request, ip, out));
  std::mt19937 random(7);
  uint8_t bytes[513];
  for (unsigned trial = 0; trial < 30000; ++trial) {
    const size_t count = random() % sizeof bytes;
    for (size_t i = 0; i < count; ++i) bytes[i] = random();
    if (trial % 2 && count >= 12) { memset(bytes + 2, 0, 10); bytes[5] = 1; }
    assert(dnsReply(bytes, count, ip, out) <= sizeof out);
  }
}
int main() {
  contracts(); newZonePairing(); standbyAccess(); eightZones(); apiDebounce(); rejectedBodies(); jsonBounds(); dnsBounds();
  puts("PASS: firmware API contracts, JSON validation/allocation limits, DNS malformed-packet and randomized tests");
}
