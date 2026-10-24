#include "domain.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace connex;
struct Memory : Storage {
  Snapshot durable;
  SaveResult result = SaveResult::Saved;
  unsigned writes = 0;
  SaveResult save(const Snapshot& next) override {
    ++writes;
    if (result != SaveResult::Rejected) durable = next;
    return result;
  }
};
struct Entropy : Random { uint64_t n = 1; uint64_t next() override { return n++; } };
struct Fixture {
  Memory memory; Entropy random; Controller controller{memory, random}; uint64_t id = 0;
  Fixture() {
    assert(controller.initialize() == Result::Ok);
    controller.setRadioReady(true);
    assert(controller.create("Living room", id) == Result::Ok);
  }
  Result target(unsigned tenths = 210, uint32_t now = 0) {
    Patch p; p.fields = Target; p.configuration.hasTarget = 1; p.configuration.temperature = tenths;
    return controller.patch(id, p, now);
  }
  void drain(uint32_t now = kDispatchGraceMs) {
    Command c;
    for (unsigned send = 0; send < kControlSends; ++send, now += kControlIntervalMs)
      while (controller.take(now, c)) controller.complete(false);
    assert(!controller.queued());
  }
};

void encoding() {
  Zone z; z.prefix = 0x1234; z.index = 2; z.configuration.hasTarget = 1;
  z.configuration.temperature = 200;
  uint8_t bytes[10]; encode(z, 0x40, bytes);
  const uint8_t golden[] = {0x12,0x34,0x40,1,2,0,0xc8,9,1,0};
  assert(!memcmp(bytes, golden, 10));
  z.configuration.temperature = 280; encode(z, 0xff, bytes);
  assert(bytes[5] == 1 && bytes[6] == 0x18 && bytes[2] == 0xff);
  z.configuration.celsius = 0; z.configuration.eco = 1; z.configuration.offset = -9;
  encode(z, 0, bytes); assert(bytes[7] == 0 && bytes[8] == 2);
  for (int unit = 0; unit < 2; ++unit) {
    Configuration c; c.hasTarget = 1; c.celsius = unit;
    for (int t = 0; t < 1000; ++t) {
      c.temperature = t;
      const bool expected = t >= (unit ? 100 : 500) && t <= (unit ? 300 : 860) && t % 5 == 0;
      assert(validConfiguration(c) == expected);
    }
  }
  assert(validName("Séjour")); assert(!validName("\xc0\x80"));
  assert(!validName("\xed\xa0\x80")); assert(!validName("\xf4\x90\x80\x80"));
  assert(!validName("\n")); assert(!validName("   "));
}

void zoneDefaults() {
  Fixture f; Command c;
  const Configuration& config = f.controller.find(f.id)->configuration;
  assert(config.hasTarget && config.temperature == 200);
  assert(config.celsius && !config.eco && config.offset == 0);
  assert(!f.controller.queued() && !f.controller.take(60000, c));
  Controller rebooted(f.memory, f.random);
  assert(rebooted.load(f.memory.durable)); rebooted.setRadioReady(true);
  const Configuration& restored = rebooted.find(f.id)->configuration;
  assert(restored.hasTarget && restored.temperature == 200);
  assert(restored.celsius && !restored.eco && restored.offset == 0);
  assert(!rebooted.queued() && !rebooted.take(60000, c));
  assert(rebooted.pair(f.id, 30, 60001) == Result::Ok);
  assert(rebooted.take(60001, c) && c.pairing && c.zoneId == f.id);
  assert(c.bytes[5] == 0 && c.bytes[6] == 200 && c.bytes[7] == 9 && c.bytes[8] == 1);
  rebooted.complete(false);
  assert(rebooted.cancelPairing(f.id) == Result::Ok);
  assert(!rebooted.take(90001, c));

  // Loading an existing unset zone does not silently invent or transmit a target.
  Snapshot unset = f.memory.durable;
  unset.zones[0].configuration = Configuration{};
  assert(rebooted.load(unset));
  assert(!rebooted.find(f.id)->configuration.hasTarget);
  assert(rebooted.pair(f.id, 30, 0) == Result::Invalid);
  assert(!rebooted.take(60000, c));
}

