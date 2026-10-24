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
    quiet(0);
  }
  Result target(uint16_t tenths, uint32_t now, uint64_t zone = 0) {
    Patch p; p.fields = Target; p.configuration.hasTarget = 1; p.configuration.temperature = tenths;
    return controller.patch(zone ? zone : id, p, now);
  }
  void quiet(uint32_t now) { Command c; assert(!controller.take(now, c)); }
  Command send(uint32_t now, uint16_t target, uint64_t zone = 0, bool pair = false) {
    Command c;
    assert(controller.take(now, c) && c.zoneId == (zone ? zone : id) && c.pairing == pair);
    assert((unsigned(c.bytes[5]) << 8 | c.bytes[6]) == target);
    controller.complete(false); // ACK and no_ack both leave the planned schedule intact.
    return c;
  }
};

void fiveCopies() {
  Fixture f;
  assert(f.target(215, 100) == Result::Ok);
  const unsigned writes = f.memory.writes;
  for (unsigned copy = 0; copy < 5; ++copy) {
    const uint32_t now = 10100 + copy * 1000;
    f.quiet(now - 1);
    const Command c = f.send(now, 215);
    assert(c.bytes[2] == copy && c.bytes[7] == 9 && c.bytes[8] == 1);
    f.quiet(now);
    assert(f.controller.queued() == (copy < 4 ? 1 : 0));
    assert(f.controller.busy(f.id) == (copy < 4));
    if (copy < 4) {
      assert(f.controller.remove(f.id) == Result::Conflict);
      assert(f.controller.pair(f.id, 30, now) == Result::Conflict);
    }
  }
  f.quiet(15100); f.quiet(25000);
  assert(f.memory.writes == writes); // Repeats/refresh never write NVS.
  f.quiet(14100 + kRefreshIntervalMs - 1);
  assert(f.send(14100 + kRefreshIntervalMs, 215).bytes[2] == 5);
  f.quiet(15100 + kRefreshIntervalMs);
  assert(!f.controller.queued() && f.memory.writes == writes);
}

void replaceInFlightAndPending() {
  Fixture f; Command old;
  assert(f.target(180, 0) == Result::Ok);
  assert(f.controller.take(10000, old) && old.bytes[6] == 180);
  assert(f.target(185, 10001) == Result::Ok);
  assert(old.bytes[6] == 180 && f.controller.queued() == 1);
  f.quiet(20001); // Old packet may finish, but cannot block admission of the new snapshot.
  f.controller.complete(false);
  f.quiet(20000);
  assert(f.send(20001, 185).bytes[2] == 1);
  const unsigned writes = f.memory.writes;
  assert(f.target(185, 20002) == Result::Ok && f.memory.writes == writes);
  f.quiet(21001); f.quiet(30001); // Even an identical edit cancels remaining copies.
  for (unsigned copy = 0; copy < 5; ++copy)
    assert(f.send(30002 + copy * 1000, 185).bytes[2] == copy + 2);
  f.quiet(35002);
  assert(!f.controller.queued());
}

void delaysFailuresAndRollover() {
  Fixture f;
  assert(f.target(220, 0) == Result::Ok);
  f.send(10000, 220);
  f.send(30000, 220); // Missed deadlines produce one copy, never a catch-up burst.
  f.quiet(30000); f.quiet(30999);
  f.memory.result = SaveResult::Rejected;
  assert(f.target(230, 30001) == Result::Unavailable);
  for (uint32_t now = 31000; now <= 33000; now += 1000) f.send(now, 220);
  f.quiet(34000);
  f.memory.result = SaveResult::Saved;
  assert(f.target(240, 35000) == Result::Ok);
  f.send(45000, 240);
  f.memory.result = SaveResult::Uncertain;
  assert(f.target(250, 45001) == Result::Uncertain);
  assert(!f.controller.queued()); f.quiet(46000); f.quiet(2 * kRefreshIntervalMs);

  Fixture wrap;
  const uint32_t start = 0xffffd800;
  assert(wrap.target(230, start) == Result::Ok);
  for (uint32_t copy = 0; copy < 5; ++copy) {
    const uint32_t now = start + 10000 + copy * 1000;
    wrap.quiet(now - 1); wrap.send(now, 230);
  }
  wrap.quiet(start + 15000);
}

