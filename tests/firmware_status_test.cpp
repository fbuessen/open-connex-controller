#include "status.h"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>

using namespace connex;
void expect(StatusColor c, unsigned red, unsigned green, unsigned blue) {
  assert(c.red == red && c.green == green && c.blue == blue);
}
int main() {
  // Standby overrides setup, lost Wi-Fi, and faults.
  for (bool setup : {false, true})
    for (bool connected : {false, true})
      for (bool healthy : {false, true})
        expect(statusColor(false, setup, connected, healthy, 700), 255, 0, 0);
  expect(statusColor(true, false, true, true, 0), 0, 255, 0);
  // Lost Wi-Fi and local faults have identical 500 ms on/off red timing.
  for (uint32_t t : {0u, 499u, 500u, 999u, 1000u, 1499u, 1500u, 1999u, 2000u,
                     0xfffffff0u, 0xffffffffu}) {
    const unsigned red = t % 1000 < 500 ? 255 : 0;
    expect(statusColor(true, false, false, true, t), red, 0, 0);
    for (bool setup : {false, true})
      for (bool connected : {false, true})
        expect(statusColor(true, setup, connected, false, t), red, 0, 0);
    expect(statusColor(true, false, true, true, t), 0, 255, 0);
  }
  expect(statusColor(true, true, false, true, 0), 0, 0, 16);
  expect(statusColor(true, true, false, true, 1000), 0, 0, 255);
  expect(statusColor(true, true, true, true, 2000), 0, 0, 16); // Still setup during grace.
  for (unsigned t = 1; t <= 1000; ++t) {
    const auto a = statusColor(true, true, false, true, t - 1);
    const auto b = statusColor(true, true, false, true, t);
    assert(b.blue >= a.blue && b.blue - a.blue <= 1);
    expect(statusColor(true, true, false, true, 2000 - t), 0, 0, b.blue);
  }
  expect(statusColor(true, false, true, false, 0), 255, 0, 0);
  expect(statusColor(true, true, false, false, 500), 0, 0, 0); // Fault overrides setup.
  expect(statusColor(true, false, true, false, 1000), 255, 0, 0);
  for (uint32_t t : {0xfffffff0u, 0xffffffffu, 0u, 20u}) {
    const auto c = statusColor(true, true, false, true, t);
    assert(c.red == 0 && c.green == 0 && c.blue >= 16);
  }
  assert(activeLowDuty(0) == 255 && activeLowDuty(255) == 0 && activeLowDuty(100) == 155);
  puts("PASS: standby/setup light priorities, matching Wi-Fi-loss/fault red blink, blue breathing, common-anode PWM polarity");
}