void acceptance() {
  Fixture f; Command c;
  assert(!f.controller.take(0, c));  // No boot/create transmissions.
  assert(f.target() == Result::Ok);
  const unsigned writes = f.memory.writes;
  assert(f.target() == Result::Ok && f.memory.writes == writes);
  assert(f.target(220) == Result::Ok);
  assert(f.controller.queued() == 1 && !f.controller.take(kDispatchGraceMs - 1, c));
  assert(f.controller.take(kDispatchGraceMs, c) && c.bytes[6] == 220);
  assert(!f.controller.take(kDispatchGraceMs, c));
  assert(f.controller.remove(f.id) == Result::Conflict);
  f.controller.complete(false);
  assert(!f.controller.take(kDispatchGraceMs, c));
  for (size_t i = 0; i < kQueue * 2; ++i) assert(f.target() == Result::Ok);
  assert(f.controller.queued() == 1);  // Repeated edits never fill the queue.
  assert(f.controller.find(f.id)->configuration.temperature == 210);
  f.drain();
  f.memory.result = SaveResult::Rejected;
  assert(f.target(230) == Result::Unavailable);
  assert(f.controller.find(f.id)->configuration.temperature == 210 && !f.controller.queued());
  f.memory.result = SaveResult::Uncertain;
  assert(f.target(230) == Result::Uncertain);
  assert(!f.controller.storageReady() && !f.controller.take(0, c));
  assert(f.controller.load(f.memory.durable));
  assert(f.controller.find(f.id)->configuration.temperature == 230);
  assert(!f.controller.take(0, c));  // Durable configuration does not replay a queue.
}

void debounce() {
  Fixture f; Command c;
  assert(f.target(180, 100) == Result::Ok);
  assert(f.target(185, 1000) == Result::Ok);
  assert(f.target(190, 2000) == Result::Ok);
  assert(f.target(195, 3000) == Result::Ok);
  assert(f.controller.queued() == 1 && f.controller.busy(f.id));
  assert(f.controller.find(f.id)->configuration.temperature == 195);
  assert(f.memory.durable.zones[0].configuration.temperature == 195);
  assert(f.controller.remove(f.id) == Result::Conflict);
  assert(f.controller.pair(f.id, 30, 3001) == Result::Conflict);
  assert(!f.controller.take(10100, c));  // The original deadline has been replaced.
  assert(!f.controller.take(12999, c));
  assert(f.controller.take(13000, c) && c.bytes[6] == 195 && c.bytes[2] == 0 && !c.pairing);
  f.controller.complete(false);
  assert(!f.controller.take(13001, c));

  // Identical control writes still request dispatch and restart the quiet period.
  assert(f.target(195, 14000) == Result::Ok);
  const unsigned writes = f.memory.writes;
  assert(f.target(195, 23000) == Result::Ok && f.memory.writes == writes);
  Patch rename; rename.fields = Name; strcpy(rename.name, "Renamed");
  assert(f.controller.patch(f.id, rename, 32000) == Result::Ok);
  assert(f.target(196, 32900) == Result::Invalid);
  assert(!f.controller.take(32999, c));
  assert(f.controller.take(33000, c) && c.bytes[6] == 195 && c.bytes[2] == 1);
  f.controller.complete(false);  // Neither rename nor rejected update reset the timer.

  // Every accepted control field merges into the same complete configuration.
  assert(f.target(200, 34000) == Result::Ok);
  Patch eco; eco.fields = Mode | Offset; eco.configuration.eco = 1; eco.configuration.offset = -3;
  assert(f.controller.patch(f.id, eco, 35000) == Result::Ok);
  Patch unit; unit.fields = Target | Unit; unit.configuration.hasTarget = 1;
  unit.configuration.temperature = 700; unit.configuration.celsius = 0;
  assert(f.controller.patch(f.id, unit, 36000) == Result::Ok);
  assert(f.controller.queued() == 1 && !f.controller.take(45999, c));
  assert(f.controller.take(46000, c));
  assert(c.bytes[5] == 2 && c.bytes[6] == 188 && c.bytes[7] == 6 && c.bytes[8] == 2);
  f.controller.complete(false);

  // Millisecond deadlines remain correct across rollover, including a re-arm.
  Fixture wrap; const uint32_t start = 0xfffff000;
  assert(wrap.target(180, start) == Result::Ok);
  assert(wrap.target(185, start + 8000) == Result::Ok);
  assert(!wrap.controller.take(start + 10000, c));
  assert(!wrap.controller.take(start + 17999, c));
  assert(wrap.controller.take(start + 18000, c) && c.bytes[6] == 185);
  wrap.controller.complete(false);
}