void hourlyLifecycle() {
  Fixture f;
  const uint32_t hour = kRefreshIntervalMs;
  const unsigned writes = f.memory.writes;
  f.quiet(hour - 1); f.send(hour, 200); f.quiet(hour); f.quiet(hour + 1000);
  f.quiet(2 * hour - 1); f.send(2 * hour, 200);
  f.send(5 * hour, 200); // Delayed polling does not backfill the missed hours.
  f.quiet(5 * hour); f.quiet(6 * hour - 1); f.send(6 * hour, 200);
  assert(f.memory.writes == writes);

  assert(f.controller.load(f.memory.durable));
  f.quiet(6 * hour + 1); f.quiet(7 * hour);
  f.send(7 * hour + 1, 200); // Reboot starts a new full-hour wait, no startup send.
  assert(f.controller.remove(f.id) == Result::Ok);
  f.quiet(8 * hour + 1);
  assert(f.controller.create("Replacement", f.id) == Result::Ok);
  f.quiet(9 * hour); f.quiet(10 * hour - 1); f.send(10 * hour, 200);
  Snapshot unset = f.memory.durable;
  unset.zones[0].configuration = Configuration{};
  assert(f.controller.load(unset));
  f.quiet(11 * hour); f.quiet(12 * hour); // Unset zones never emit zero-temperature packets.

  Fixture wrap; const uint32_t start = 0xfffff000;
  assert(wrap.controller.load(wrap.memory.durable)); wrap.quiet(start);
  uint32_t now = start;
  for (unsigned cycle = 0; cycle < 1300; ++cycle) { // More than a full millis() cycle.
    now += hour; wrap.quiet(now - 1); wrap.send(now, 200); wrap.quiet(now);
  }
}

void refreshYieldsToUpdatesAndPairing() {
  Fixture f; const uint32_t hour = kRefreshIntervalMs;
  assert(f.target(230, hour - 500) == Result::Ok);
  f.quiet(hour); f.quiet(hour + 9499); // Never bypass the grace period with an hourly send.
  for (uint32_t copy = 0; copy < 5; ++copy) f.send(hour + 9500 + copy * 1000, 230);
  f.quiet(2 * hour + 13499);
  f.send(2 * hour + 13500, 230);
  f.quiet(2 * hour + 14500);

  Fixture paired;
  assert(paired.controller.pair(paired.id, 30, hour - 1) == Result::Ok);
  for (uint32_t copy = 0; copy < 6; ++copy) {
    const uint32_t now = hour - 1 + copy * 5000;
    paired.send(now, 200, 0, true); paired.quiet(now + 4999);
  }
  paired.quiet(hour + 29999);
  paired.quiet(2 * hour + 24998);
  paired.send(2 * hour + 24999, 200);
  paired.quiet(2 * hour + 25999); // Pairing does not create five-copy control bursts.
}

void pendingRefreshReplacementAndFairness() {
  Fixture f; uint64_t second = 0; Command c;
  const uint32_t hour = kRefreshIntervalMs;
  assert(f.controller.create("Bedroom", second) == Result::Ok); f.quiet(0);
  assert(f.target(190, hour - 10001, second) == Result::Ok);
  assert(f.controller.take(hour - 1, c) && c.zoneId == second);
  f.quiet(hour); // First zone's refresh queues behind the in-flight second zone.
  assert(f.controller.queued() == 2);
  assert(f.target(250, hour + 1) == Result::Ok && f.controller.queued() == 2);
  f.controller.complete(false); f.quiet(hour + 2);
  for (uint32_t copy = 0; copy < 4; ++copy) f.send(hour + 999 + copy * 1000, 190, second);
  f.quiet(hour + 10000);
  for (uint32_t copy = 0; copy < 5; ++copy) f.send(hour + 10001 + copy * 1000, 250);
  f.quiet(hour + 15001);

  Fixture many; uint64_t ids[kZones] = {many.id};
  for (size_t i = 1; i < kZones; ++i) assert(many.controller.create("Zone", ids[i]) == Result::Ok);
  for (size_t i = 0; i < kZones; ++i) assert(many.target(200 + i * 5, 0, ids[i]) == Result::Ok);
  for (uint32_t copy = 0; copy < 5; ++copy) {
    const uint32_t now = 10000 + copy * 1000;
    for (size_t i = 0; i < kZones; ++i) many.send(now, 200 + i * 5, ids[i]);
    many.quiet(now);
  }
  assert(!many.controller.queued());
  for (size_t i = 0; i < kZones; ++i) many.send(hour + 14000, 200 + i * 5, ids[i]);
  many.quiet(hour + 14000); many.quiet(hour + 15000);
}

