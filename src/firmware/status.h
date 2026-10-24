#pragma once
#include <stdint.h>

namespace connex {
struct StatusColor { uint8_t red, green, blue; };

// Logical brightness, independent of the common-anode output polarity.
inline StatusColor statusColor(bool enabled, bool setup, bool connected, bool healthy, uint32_t now) {
  if (!enabled) return {255, 0, 0};
  // Setup keeps its blue pulse without home Wi-Fi; otherwise a lost connection
  // shares the same red blink as a local fault.
  if (!healthy || (!setup && !connected))
    return {static_cast<uint8_t>(now % 1000 < 500 ? 255 : 0), 0, 0};
  if (setup) {
    const uint32_t phase = now % 2000;
    const uint32_t ramp = phase <= 1000 ? phase : 2000 - phase;
    // Two-second breathing cycle. Keep a little blue at the dimmest point.
    const auto brightness = static_cast<uint8_t>(16 + 239 * ramp * ramp / 1000000);
    return {0, 0, brightness};
  }
  return {0, 255, 0};
}

inline uint8_t activeLowDuty(uint8_t brightness) { return 255 - brightness; }
}  // namespace connex
