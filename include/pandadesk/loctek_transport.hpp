#pragma once

#include <cstdint>

#include "esp_err.h"
#include "pandadesk/jiecang_transport.hpp"

namespace pandadesk::loctek_transport {

void start_if_configured(bool nvs_available);
bool is_configured();
bool is_ready();
bool height_tenths_cm(uint16_t *height);
jiecang_transport::MotionState motion_state();
uint32_t state_sequence();
esp_err_t queue_move(jiecang_transport::Direction direction, uint16_t duration_ms);

}  // namespace pandadesk::loctek_transport