void radioFaultAndRestart() {
  Fixture f; Command c; const uint32_t hour = kRefreshIntervalMs;
  assert(f.target(210, 0) == Result::Ok);
  assert(f.controller.take(10000, c)); f.controller.complete(true);
  assert(!f.controller.healthy() && !f.controller.queued());
  f.quiet(11000); f.quiet(hour + 10000);
  f.controller.setRadioReady(true);
  f.quiet(hour + 10001); f.quiet(2 * hour + 10000);
  f.send(2 * hour + 10001, 210); // Explicit recovery waits an hour, no replayed burst.

  Fixture restart;
  assert(restart.target(220, 0) == Result::Ok); restart.send(10000, 220);
  assert(restart.controller.load(restart.memory.durable));
  restart.quiet(10500); restart.quiet(11000); restart.quiet(hour + 10499);
  restart.send(hour + 10500, 220); restart.quiet(hour + 11500);
}

void standby() {
  Fixture f; Command c; const uint32_t hour = kRefreshIntervalMs;
  assert(f.controller.enabled());
  assert(f.target(210, 0) == Result::Ok);
  const auto saved = f.memory.durable;
  const auto writes = f.memory.writes;
  f.controller.setEnabled(false);
  assert(!f.controller.enabled() && !f.controller.healthy() && f.controller.storageReady());
  assert(!f.controller.queued() && !f.controller.busy(f.id));
  assert(f.target(230, 100) == Result::Unavailable);
  assert(f.controller.pair(f.id, 30, 100) == Result::Unavailable);
  f.quiet(10000); f.quiet(hour); f.quiet(3 * hour); f.quiet(0xfffffff0);
  assert(f.memory.writes == writes && !memcmp(&saved, &f.memory.durable, sizeof saved));
  f.controller.setEnabled(true);
  assert(f.controller.enabled() && f.controller.healthy());
  f.quiet(3 * hour); f.quiet(4 * hour - 1); f.send(4 * hour, 210);

  // Turning off during a five-copy burst cancels remaining copies. The driver
  // completes/cancels the in-flight packet independently of the standby policy.
  assert(f.target(215, 4 * hour + 1) == Result::Ok);
  assert(f.controller.take(4 * hour + 10001, c));
  f.controller.setEnabled(false); f.quiet(4 * hour + 20000);
  f.controller.complete(false);
  assert(!f.controller.busy(f.id) && !f.controller.queued());
  f.controller.setEnabled(true); f.quiet(4 * hour + 20001);
  f.quiet(5 * hour + 20000); f.send(5 * hour + 20001, 215);

  // Pairing never resumes; repeated setEnabled(true) must not erase new work.
  assert(f.controller.pair(f.id, 30, 6 * hour) == Result::Ok);
  assert(f.controller.take(6 * hour, c) && c.pairing);
  f.controller.setEnabled(false); f.controller.complete(false);
  f.controller.setEnabled(true); f.quiet(6 * hour + 1); f.quiet(6 * hour + 5000);
  assert(f.controller.cancelPairing(f.id) == Result::Ok);
  assert(f.target(220, 6 * hour + 6000) == Result::Ok);
  f.controller.setEnabled(true); f.send(6 * hour + 16000, 220);

  // Wake cannot mask a hardware/storage fault. Standby itself writes no NVS;
  // a new controller instance starts enabled with the existing saved settings.
  f.controller.complete(true); f.controller.setEnabled(false); f.controller.setEnabled(true);
  assert(!f.controller.healthy()); f.quiet(10 * hour);
  f.controller.setRadioReady(true);
  f.controller.setEnabled(false);
  Controller reboot(f.memory, f.random);
  assert(reboot.load(f.memory.durable)); reboot.setRadioReady(true);
  assert(reboot.enabled() && reboot.healthy() && !reboot.take(0, c));
  assert(reboot.find(f.id)->configuration.temperature == 220);
}

int main() {
  fiveCopies(); replaceInFlightAndPending(); delaysFailuresAndRollover(); hourlyLifecycle();
  refreshYieldsToUpdatesAndPairing(); pendingRefreshReplacementAndFairness(); radioFaultAndRestart(); standby();
  puts("PASS: five-copy control bursts, one-second spacing, latest-wins cancellation, hourly refresh, fairness, rollover, failures/restart, pairing isolation, standby/wake");
}
