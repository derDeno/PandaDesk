#pragma once

#include <cstdint>

namespace pandadesk::status_led {
inline constexpr uint8_t kDefaultBrightness = 50;

// The configured brightness is the peak for both animated states.
constexpr uint8_t level(uint32_t elapsed_ms, uint8_t brightness, bool access_point,
                        bool wifi_connected, bool desk_connected) {
  if (access_point) return elapsed_ms % 250 < 125 ? brightness : 0;
  if (wifi_connected && desk_connected) return brightness;
  const uint32_t phase = elapsed_ms % 3000;
  const uint32_t ramp = phase < 1500 ? phase : 3000 - phase;
  return static_cast<uint8_t>(brightness * ramp / 1500);
}

void set_brightness(uint8_t brightness);
}  // namespace pandadesk::status_led