void debounceIsolationAndFailures() {
  Fixture f; Command c; uint64_t second = 0;
  assert(f.controller.create("Second", second) == Result::Ok);
  Patch p; p.fields = Target; p.configuration.hasTarget = 1; p.configuration.temperature = 200;
  assert(f.target(180, 0) == Result::Ok);
  assert(f.controller.patch(second, p, 1000) == Result::Ok);
  assert(f.target(185, 9000) == Result::Ok);
  assert(!f.controller.take(10999, c));
  assert(f.controller.take(11000, c) && c.zoneId == second && c.bytes[6] == 200);
  f.controller.complete(false);  // An earlier, still-debouncing queue entry cannot block this zone.
  assert(f.controller.pair(second, 30, 11001) == Result::Conflict);
  for (uint32_t now = 12000; now <= 15000; now += 1000) {
    assert(f.controller.take(now, c) && c.zoneId == second); f.controller.complete(false);
  }
  assert(f.controller.pair(second, 30, 15001) == Result::Ok);
  assert(f.controller.take(15001, c) && c.zoneId == second && c.pairing);
  f.controller.complete(false);  // Pairing is deliberately immediate, not debounced.
  assert(f.controller.cancelPairing(second) == Result::Ok);
  assert(f.controller.queued() == 1);
  assert(!f.controller.take(18999, c));
  assert(f.controller.take(19000, c) && c.zoneId == f.id && c.bytes[6] == 185);

  // In-flight bytes stay immutable. A later edit starts a new grace period.
  assert(f.target(190, 19001) == Result::Ok);
  assert(f.target(195, 19002) == Result::Ok);
  assert(c.bytes[6] == 185 && f.controller.queued() == 1);
  assert(!f.controller.take(29002, c));  // Radio still busy.
  f.controller.complete(false);
  assert(f.target(200, 29003) == Result::Ok);  // Re-arm even if the queued snapshot was due.
  assert(!f.controller.take(39002, c));
  assert(f.controller.take(39003, c) && c.bytes[6] == 200);
  f.controller.complete(false);

  Fixture failed;
  assert(failed.target(180, 0) == Result::Ok);
  failed.memory.result = SaveResult::Rejected;
  assert(failed.target(185, 9000) == Result::Unavailable);
  assert(failed.controller.find(failed.id)->configuration.temperature == 180);
  assert(failed.controller.take(10000, c) && c.bytes[6] == 180);
  failed.controller.complete(false);  // Failed saves leave the prior snapshot/deadline intact.
  failed.memory.result = SaveResult::Saved;
  assert(failed.target(190, 11000) == Result::Ok);
  failed.memory.result = SaveResult::Uncertain;
  assert(failed.target(195, 12000) == Result::Uncertain);
  assert(!failed.controller.queued() && !failed.controller.take(30000, c));
  assert(failed.controller.load(failed.memory.durable));
  assert(!failed.controller.queued() && !failed.controller.take(40000, c));

  Fixture reset;
  assert(reset.target(210, 0) == Result::Ok);
  assert(reset.controller.load(reset.memory.durable));
  assert(!reset.controller.take(10000, c));  // Reboot never replays pending sends.
  assert(reset.target(215, 11000) == Result::Ok);
  reset.controller.setRadioReady(false); reset.controller.setRadioReady(true);
  assert(!reset.controller.take(21000, c));  // Radio recovery also never replays work.
}

void zonesAndValidation() {
  Fixture f; uint64_t extra;
  const auto oldPrefix = f.controller.find(f.id)->prefix;
  assert(f.controller.remove(f.id) == Result::Ok);
  assert(f.controller.create("Kitchen", extra) == Result::Ok);
  assert(extra != f.id && oldPrefix != f.controller.find(extra)->prefix);
  assert(f.controller.snapshot().retiredCount == 1);
  for (size_t i = 1; i < kZones; ++i) assert(f.controller.create("Zone", extra) == Result::Ok);
  assert(f.controller.create("Overflow", extra) == Result::Conflict);
  Snapshot corrupt = f.controller.snapshot(); corrupt.zones[0].active = 2;
  assert(!validSnapshot(corrupt));
  corrupt = f.controller.snapshot(); corrupt.zones[0].name[0] = 0;
  assert(!validSnapshot(corrupt));
  char text[32]; formatId("zone_", extra, text, sizeof text);
  uint64_t parsed = 0; assert(parseZoneId(text, parsed) && parsed == extra);
  assert(!parseZoneId("zone_0000000000000000", parsed));
  assert(!parseZoneId("zone_000000000000000g", parsed));
  Fixture g;
  Patch patch; patch.fields = Unit; patch.configuration.celsius = 0;
  assert(g.controller.patch(g.id, patch, 0) == Result::Invalid);
  assert(g.target() == Result::Ok); g.drain();
  assert(g.controller.patch(g.id, patch, 0) == Result::Invalid);  // No implicit conversion.
  patch.fields |= Target; patch.configuration.hasTarget = 1; patch.configuration.temperature = 700;
  assert(g.controller.patch(g.id, patch, 0) == Result::Ok);
  g.drain(); g.controller.setRadioReady(false);
  assert(g.target(710) == Result::Unavailable);
  patch = {}; patch.fields = Name; strcpy(patch.name, "Rename without RF");
  assert(g.controller.patch(g.id, patch, 0) == Result::Ok);
  assert(!g.controller.queued());
}

