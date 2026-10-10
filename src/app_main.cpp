#include <cstdint>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "pandadesk/jiecang_jarvis_transport.hpp"
#include "pandadesk/pins.hpp"
#include "pandadesk/wifi.hpp"

namespace {
constexpr char kTag[] = "PandaDesk";

constexpr uint64_t output_mask =
    (1ULL << pandadesk::pins::desk_tx) |
    (1ULL << pandadesk::pins::translator_enable) |
    (1ULL << pandadesk::pins::loctek_wake) |
    (1ULL << pandadesk::pins::hs0) |
    (1ULL << pandadesk::pins::hs1) |
    (1ULL << pandadesk::pins::hs2) |
    (1ULL << pandadesk::pins::hs3) |
    (1ULL << pandadesk::pins::status_led);

void initialize_safe_gpio() {
  // Load safe output levels before enabling the output drivers.
  ESP_ERROR_CHECK(gpio_set_level(pandadesk::pins::desk_tx, 1));
  ESP_ERROR_CHECK(gpio_set_level(pandadesk::pins::translator_enable, 0));
  ESP_ERROR_CHECK(gpio_set_level(pandadesk::pins::loctek_wake, 0));
  ESP_ERROR_CHECK(gpio_set_level(pandadesk::pins::hs0, 1));
  ESP_ERROR_CHECK(gpio_set_level(pandadesk::pins::hs1, 1));
  ESP_ERROR_CHECK(gpio_set_level(pandadesk::pins::hs2, 1));
  ESP_ERROR_CHECK(gpio_set_level(pandadesk::pins::hs3, 1));
  ESP_ERROR_CHECK(gpio_set_level(pandadesk::pins::status_led, 1));

  const gpio_config_t outputs = {
      .pin_bit_mask = output_mask,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&outputs));

  const gpio_config_t inputs = {
      .pin_bit_mask = (1ULL << pandadesk::pins::controller_rx) |
                      (1ULL << pandadesk::pins::handset_rx),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&inputs));
}

uint8_t load_led_brightness(bool nvs_available) {
  if (!nvs_available) return 100;

  nvs_handle_t settings;
  if (nvs_open("device", NVS_READONLY, &settings) != ESP_OK) return 100;

  uint8_t brightness = 100;
  if (nvs_get_u8(settings, "led_brightness", &brightness) != ESP_OK || brightness > 100) {
    brightness = 100;
  }
  nvs_close(settings);
  return brightness;
}

void apply_led_brightness(uint8_t brightness) {
  constexpr uint32_t max_duty = (1U << LEDC_TIMER_13_BIT) - 1U;
  ledc_timer_config_t timer{};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_13_BIT;
  timer.timer_num = LEDC_TIMER_0;
  timer.freq_hz = 5000;
  timer.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&timer));

  ledc_channel_config_t channel{};
  channel.gpio_num = pandadesk::pins::status_led;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = LEDC_CHANNEL_0;
  channel.intr_type = LEDC_INTR_DISABLE;
  channel.timer_sel = LEDC_TIMER_0;
  channel.duty = max_duty * brightness / 100;
  channel.hpoint = 0;
  ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

void mount_webui() {
  esp_vfs_littlefs_conf_t config{};
  config.base_path = "/webui";
  config.partition_label = "littlefs";
  config.format_if_mount_failed = false;
  const esp_err_t err = esp_vfs_littlefs_register(&config);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "Could not mount LittleFS: %s", esp_err_to_name(err));
  }
}
}  // namespace

extern "C" void app_main() {
  initialize_safe_gpio();
  const esp_err_t nvs_result = nvs_flash_init();
  const bool nvs_available = nvs_result == ESP_OK;
  if (!nvs_available) ESP_LOGW(kTag, "Settings unavailable: %s", esp_err_to_name(nvs_result));
  apply_led_brightness(load_led_brightness(nvs_available));
  mount_webui();
  ESP_LOGI(kTag, "Firmware %s booted; desk movement remains disabled", esp_app_get_description()->version);
  ESP_LOGI(kTag, "Jarvis UART startup runs only for a saved FullyCB2C-A profile");
  pandadesk::wifi::start(nvs_available);
  pandadesk::jiecang_jarvis_transport::start_if_configured(nvs_available);
}
