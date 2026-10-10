#pragma once

#include "driver/gpio.h"

namespace pandadesk::pins {
inline constexpr gpio_num_t desk_tx = GPIO_NUM_19;
inline constexpr gpio_num_t controller_rx = GPIO_NUM_18;
inline constexpr gpio_num_t handset_rx = GPIO_NUM_5;
inline constexpr gpio_num_t translator_enable = GPIO_NUM_15;
inline constexpr gpio_num_t loctek_wake = GPIO_NUM_14;
inline constexpr gpio_num_t hs0 = GPIO_NUM_23;
inline constexpr gpio_num_t hs1 = GPIO_NUM_22;
inline constexpr gpio_num_t hs2 = GPIO_NUM_21;
inline constexpr gpio_num_t hs3 = GPIO_NUM_20;
inline constexpr gpio_num_t status_led = GPIO_NUM_7;
}  // namespace pandadesk::pins