void pairing() {
  Fixture f; Command c;
  assert(f.controller.pair(f.id, 0, 0) == Result::Invalid);
  assert(f.controller.pair(f.id, 61, 0) == Result::Invalid);
  assert(f.target() == Result::Ok);
  assert(f.controller.pair(f.id, 30, 0) == Result::Conflict); f.drain();
  const uint32_t start = 0xfffff000;
  assert(f.controller.pair(f.id, 30, start) == Result::Ok);
  assert(f.target(220) == Result::Conflict);
  assert(f.controller.take(start, c) && c.pairing); f.controller.complete(false); // no_ack is not a local fault.
  assert(!f.controller.take(start + 4999, c));
  assert(f.controller.take(start + 5000, c) && c.pairing); f.controller.complete(false);
  assert(!f.controller.take(start + 9999, c));
  assert(f.controller.take(start + 10000, c) && c.pairing); f.controller.complete(false);
  assert(f.controller.take(start + 25000, c)); f.controller.complete(false);
  assert(!f.controller.take(start + 25001, c));  // No catch-up burst.
  assert(!f.controller.take(start + 29999, c));
  assert(!f.controller.take(start + 30000, c));
  assert(f.controller.pair(f.id, 30, 0) == Result::Ok);
  assert(f.controller.take(25000, c)); f.controller.complete(false);
  assert(!f.controller.take(25001, c)); // A delayed first send cannot cause a catch-up repeat.
  assert(!f.controller.take(30000, c));
  assert(f.controller.pair(f.id, 30, 100) == Result::Ok);
  assert(f.controller.cancelPairing(f.id) == Result::Ok && !f.controller.queued());
  assert(f.controller.pair(f.id, 30, 100) == Result::Ok);
  assert(f.controller.take(100, c));
  assert(f.controller.cancelPairing(f.id) == Result::Ok);
  assert(f.controller.remove(f.id) == Result::Conflict);  // In-flight remains in-flight.
  f.controller.complete(false);
  assert(f.controller.pair(f.id, 30, 100) == Result::Ok);
  assert(f.controller.take(100, c)); f.controller.complete(true);
  assert(!f.controller.healthy() && !f.controller.take(20000, c));
}

void fairnessAndExhaustion() {
  Fixture f; uint64_t second = 0;
  assert(f.controller.create("Second", second) == Result::Ok);
  Patch p; p.fields = Target; p.configuration.hasTarget = 1; p.configuration.temperature = 200;
  assert(f.target() == Result::Ok && f.controller.patch(second, p, 0) == Result::Ok); f.drain();
  assert(f.controller.pair(f.id, 30, 0) == Result::Ok && f.controller.pair(second, 30, 0) == Result::Ok);
  Command a, b;
  assert(f.controller.take(0, a)); f.controller.complete(false);
  assert(f.controller.take(1, b)); f.controller.complete(false);
  assert(a.zoneId != b.zoneId);
  assert(f.controller.take(5000, a)); f.controller.complete(false);
  assert(f.controller.take(5001, b)); f.controller.complete(false);
  assert(a.zoneId != b.zoneId);
  f.controller.cancelPairing(f.id); f.controller.cancelPairing(second);
  uint8_t previous = 0;
  for (unsigned i = 0; i < 300; ++i) {
    assert(f.target() == Result::Ok && f.controller.take(kDispatchGraceMs, a));
    if (i) assert(a.bytes[2] == uint8_t(previous + 1));
    previous = a.bytes[2]; f.controller.complete(false);
  }
  Fixture g;
  uint64_t id = g.id;
  for (size_t i = 0; i < kRetired; ++i) {
    assert(g.controller.remove(id) == Result::Ok);
    const Result created = g.controller.create("Replacement", id);
    assert(created == (i + 1 == kRetired ? Result::Conflict : Result::Ok));
  }
  assert(validSnapshot(g.controller.snapshot()));
}

