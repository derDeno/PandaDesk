#include <cstdint>
#include <atomic>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "pandadesk/api.hpp"
#include "pandadesk/jiecang_transport.hpp"
#include "pandadesk/loctek_transport.hpp"
#include "pandadesk/logs.hpp"
#include "pandadesk/mqtt.hpp"
#include "pandadesk/pins.hpp"
#include "pandadesk/status_led.hpp"
#include "pandadesk/wifi.hpp"

namespace {
constexpr char kTag[] = "PandaDesk";
std::atomic<uint8_t> g_led_brightness{pandadesk::status_led::kDefaultBrightness};

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
  if (!nvs_available) return pandadesk::status_led::kDefaultBrightness;

  nvs_handle_t settings;
  if (nvs_open("device", NVS_READONLY, &settings) != ESP_OK) return pandadesk::status_led::kDefaultBrightness;

  uint8_t brightness = pandadesk::status_led::kDefaultBrightness;
  if (nvs_get_u8(settings, "led_brightness", &brightness) != ESP_OK || brightness > 100) {
    brightness = pandadesk::status_led::kDefaultBrightness;
  }
  nvs_close(settings);
  return brightness;
}

void apply_led_brightness(uint8_t brightness) {
  g_led_brightness.store(brightness);
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

void led_task(void *) {
  constexpr uint32_t max_duty = (1U << LEDC_TIMER_13_BIT) - 1U;
  while (true) {
    uint16_t height = 0;
    const bool desk_connected = pandadesk::jiecang_transport::height_tenths_cm(&height) ||
                                pandadesk::loctek_transport::height_tenths_cm(&height);
    const auto level = pandadesk::status_led::level(
        static_cast<uint32_t>(esp_timer_get_time() / 1000), g_led_brightness.load(),
        pandadesk::wifi::captive_portal_url() != nullptr,
        pandadesk::wifi::connected(), desk_connected);
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, max_duty * level / 100));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

}  // namespace

void pandadesk::status_led::set_brightness(uint8_t brightness) {
  g_led_brightness.store(brightness);
}

extern "C" void app_main() {
  pandadesk::logs::start();
  initialize_safe_gpio();
  const esp_err_t nvs_result = nvs_flash_init();
  const bool nvs_available = nvs_result == ESP_OK;
  if (!nvs_available) ESP_LOGW(kTag, "Settings unavailable: %s", esp_err_to_name(nvs_result));
  apply_led_brightness(load_led_brightness(nvs_available));
  pandadesk::api::mount_webui();
  ESP_LOGI(kTag, "Firmware %s booted", esp_app_get_description()->version);
  ESP_LOGI(kTag, "Desk transports start only for a supported saved profile/model");
  pandadesk::wifi::start(nvs_available);
  pandadesk::jiecang_transport::start_if_configured(nvs_available);
  pandadesk::loctek_transport::start_if_configured(nvs_available);
  pandadesk::mqtt::start();
  if (xTaskCreate(led_task, "pandadesk_led", 2048, nullptr, 2, nullptr) != pdPASS) {
    ESP_LOGE(kTag, "Could not create status LED task");
  }
}
