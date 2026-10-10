#pragma once

#include <cstdint>

#include "esp_err.h"

namespace pandadesk::jiecang_transport {

enum class Direction : uint8_t { up, down, stop };
enum class MotionState : uint8_t { unknown, stopped, moving };

void start_if_configured(bool nvs_available);
bool is_ready();
bool supports_target_height();
const char *profile_name();
bool height_tenths_cm(uint16_t *height);
MotionState motion_state();
uint32_t state_sequence();
esp_err_t queue_move(Direction direction, uint16_t duration_ms);
esp_err_t queue_target_height(uint16_t height_tenths_cm);

}  // namespace pandadesk::jiecang_transport