void buttonAndNetwork() {
  using E = ButtonEvent;
  SetupButton b;
  assert(b.sample(true, 100) == E::None); assert(b.sample(true, 130) == E::None);
  assert(b.sample(true, 5099) == E::None); assert(b.sample(true, 5100) == E::Reset);
  assert(b.sample(true, 50000) == E::None);
  assert(b.sample(false, 50001) == E::None); assert(b.sample(false, 50031) == E::None);
  assert(b.sample(true, 60000) == E::None); assert(b.sample(true, 60030) == E::None);
  assert(b.sample(true, 65000) == E::Reset);
  SetupButton wrap; const uint32_t start = 0xfffff000;
  assert(wrap.sample(true, start) == E::None); assert(wrap.sample(true, start + 30) == E::None);
  assert(wrap.sample(true, start + 5000) == E::Reset);
  SetupButton shortPress; shortPress.sample(true, 0); shortPress.sample(true, 30);
  assert(shortPress.sample(false, 4999) == E::None);
  assert(shortPress.sample(false, 5029) == E::Toggle); // Release debounce doesn't make a long hold.
  assert(shortPress.sample(false, 6000) == E::None);
  SetupButton delayed; delayed.sample(true, 0); delayed.sample(true, 30);
  assert(delayed.sample(false, 5000) == E::None);
  assert(delayed.sample(false, 5030) == E::Reset); // Threshold crossed between loop samples.
  assert(delayed.sample(false, 6000) == E::None);
  SetupButton bounce;
  assert(bounce.sample(true, 0) == E::None); assert(bounce.sample(false, 10) == E::None);
  assert(bounce.sample(false, 40) == E::None); // Short noise cannot toggle power.
  assert(bounce.sample(true, 100) == E::None); assert(bounce.sample(true, 130) == E::None);
  assert(bounce.sample(false, 200) == E::None); assert(bounce.sample(true, 210) == E::None);
  assert(bounce.sample(false, 220) == E::None); assert(bounce.sample(false, 249) == E::None);
  assert(bounce.sample(false, 250) == E::Toggle); assert(bounce.sample(false, 500) == E::None);
  assert(bounce.sample(true, 600) == E::None); assert(bounce.sample(true, 630) == E::None);
  assert(bounce.sample(false, 800) == E::None); assert(bounce.sample(false, 830) == E::Toggle);
  SetupButton wrapShort; wrapShort.sample(true, 0xfffffff0); wrapShort.sample(true, 14);
  assert(wrapShort.sample(false, 100) == E::None); assert(wrapShort.sample(false, 130) == E::Toggle);

  NetworkPolicy n; n.state = NetworkState::Setup;
  assert(n.submit(start)); const auto generation = n.generation;
  assert(!n.submit(start + 1)); assert(!n.expired(start + 29999)); assert(n.expired(start + 30000));
  assert(n.saved(generation, start + 10));
  assert(n.state == NetworkState::Grace && !n.failed(generation));
  assert(!n.submit(start + 20)); assert(n.deadline == start + 5010);
  n.reset(); assert(!n.saved(generation, start + 30) && !n.submit(start + 31));
  n.resetComplete(false); assert(n.state == NetworkState::Recovery && !n.submit(0));
  n.resetComplete(true); assert(n.submit(0)); assert(n.failed(n.generation));
  assert(n.state == NetworkState::Setup);
  n.state = NetworkState::Configured; assert(n.submit(1));
  assert(n.state == NetworkState::Editing && !n.submit(2));
  assert(n.failed(n.generation) && n.state == NetworkState::Configured);
}

int main() {
  encoding(); zoneDefaults(); acceptance(); debounce(); debounceIsolationAndFailures();
  zonesAndValidation(); pairing(); fairnessAndExhaustion(); buttonAndNetwork();
  puts("PASS: firmware encoding, validation, zone defaults, 10-second per-zone debounce, storage/queue atomicity, identities, 5-second pairing, button, Wi-Fi policy");
}
