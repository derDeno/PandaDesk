#include "pandadesk/loctek_transport.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "pandadesk/pins.hpp"

namespace pandadesk::loctek_transport {
namespace {
constexpr char kTag[] = "LoctekUART";
constexpr uart_port_t kControllerUart = UART_NUM_1;
constexpr uart_port_t kHandsetUart = UART_NUM_0;
constexpr uint32_t kBaudRate = 9600;
constexpr size_t kMaxFrameSize = 32;
constexpr uint32_t kWakeSettleMs = 1100;
constexpr uint32_t kWakeHoldMs = 10000;
constexpr uint32_t kHandsetReplyWindowMs = 4;
constexpr uint8_t kStart = 0x9B;
constexpr uint8_t kEnd = 0x9D;
constexpr uint8_t kPoll = 0x11;
constexpr uint8_t kHeight = 0x12;
constexpr uint8_t kButtonState = 0x02;

constexpr uint8_t kIdleFrame[] = {0x9B, 0x06, 0x02, 0x00, 0x00, 0x6C, 0xA1, 0x9D};
constexpr uint8_t kUpFrame[] = {0x9B, 0x06, 0x02, 0x01, 0x00, 0xFC, 0xA0, 0x9D};
constexpr uint8_t kDownFrame[] = {0x9B, 0x06, 0x02, 0x02, 0x00, 0x0C, 0xA0, 0x9D};
QueueHandle_t gHandsetFrames = nullptr;
std::atomic<bool> gReady{false};
std::atomic<bool> gHasHeight{false};
std::atomic<uint16_t> gHeightTenthsCm{0};
std::atomic<jiecang_transport::MotionState> gMotion{jiecang_transport::MotionState::unknown};
std::atomic<uint32_t> gSequence{0};
std::atomic<int> gRequestedDirection{0};
std::atomic<uint16_t> gRequestedDurationMs{0};
std::atomic<uint32_t> gQueuedExpiryMs{0};
std::atomic<uint32_t> gMoveDeadlineMs{0};
std::atomic<uint32_t> gWakeReadyAtMs{0};
std::atomic<uint32_t> gWakeReleaseAtMs{0};
std::atomic<bool> gWakeActive{false};
std::atomic<bool> gConfigured{false};

struct Frame {
  uint8_t size = 0;
  uint32_t received_at_ms = 0;
  uint8_t bytes[kMaxFrameSize]{};
};

uint32_t now_ms() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

bool deadline_reached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

bool supported_model(const char *model) {
  return std::strcmp(model, "FLEXISPOT_E7_PRO_PLUS") == 0;
}

bool configured_profile(bool nvs_available) {
  if (!nvs_available) return false;
  nvs_handle_t settings;
  if (nvs_open("device", NVS_READONLY, &settings) != ESP_OK) return false;
  char profile[33]{};
  char model[33]{};
  size_t profile_size = sizeof(profile);
  size_t model_size = sizeof(model);
  const esp_err_t profile_result = nvs_get_str(settings, "desk_profile", profile, &profile_size);
  const esp_err_t model_result = nvs_get_str(settings, "desk_model", model, &model_size);
  nvs_close(settings);
  return profile_result == ESP_OK && model_result == ESP_OK &&
         std::strcmp(profile, "loctek_flexispot_rj45") == 0 && supported_model(model);
}

bool initialize_uart() {
  uart_config_t config{};
  config.baud_rate = kBaudRate;
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_1;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.rx_flow_ctrl_thresh = 0;
  config.source_clk = UART_SCLK_DEFAULT;

  esp_err_t result = uart_param_config(kHandsetUart, &config);
  if (result != ESP_OK) return false;
  result = uart_driver_install(kHandsetUart, 512, 0, 0, nullptr, 0);
  if (result != ESP_OK) return false;
  result = uart_set_pin(kHandsetUart, UART_PIN_NO_CHANGE, pandadesk::pins::handset_rx,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (result != ESP_OK) return false;
  result = uart_set_rx_timeout(kHandsetUart, 2);
  if (result != ESP_OK) return false;

  result = uart_param_config(kControllerUart, &config);
  if (result != ESP_OK) return false;
  result = uart_driver_install(kControllerUart, 512, 0, 0, nullptr, 0);
  if (result != ESP_OK) return false;
  result = uart_set_pin(kControllerUart, pandadesk::pins::desk_tx, pandadesk::pins::controller_rx,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (result != ESP_OK) return false;
  uart_flush_input(kHandsetUart);
  uart_flush_input(kControllerUart);
  return gpio_set_level(pandadesk::pins::translator_enable, 1) == ESP_OK;
}

void start_wake() {
  const uint32_t now = now_ms();
  if (!gWakeActive.exchange(true)) {
    gpio_set_level(pandadesk::pins::loctek_wake, 1);
    gWakeReadyAtMs.store(now + kWakeSettleMs);
  }
  gWakeReleaseAtMs.store(now + kWakeHoldMs);
}

void release_wake_if_idle(uint32_t now) {
  if (gWakeActive.load() && gRequestedDirection.load() == 0 &&
      deadline_reached(now, gWakeReleaseAtMs.load())) {
    gpio_set_level(pandadesk::pins::loctek_wake, 0);
    gWakeActive.store(false);
  }
}

class Parser {
 public:
  bool feed(uint8_t byte, Frame *frame) {
    if (frame == nullptr) return false;
    if (size_ == 0) {
      if (byte == kStart) bytes_[size_++] = byte;
      return false;
    }
    if (size_ == 1 && (byte < 2 || byte > kMaxFrameSize - 2)) {
      size_ = byte == kStart ? 1 : 0;
      if (size_ != 0) bytes_[0] = kStart;
      return false;
    }
    bytes_[size_++] = byte;
    if (size_ < 3) return false;
    const size_t expected = static_cast<size_t>(bytes_[1]) + 2;
    if (size_ < expected) return false;
    // shortcut: validate framing only; add checksum verification if hardware captures expose varying inbound frames.
    const bool valid = size_ == expected && bytes_[expected - 1] == kEnd;
    if (valid) {
      frame->size = static_cast<uint8_t>(size_);
      frame->received_at_ms = now_ms();
      std::memcpy(frame->bytes, bytes_, size_);
    }
    size_ = 0;
    return valid;
  }

 private:
  uint8_t bytes_[kMaxFrameSize]{};
  size_t size_ = 0;
};

bool send_frame(const uint8_t *bytes, size_t size) {
  return uart_write_bytes(kControllerUart, bytes, size) == static_cast<int>(size) &&
         uart_wait_tx_done(kControllerUart, pdMS_TO_TICKS(100)) == ESP_OK;
}

bool send_frame(const Frame &frame) { return send_frame(frame.bytes, frame.size); }

int decode_digit(uint8_t segment) {
  constexpr uint8_t patterns[] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  segment &= 0x7F;
  if (segment == 0) return -2;
  if (segment == 0x40) return -3;
  for (int digit = 0; digit < 10; ++digit) {
    if (segment == patterns[digit]) return digit;
  }
  return -1;
}

void read_height(const Frame &frame) {
  if (frame.bytes[2] != kHeight || (frame.bytes[1] != 7 && frame.bytes[1] != 10)) return;
  const int hundreds = decode_digit(frame.bytes[3]);
  const int tens = decode_digit(frame.bytes[4]);
  const int units = decode_digit(frame.bytes[5]);
  if ((hundreds < 0 && hundreds != -2) || tens < 0 || units < 0) return;
  const int leading = hundreds == -2 ? 0 : hundreds;
  const uint16_t displayed_tenths = static_cast<uint16_t>((leading * 100 + tens * 10 + units) *
                                                          ((frame.bytes[4] & 0x80) ? 1 : 10));
  if (displayed_tenths == 0) return;

  nvs_handle_t settings;
  uint16_t minimum = 600, maximum = 1250;
  if (nvs_open("device", NVS_READONLY, &settings) == ESP_OK) {
    nvs_get_u16(settings, "min_h_tenths", &minimum);
    nvs_get_u16(settings, "max_h_tenths", &maximum);
    nvs_close(settings);
  }
  uint16_t height = displayed_tenths;
  if (height < minimum && static_cast<uint32_t>(height) * 254 / 100 >= minimum &&
      static_cast<uint32_t>(height) * 254 / 100 <= maximum) {
    height = static_cast<uint16_t>(static_cast<uint32_t>(height) * 254 / 100);
  }
  if (height < minimum || height > maximum) return;
  if (!gHasHeight.exchange(true) || gHeightTenthsCm.exchange(height) != height) gSequence.fetch_add(1);
}

bool is_idle_button(const Frame &frame) {
  return frame.bytes[2] == kButtonState && frame.size >= 8 && frame.bytes[3] == 0 && frame.bytes[4] == 0;
}

bool is_active_button(const Frame &frame) {
  return frame.bytes[2] == kButtonState && frame.size >= 8 && (frame.bytes[3] != 0 || frame.bytes[4] != 0);
}

void handset_receive_task(void *) {
  Parser parser;
  uint8_t byte = 0;
  while (true) {
    if (uart_read_bytes(kHandsetUart, &byte, 1, pdMS_TO_TICKS(50)) != 1) continue;
    Frame frame{};
    if (!parser.feed(byte, &frame) || frame.bytes[2] != kButtonState) continue;
    if (is_active_button(frame)) start_wake();
    if (xQueueSendToBack(gHandsetFrames, &frame, 0) != pdTRUE) {
      Frame dropped{};
      xQueueReceive(gHandsetFrames, &dropped, 0);
      xQueueSendToBack(gHandsetFrames, &frame, 0);
      ESP_LOGW(kTag, "Handset response queue full; replaced the oldest frame");
    }
  }
}

void handle_poll() {
  static Frame deferred{};
  static bool has_deferred = false;
  static bool handset_was_active = false;
  Frame handset{};
  const bool have_handset = xQueueReceive(gHandsetFrames, &handset,
                                           pdMS_TO_TICKS(kHandsetReplyWindowMs)) == pdTRUE;
  const uint32_t now = now_ms();
  if (have_handset && now - handset.received_at_ms > 250) {
    handset.size = 0;
  }
  if (have_handset && handset.size != 0 && !deadline_reached(now, gWakeReadyAtMs.load())) {
    // Preserve the first handset command until the wake interval has elapsed.
    deferred = handset;
    has_deferred = true;
    send_frame(kIdleFrame, sizeof(kIdleFrame));
    return;
  }

  if (have_handset && handset.size != 0) {
    deferred = handset;
    has_deferred = true;
  }
  const bool use_deferred = has_deferred && deadline_reached(now, gWakeReadyAtMs.load()) &&
                            now - deferred.received_at_ms <= 250;
  const bool have_response = use_deferred || (have_handset && handset.size != 0);
  const Frame &response = use_deferred ? deferred : handset;
  const bool manual_active = have_response && is_active_button(response);
  const bool manual_idle = have_response && is_idle_button(response);

  if (manual_active) {
    handset_was_active = true;
    gRequestedDirection.store(0);
    gMoveDeadlineMs.store(0);
    gMotion.store(jiecang_transport::MotionState::moving);
  } else if (manual_idle && handset_was_active) {
    handset_was_active = false;
    gRequestedDirection.store(0);
    gMoveDeadlineMs.store(0);
    gMotion.store(jiecang_transport::MotionState::stopped);
  }

  bool sent = false;
  if (have_response && response.size != 0 && (manual_active || gRequestedDirection.load() == 0)) {
    sent = send_frame(response);
    has_deferred = false;
    gSequence.fetch_add(1);
  } else {
    int direction = gRequestedDirection.load();
    uint32_t deadline = gMoveDeadlineMs.load();
    if (direction != 0 && deadline == 0) {
      if (deadline_reached(now, gQueuedExpiryMs.load())) {
        gRequestedDirection.store(0);
        direction = 0;
      } else if (deadline_reached(now, gWakeReadyAtMs.load())) {
        deadline = now + gRequestedDurationMs.load();
        gMoveDeadlineMs.store(deadline);
      }
    } else if (direction != 0 && deadline_reached(now, deadline)) {
      gRequestedDirection.store(0);
      gMoveDeadlineMs.store(0);
      gMotion.store(jiecang_transport::MotionState::stopped);
      gSequence.fetch_add(1);
      direction = 0;
    }
    if (direction == 1 && deadline != 0 && deadline_reached(now, gWakeReadyAtMs.load())) {
      sent = send_frame(kUpFrame, sizeof(kUpFrame));
      gMotion.store(jiecang_transport::MotionState::moving);
    } else if (direction == 2 && deadline != 0 && deadline_reached(now, gWakeReadyAtMs.load())) {
      sent = send_frame(kDownFrame, sizeof(kDownFrame));
      gMotion.store(jiecang_transport::MotionState::moving);
    }
    if (!sent && have_response && response.size != 0) {
      sent = send_frame(response);
      has_deferred = false;
    } else if (!sent && deadline_reached(now, gWakeReadyAtMs.load())) {
      sent = send_frame(kIdleFrame, sizeof(kIdleFrame));
    } else if (!sent) {
      sent = send_frame(kIdleFrame, sizeof(kIdleFrame));
    }
  }
  if (!sent) {
    gReady.store(false);
    gMotion.store(jiecang_transport::MotionState::unknown);
    ESP_LOGW(kTag, "Could not send the Loctek poll response");
  }
  release_wake_if_idle(now);
}

void controller_receive_task(void *) {
  Parser parser;
  uint8_t byte = 0;
  while (true) {
    const uint32_t now = now_ms();
    release_wake_if_idle(now);
    if (uart_read_bytes(kControllerUart, &byte, 1, pdMS_TO_TICKS(20)) != 1) continue;
    Frame frame{};
    if (!parser.feed(byte, &frame)) continue;
    if (frame.bytes[2] == kHeight) {
      read_height(frame);
    } else if (frame.bytes[2] == kPoll) {
      if (!gReady.exchange(true)) {
        gMotion.store(jiecang_transport::MotionState::unknown);
        gSequence.fetch_add(1);
        ESP_LOGI(kTag, "Loctek controller poll received; RJ45 controls are ready");
      }
      handle_poll();
    }
  }
}

void transport_task(void *) {
  if (!initialize_uart()) {
    ESP_LOGE(kTag, "Could not initialize Loctek UART");
    gpio_set_level(pandadesk::pins::translator_enable, 0);
    gConfigured.store(false);
    vTaskDelete(nullptr);
    return;
  }
  gHandsetFrames = xQueueCreate(16, sizeof(Frame));
  if (gHandsetFrames == nullptr) {
    ESP_LOGE(kTag, "Could not create Loctek handset queue");
    gpio_set_level(pandadesk::pins::translator_enable, 0);
    gConfigured.store(false);
    vTaskDelete(nullptr);
    return;
  }
  gpio_set_level(pandadesk::pins::loctek_wake, 1);
  gWakeActive.store(true);
  const uint32_t now = now_ms();
  gWakeReadyAtMs.store(now + kWakeSettleMs);
  gWakeReleaseAtMs.store(now + kWakeHoldMs);
  if (xTaskCreate(handset_receive_task, "loctek_handset", 3072, nullptr, 7, nullptr) != pdPASS ||
      xTaskCreate(controller_receive_task, "loctek_controller", 4096, nullptr, 7, nullptr) != pdPASS) {
    ESP_LOGE(kTag, "Could not start Loctek UART tasks");
    gpio_set_level(pandadesk::pins::loctek_wake, 0);
    gpio_set_level(pandadesk::pins::translator_enable, 0);
    gConfigured.store(false);
  }
  ESP_LOGI(kTag, "Loctek RJ45 wake and 9600/8N1 transport started");
  vTaskDelete(nullptr);
}
}  // namespace

void start_if_configured(bool nvs_available) {
  gReady.store(false);
  gConfigured.store(false);
  gHasHeight.store(false);
  gMotion.store(jiecang_transport::MotionState::unknown);
  if (!configured_profile(nvs_available)) return;
  gConfigured.store(true);
  if (xTaskCreate(transport_task, "loctek_uart", 4096, nullptr, 6, nullptr) != pdPASS) {
    gConfigured.store(false);
    ESP_LOGE(kTag, "Could not create Loctek UART task");
  }
}

bool is_ready() { return gReady.load(); }

bool is_configured() { return gConfigured.load(); }

bool height_tenths_cm(uint16_t *height) {
  if (height == nullptr || !gHasHeight.load()) return false;
  *height = gHeightTenthsCm.load();
  return true;
}

jiecang_transport::MotionState motion_state() { return gMotion.load(); }

uint32_t state_sequence() { return gSequence.load(); }

esp_err_t queue_move(jiecang_transport::Direction direction, uint16_t duration_ms) {
  if (!gConfigured.load() || !gReady.load()) return ESP_ERR_INVALID_STATE;
  if (direction == jiecang_transport::Direction::stop) {
    gRequestedDirection.store(0);
    gMoveDeadlineMs.store(0);
    gMotion.store(jiecang_transport::MotionState::stopped);
    gSequence.fetch_add(1);
    start_wake();
    return ESP_OK;
  }
  if (duration_ms < 1 || duration_ms > 5000) return ESP_ERR_INVALID_ARG;
  start_wake();
  gRequestedDirection.store(direction == jiecang_transport::Direction::up ? 1 : 2);
  gRequestedDurationMs.store(duration_ms);
  gMoveDeadlineMs.store(0);
  gQueuedExpiryMs.store(now_ms() + kWakeSettleMs + duration_ms + 3000);
  gWakeReleaseAtMs.store(now_ms() + kWakeSettleMs + duration_ms + 1000);
  return ESP_OK;
}

}  // namespace pandadesk::loctek_transport
