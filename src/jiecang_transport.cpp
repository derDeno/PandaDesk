#include "pandadesk/jiecang_transport.hpp"

#include <cstdint>
#include <cstring>
#include <atomic>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "pandadesk/jiecang_rj12.hpp"
#include "pandadesk/pins.hpp"

namespace pandadesk::jiecang_transport {
namespace {
constexpr char kTag[] = "JiecangUART";
constexpr uart_port_t kUart = UART_NUM_1;
constexpr uart_port_t kHandsetUart = UART_NUM_0;
constexpr uint32_t kBaudRate = 9600;
constexpr uint32_t kResponseTimeoutMs = 500;
constexpr uint32_t kLockRefreshIntervalMs = 3000;
constexpr uint32_t kLockResponseTimeoutMs = 1000;
// UART work preempts TCP/IP (18), while Wi-Fi and system event tasks keep priority.
constexpr UBaseType_t kUartTaskPriority = 19;
static_assert(kUartTaskPriority > CONFIG_LWIP_TCPIP_TASK_PRIO);
enum class Profile : uint8_t { none, rj12, jarvis_rj45 };
QueueHandle_t gHandsetEvents = nullptr;
QueueHandle_t gHandsetTxQueue = nullptr;
QueueHandle_t gApiTxQueue = nullptr;
QueueSetHandle_t gTxQueueSet = nullptr;
std::atomic<bool> gReady{false};
std::atomic<Profile> gProfile{Profile::none};
std::atomic<bool> gTargetHeightSupported{false};
std::atomic<bool> gHasHeight{false};
std::atomic<uint16_t> gHeightTenthsCm{0};
std::atomic<uint16_t> gPresetHeights[4]{};
std::atomic<bool> gHasPresetReports{false};
std::atomic<int8_t> gChildLock{-1};
std::atomic<bool> gChildLockBusy{false};
QueueHandle_t gChildLockReports = nullptr;
SemaphoreHandle_t gChildLockMutex = nullptr;
std::atomic<MotionState> gMotion{MotionState::unknown};
std::atomic<uint32_t> gSequence{0};
std::atomic<uint32_t> gCommandGeneration{0};

enum class TxKind : uint8_t { frame, break_signal };
struct TxItem {
  TxKind kind = TxKind::frame;
  uint8_t size = 0;
  uint16_t duration_ms = 0;
  uint32_t generation = 0;
  uint8_t preset = 0;
  bool save_preset = false;
  uint16_t minimum = 0;
  uint16_t maximum = 0;
  uint8_t bytes[jiecang_rj12::kMaxFrameSize]{};
};

esp_err_t queue_child_lock_frame(bool toggle) {
  if (gApiTxQueue == nullptr) return ESP_ERR_INVALID_STATE;
  const auto frame = toggle ? jiecang_rj12::make_child_lock_toggle() : jiecang_rj12::make_child_lock_query();
  TxItem item{};
  item.generation = gCommandGeneration.load();
  item.size = static_cast<uint8_t>(frame.size);
  std::memcpy(item.bytes, frame.bytes, frame.size);
  return xQueueSendToBack(gApiTxQueue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t change_child_lock(bool wanted) {
  xQueueReset(gChildLockReports);
  esp_err_t result = queue_child_lock_frame(false);
  if (result != ESP_OK) return result;
  bool reported = false;
  if (xQueueReceive(gChildLockReports, &reported, pdMS_TO_TICKS(kLockResponseTimeoutMs)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  if (reported == wanted) return ESP_OK;

  xQueueReset(gChildLockReports);
  result = queue_child_lock_frame(true);
  if (result != ESP_OK) return result;
  const TickType_t started = xTaskGetTickCount();
  const TickType_t timeout = pdMS_TO_TICKS(kLockResponseTimeoutMs);
  while (true) {
    const TickType_t elapsed = xTaskGetTickCount() - started;
    if (elapsed >= timeout) break;
    const TickType_t remaining = timeout - elapsed;
    if (xQueueReceive(gChildLockReports, &reported, remaining) != pdTRUE) break;
    if (reported == wanted) return ESP_OK;
  }
  return ESP_ERR_TIMEOUT;
}

Profile configured_profile(bool nvs_available, bool *target_height_supported) {
  if (!nvs_available || target_height_supported == nullptr) return Profile::none;

  nvs_handle_t settings;
  if (nvs_open("device", NVS_READONLY, &settings) != ESP_OK) return Profile::none;
  char profile[33]{};
  char model[33]{};
  size_t profile_size = sizeof(profile);
  size_t model_size = sizeof(model);
  const esp_err_t profile_result = nvs_get_str(settings, "desk_profile", profile, &profile_size);
  const esp_err_t model_result = nvs_get_str(settings, "desk_model", model, &model_size);
  nvs_close(settings);
  if (profile_result != ESP_OK || model_result != ESP_OK) return Profile::none;
  if (std::strcmp(profile, "jiecang_rj12") == 0 && jiecang_rj12::normalize_model(model) != nullptr) {
    *target_height_supported = std::strcmp(model, jiecang_rj12::kNoTargetHeightModel) != 0;
    return Profile::rj12;
  }
  if (std::strcmp(profile, "jiecang_jarvis_rj45") == 0 &&
      std::strcmp(model, "FullyCB2C-A") == 0) {
    *target_height_supported = true;
    return Profile::jarvis_rj45;
  }
  return Profile::none;
}

void update_controller_state(const jiecang_rj12::Frame &frame) {
  uint8_t preset = 0;
  uint16_t raw_preset_height = 0;
  if (gProfile.load() == Profile::rj12 && jiecang_rj12::decode_preset_raw(frame, &preset, &raw_preset_height)) {
    gPresetHeights[preset - 1].store(jiecang_rj12::raw_height_to_tenths_cm(raw_preset_height));
    gHasPresetReports.store(true);
    gSequence.fetch_add(1);
  }
  uint16_t raw_height = 0;
  if (jiecang_rj12::decode_height_raw(frame, &raw_height)) {
    const uint16_t height = jiecang_rj12::raw_height_to_tenths_cm(raw_height);
    if (height != 0) {
      gHeightTenthsCm.store(height);
      gHasHeight.store(true);
      gSequence.fetch_add(1);
    }
  }
  bool locked = false;
  if (jiecang_rj12::decode_child_lock(frame, &locked)) {
    const int8_t value = locked ? 1 : 0;
    if (gChildLock.exchange(value) != value) gSequence.fetch_add(1);
    if (gChildLockBusy.load()) xQueueOverwrite(gChildLockReports, &locked);
  }
}

bool receive_controller_frame(jiecang_rj12::Frame *frame, uint32_t timeout_ms) {
  if (frame == nullptr) return false;

  uint8_t bytes[jiecang_rj12::kMaxFrameSize]{};
  size_t length = 0;
  const TickType_t started = xTaskGetTickCount();
  const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
#ifdef PANDADESK_TRACE_JIECANG
  uint8_t raw_trace[64]{};
  size_t raw_count = 0;
  const auto flush_trace = [&] {
    if (raw_count != 0) ESP_LOG_BUFFER_HEX_LEVEL("JiecangRXRaw", raw_trace, raw_count, ESP_LOG_INFO);
    raw_count = 0;
  };
#endif
  while (xTaskGetTickCount() - started < timeout) {
    uint8_t byte = 0;
    const TickType_t elapsed = xTaskGetTickCount() - started;
    const TickType_t remaining = timeout > elapsed ? timeout - elapsed : 0;
    if (uart_read_bytes(kUart, &byte, 1, remaining) != 1) {
#ifdef PANDADESK_TRACE_JIECANG
      flush_trace();
#endif
      return false;
    }
#ifdef PANDADESK_TRACE_JIECANG
    raw_trace[raw_count++] = byte;
    if (raw_count == sizeof(raw_trace)) flush_trace();
#endif

    if (length == 0) {
      if (byte == jiecang_rj12::kControllerAddress) bytes[length++] = byte;
      continue;
    }
    if (length == 1 && byte != jiecang_rj12::kControllerAddress) {
      length = 0;
      continue;
    }
    bytes[length++] = byte;
    if (length == 4 && bytes[3] > jiecang_rj12::kMaxParameters) {
#ifdef PANDADESK_TRACE_JIECANG
      ESP_LOGW(kTag, "Controller frame has unsupported parameter count: %u", bytes[3]);
#endif
      length = 0;
      continue;
    }
    if (length >= 4 && length == static_cast<size_t>(bytes[3]) + 6) {
      if (jiecang_rj12::decode_frame(bytes, length, frame) &&
          frame->bytes[0] == jiecang_rj12::kControllerAddress) {
#ifdef PANDADESK_TRACE_JIECANG
        flush_trace();
        ESP_LOG_BUFFER_HEX_LEVEL("JiecangRX", bytes, length, ESP_LOG_INFO);
#endif
        update_controller_state(*frame);
        return true;
      }
      length = 0;
    }
    if (length >= jiecang_rj12::kMaxFrameSize) length = 0;
  }
#ifdef PANDADESK_TRACE_JIECANG
  flush_trace();
#endif
  return false;
}

esp_err_t send_handset_bytes(const uint8_t *bytes, size_t size) {
  if (bytes == nullptr || size == 0 || uart_write_bytes(kUart, bytes, size) != static_cast<int>(size)) {
    return ESP_FAIL;
  }
  const esp_err_t result = uart_wait_tx_done(kUart, pdMS_TO_TICKS(1000));
#ifdef PANDADESK_TRACE_JIECANG
  ESP_LOG_BUFFER_HEX_LEVEL("JiecangTX", bytes, size, ESP_LOG_INFO);
#endif
  return result;
}

esp_err_t send_handset_frame(const jiecang_rj12::Frame &frame) {
  jiecang_rj12::Frame checked{};
  if (!jiecang_rj12::decode_frame(frame.bytes, frame.size, &checked) ||
      checked.bytes[0] != jiecang_rj12::kHandsetAddress) {
    return ESP_ERR_INVALID_ARG;
  }
  return send_handset_bytes(frame.bytes, frame.size);
}

esp_err_t send_break_230ms() {
  // UART_BREAK is trailing-only in ESP-IDF; switch the TX pad to GPIO for the reference's leading BREAK.
  esp_err_t result = uart_wait_tx_done(kUart, pdMS_TO_TICKS(1000));
  if (result != ESP_OK) return result;
  result = gpio_reset_pin(pandadesk::pins::desk_tx);
  if (result != ESP_OK) return result;
  result = gpio_set_level(pandadesk::pins::desk_tx, 0);
  if (result != ESP_OK) return result;
  result = gpio_set_direction(pandadesk::pins::desk_tx, GPIO_MODE_OUTPUT);
  if (result != ESP_OK) return result;
  vTaskDelay(pdMS_TO_TICKS(230));
  result = gpio_set_level(pandadesk::pins::desk_tx, 1);
  if (result != ESP_OK) return result;
  return uart_set_pin(kUart, pandadesk::pins::desk_tx, pandadesk::pins::controller_rx,
                      UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

esp_err_t initialize_uart() {
  uart_config_t config{};
  config.baud_rate = kBaudRate;
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_1;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.rx_flow_ctrl_thresh = 0;
  config.source_clk = UART_SCLK_DEFAULT;

  // UART0 is RX-only here; diagnostics use native USB Serial/JTAG and TX stays unrouted.
  esp_err_t result = uart_param_config(kHandsetUart, &config);
  if (result != ESP_OK) return result;
  result = uart_driver_install(kHandsetUart, 512, 0, 16, &gHandsetEvents, 0);
  if (result != ESP_OK) return result;
  result = uart_set_pin(kHandsetUart, UART_PIN_NO_CHANGE, pandadesk::pins::handset_rx,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (result != ESP_OK) {
    uart_driver_delete(kHandsetUart);
    return result;
  }
  result = uart_set_rx_timeout(kHandsetUart, 2);
  if (result != ESP_OK) {
    uart_driver_delete(kHandsetUart);
    return result;
  }

  result = uart_param_config(kUart, &config);
  if (result != ESP_OK) {
    uart_driver_delete(kHandsetUart);
    return result;
  }
  result = uart_driver_install(kUart, 512, 0, 0, nullptr, 0);
  if (result != ESP_OK) {
    uart_driver_delete(kHandsetUart);
    return result;
  }
  result = uart_set_pin(kUart, pandadesk::pins::desk_tx, pandadesk::pins::controller_rx,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (result != ESP_OK) {
    uart_driver_delete(kUart);
    uart_driver_delete(kHandsetUart);
    return result;
  }
  uart_flush_input(kHandsetUart);
  uart_flush_input(kUart);
  return gpio_set_level(pandadesk::pins::translator_enable, 1);
}

void stop_uart() {
  uart_driver_delete(kUart);
  uart_driver_delete(kHandsetUart);
  gHandsetEvents = nullptr;
}

bool queue_handset_item(const TxItem &item) {
  if (gProfile.load() == Profile::rj12 && item.kind == TxKind::frame && item.size == 6) {
    for (uint8_t slot = 0; slot < 4; ++slot) {
      if (item.bytes[2] == jiecang_rj12::kSavePresetCommands[slot]) gPresetHeights[slot].store(0);
    }
  }
  if (xQueueSendToBack(gHandsetTxQueue, &item, 0) == pdTRUE) return true;
  ESP_LOGW(kTag, "Handset TX queue full; dropping one handset message");
  return false;
}

void handset_receive_task(void *) {
  struct Parser {
    uint8_t bytes[jiecang_rj12::kMaxFrameSize]{};
    size_t length = 0;
    size_t expected = 0;

    bool feed(uint8_t byte, TxItem *item) {
      if (length == 0) {
        if (byte == 0) {
          item->size = 1;
          item->bytes[0] = byte;
          return true;
        }
        if (byte == jiecang_rj12::kHandsetAddress) bytes[length++] = byte;
        return false;
      }
      if (length == 1 && byte != jiecang_rj12::kHandsetAddress) {
        length = 0;
        return false;
      }
      bytes[length++] = byte;
      if (length == 4) {
        if (bytes[3] > jiecang_rj12::kMaxParameters) {
          length = 0;
          return false;
        }
        expected = static_cast<size_t>(bytes[3]) + 6;
      }
      if (expected == 0 || length < expected) return false;

      jiecang_rj12::Frame checked{};
      const bool valid = jiecang_rj12::decode_frame(bytes, length, &checked) &&
                         checked.bytes[0] == jiecang_rj12::kHandsetAddress;
      if (valid) {
        item->size = static_cast<uint8_t>(length);
        std::memcpy(item->bytes, bytes, length);
      }
      length = 0;
      expected = 0;
      return valid;
    }
    void reset() { length = expected = 0; }
  } parser;

  uart_event_t event{};
  uint8_t bytes[128]{};
  while (true) {
    if (xQueueReceive(gHandsetEvents, &event, portMAX_DELAY) != pdTRUE) continue;
    if (event.type == UART_DATA) {
      size_t remaining = event.size;
      while (remaining != 0) {
        const size_t requested = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
        const int count = uart_read_bytes(kHandsetUart, bytes, requested, 0);
        if (count <= 0) {
          ESP_LOGW(kTag, "Could not drain handset UART buffer");
          parser.reset();
          uart_flush_input(kHandsetUart);
          xQueueReset(gHandsetEvents);
          break;
        }
        for (int i = 0; i < count; ++i) {
          TxItem item{};
          if (parser.feed(bytes[i], &item)) queue_handset_item(item);
        }
        remaining -= static_cast<size_t>(count);
      }
    } else if (event.type == UART_BREAK) {
      parser.reset();
      TxItem item{};
      item.kind = TxKind::break_signal;
      queue_handset_item(item);
    } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
               event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR) {
      ESP_LOGW(kTag, "Handset UART error (%d); dropping buffered input", event.type);
      parser.reset();
      uart_flush_input(kHandsetUart);
      xQueueReset(gHandsetEvents);
    }
  }
}

esp_err_t transmit(const TxItem &item) {
  if (item.kind == TxKind::break_signal) return send_break_230ms();
  return send_handset_bytes(item.bytes, item.size);
}

esp_err_t send_stop_frame() {
  return send_handset_frame(jiecang_rj12::make_stop_command());
}

void tx_task(void *) {
  TxItem item{};
  bool timed_move_active = false;
  uint8_t move_command = 0;
  TickType_t move_deadline = 0;
  while (true) {
    TickType_t wait = portMAX_DELAY;
    if (timed_move_active) {
      const int32_t remaining = static_cast<int32_t>(move_deadline - xTaskGetTickCount());
      wait = remaining > 0 ? static_cast<TickType_t>(remaining) : 0;
    }
    const QueueSetMemberHandle_t selected = xQueueSelectFromSet(gTxQueueSet, wait);
    if (selected == nullptr) {
      if (timed_move_active) {
        const esp_err_t result = send_stop_frame();
        timed_move_active = false;
        gMotion.store(result == ESP_OK ? MotionState::stopped : MotionState::unknown);
        gSequence.fetch_add(1);
        if (result != ESP_OK) gReady.store(false);
      }
      continue;
    }

    const bool handset_item = selected == gHandsetTxQueue;
    QueueHandle_t source = handset_item ? gHandsetTxQueue : gApiTxQueue;
    if (xQueueReceive(source, &item, 0) != pdTRUE) continue;
    if (!handset_item && item.generation != gCommandGeneration.load()) continue;

    if (handset_item) {
      const bool handset_stop = item.kind == TxKind::frame && item.size >= 6 &&
                                item.bytes[2] == jiecang_rj12::kStopCommand;
      if (timed_move_active && !handset_stop) {
        if (send_stop_frame() != ESP_OK) gReady.store(false);
      }
      timed_move_active = false;
      const esp_err_t result = transmit(item);
      if (result != ESP_OK) {
        ESP_LOGE(kTag, "Handset passthrough TX failed: %s", esp_err_to_name(result));
        gReady.store(false);
        gMotion.store(MotionState::unknown);
      } else if (item.kind == TxKind::frame && item.size >= 6 &&
                 item.bytes[2] == jiecang_rj12::kStopCommand) {
        gMotion.store(MotionState::stopped);
      } else if (item.kind == TxKind::frame && item.size >= 6 &&
                 (item.bytes[2] == jiecang_rj12::kRaiseCommand ||
                  item.bytes[2] == jiecang_rj12::kLowerCommand)) {
        gMotion.store(MotionState::unknown);
      }
      if (result == ESP_OK && gProfile.load() == Profile::rj12 && item.kind == TxKind::frame && item.size == 6) {
        for (const auto command : jiecang_rj12::kSavePresetCommands) {
          if (item.bytes[2] == command && send_handset_frame(jiecang_rj12::make_read_settings_command()) != ESP_OK) gReady.store(false);
        }
      }
      gSequence.fetch_add(1);
      continue;
    }

    if (item.size == 0) continue;
    const uint8_t command = item.bytes[2];
    const bool timed_move = command == jiecang_rj12::kRaiseCommand ||
                            command == jiecang_rj12::kLowerCommand;
    const bool position_move = command == jiecang_rj12::kGotoHeightCommand || (item.preset != 0 && !item.save_preset);
    if (item.preset != 0 && !item.save_preset) {
      uint16_t height = 0;
      // A handset may have overwritten the slot while this command was queued.
      if (!preset_height_tenths_cm(item.preset, &height) || height < item.minimum || height > item.maximum) continue;
    }
    if (timed_move_active && ((timed_move && command != move_command) || position_move)) {
      if (send_stop_frame() != ESP_OK) gReady.store(false);
      timed_move_active = false;
    }
    if (gChildLock.load() == 1 && (timed_move || position_move || item.save_preset)) continue;
    if (item.save_preset) gPresetHeights[item.preset - 1].store(0);
    const esp_err_t result = transmit(item);
    if (result != ESP_OK) {
      ESP_LOGE(kTag, "API command TX failed: %s", esp_err_to_name(result));
      if (timed_move) send_stop_frame();
      gReady.store(false);
      gMotion.store(MotionState::unknown);
      continue;
    }
    if (command == jiecang_rj12::kChildLockCommand) continue;
    if (item.save_preset) {
      // Display only the controller's subsequent report of the stored position.
      if (send_handset_frame(jiecang_rj12::make_read_settings_command()) != ESP_OK) gReady.store(false);
    }
    gSequence.fetch_add(1);
    if (timed_move) {
      timed_move_active = true;
      move_command = command;
      move_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(item.duration_ms);
      gMotion.store(MotionState::moving);
    } else if (command == jiecang_rj12::kStopCommand) {
      timed_move_active = false;
      gMotion.store(MotionState::stopped);
    } else {
      gMotion.store(MotionState::unknown);
    }
  }
}

bool start_command_tasks() {
  gChildLockReports = xQueueCreate(1, sizeof(bool));
  gChildLockMutex = xSemaphoreCreateMutex();
  gHandsetTxQueue = xQueueCreate(16, sizeof(TxItem));
  gApiTxQueue = xQueueCreate(8, sizeof(TxItem));
  gTxQueueSet = xQueueCreateSet(24);
  if (gChildLockReports == nullptr || gChildLockMutex == nullptr ||
      gHandsetTxQueue == nullptr || gApiTxQueue == nullptr || gTxQueueSet == nullptr ||
      xQueueAddToSet(gHandsetTxQueue, gTxQueueSet) != pdPASS ||
      xQueueAddToSet(gApiTxQueue, gTxQueueSet) != pdPASS) {
    return false;
  }
  if (xTaskCreate(tx_task, "jiecang_tx", 4096, nullptr, kUartTaskPriority, nullptr) != pdPASS ||
      xTaskCreate(handset_receive_task, "jiecang_handset", 4096, nullptr, kUartTaskPriority, nullptr) != pdPASS) {
    return false;
  }
  return true;
}

esp_err_t negotiate_startup() {
  // The Maidesite RJ12 controller answers this read while ignoring WAKE at idle.
  if (gProfile.load() == Profile::rj12 &&
      send_handset_frame(jiecang_rj12::make_child_lock_query()) != ESP_OK) return ESP_FAIL;
  constexpr uint8_t null_byte = 0x00;
  if (send_handset_bytes(&null_byte, 1) != ESP_OK) return ESP_FAIL;

  jiecang_rj12::Frame response{};
  if (receive_controller_frame(&response, kResponseTimeoutMs)) return ESP_OK;

  if (send_break_230ms() != ESP_OK || send_handset_bytes(&null_byte, 1) != ESP_OK) return ESP_FAIL;
  if (receive_controller_frame(&response, kResponseTimeoutMs)) return ESP_OK;

  ESP_LOGI(kTag, "No response to NULL/BREAK startup; polling with the documented WAKE frame");
  const auto wake = jiecang_rj12::make_handset_command(jiecang_rj12::kWakeCommand, nullptr, 0);
  while (true) {
    if (send_handset_frame(wake) != ESP_OK) return ESP_FAIL;
    if (receive_controller_frame(&response, kResponseTimeoutMs)) return ESP_OK;
  }
}

void transport_task(void *) {
  const esp_err_t result = initialize_uart();
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Could not initialize Jiecang UART: %s", esp_err_to_name(result));
    stop_uart();
    gpio_set_level(pandadesk::pins::translator_enable, 0);
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(kTag, "Starting Jiecang UART negotiation at %lu 8N1", static_cast<unsigned long>(kBaudRate));
  const esp_err_t startup_result = negotiate_startup();
  if (startup_result == ESP_OK) {
    ESP_LOGI(kTag, "Controller startup response received");
    if (!start_command_tasks()) {
      ESP_LOGE(kTag, "Could not start Jiecang command and handset tasks");
      vTaskDelete(nullptr);
      return;
    }
    gMotion.store(MotionState::unknown);
    gReady.store(true);
    ESP_LOGI(kTag, "Movement commands and handset pass-through active");
    TxItem settings_request{};
    const auto request_frame = jiecang_rj12::make_read_settings_command();
    settings_request.size = static_cast<uint8_t>(request_frame.size);
    std::memcpy(settings_request.bytes, request_frame.bytes, request_frame.size);
    xQueueSendToBack(gApiTxQueue, &settings_request, 0);
    TickType_t last_lock_query = xTaskGetTickCount();
    while (true) {
      jiecang_rj12::Frame frame{};
      if (receive_controller_frame(&frame, 1000)) {
        uint16_t raw_height = 0;
        const TickType_t now = xTaskGetTickCount();
        // Refresh only on height activity; idle queries repeatedly wake the handset.
        if (gProfile.load() == Profile::rj12 && !gChildLockBusy.load() &&
            jiecang_rj12::decode_height_raw(frame, &raw_height) &&
            jiecang_rj12::raw_height_to_tenths_cm(raw_height) != 0 &&
            now - last_lock_query >= pdMS_TO_TICKS(kLockRefreshIntervalMs)) {
          if (queue_child_lock_frame(false) == ESP_OK) last_lock_query = now;
        }
      }
    }
  } else {
    ESP_LOGE(kTag, "Jiecang startup negotiation failed: %s", esp_err_to_name(startup_result));
    stop_uart();
    gpio_set_level(pandadesk::pins::translator_enable, 0);
  }
  vTaskDelete(nullptr);
}
}  // namespace

void start_if_configured(bool nvs_available) {
  gReady.store(false);
  gProfile.store(Profile::none);
  gTargetHeightSupported.store(false);
  gHasHeight.store(false);
  for (auto &height : gPresetHeights) height.store(0);
  gHasPresetReports.store(false);
  gChildLock.store(-1);
  gMotion.store(MotionState::unknown);
  bool supports_target = false;
  const Profile profile = configured_profile(nvs_available, &supports_target);
  if (profile == Profile::none) return;
  gProfile.store(profile);
  gTargetHeightSupported.store(supports_target);
  if (xTaskCreate(transport_task, "jiecang_uart", 4096, nullptr, 6, nullptr) != pdPASS) {
    gProfile.store(Profile::none);
    gTargetHeightSupported.store(false);
    ESP_LOGE(kTag, "Could not create Jiecang UART task");
  }
}

bool is_ready() { return gReady.load(); }

bool supports_target_height() { return gReady.load() && gTargetHeightSupported.load(); }

bool supports_presets() {
  return gReady.load() && gProfile.load() == Profile::rj12 && gHasPresetReports.load();
}

bool preset_height_tenths_cm(uint8_t preset, uint16_t *height) {
  if (!supports_presets() || height == nullptr || preset < 1 || preset > 4) return false;
  *height = gPresetHeights[preset - 1].load();
  return *height != 0;
}

const char *profile_name() {
  if (!gReady.load()) return nullptr;
  return gProfile.load() == Profile::rj12 ? "jiecang_rj12" : "jiecang_jarvis_rj45";
}

bool height_tenths_cm(uint16_t *height) {
  if (height == nullptr || !gHasHeight.load()) return false;
  *height = gHeightTenthsCm.load();
  return true;
}

MotionState motion_state() { return gMotion.load(); }

bool child_locked(bool *locked) {
  const int8_t value = gChildLock.load();
  if (locked == nullptr || !gReady.load() || value < 0) return false;
  *locked = value == 1;
  return true;
}

bool supports_child_lock() {
  return gReady.load() && gProfile.load() == Profile::rj12 && gChildLock.load() >= 0;
}

esp_err_t set_child_lock(bool locked) {
  if (!supports_child_lock() || gChildLockMutex == nullptr) return ESP_ERR_NOT_SUPPORTED;
  if (xSemaphoreTake(gChildLockMutex, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
  gChildLockBusy.store(true);
  // Wait in the caller; the UART owner remains free to process movement/STOP.
  const esp_err_t result = change_child_lock(locked);
  gChildLockBusy.store(false);
  xSemaphoreGive(gChildLockMutex);
  return result;
}

uint32_t state_sequence() { return gSequence.load(); }

esp_err_t queue_move(Direction direction, uint16_t duration_ms) {
  if (!gReady.load() || gApiTxQueue == nullptr) return ESP_ERR_INVALID_STATE;
  TxItem item{};
  if (direction == Direction::stop) {
    gCommandGeneration.fetch_add(1);
    item.generation = gCommandGeneration.load();
    const auto frame = jiecang_rj12::make_stop_command();
    item.size = static_cast<uint8_t>(frame.size);
    std::memcpy(item.bytes, frame.bytes, frame.size);
    return xQueueSendToFront(gApiTxQueue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
  }
  if (duration_ms < 1 || duration_ms > 5000) return ESP_ERR_INVALID_ARG;
  if (gChildLock.load() == 1) return ESP_ERR_NOT_ALLOWED;
  const auto frame = direction == Direction::up ? jiecang_rj12::make_raise_command()
                                                 : jiecang_rj12::make_lower_command();
  item.generation = gCommandGeneration.load();
  item.duration_ms = duration_ms;
  item.size = static_cast<uint8_t>(frame.size);
  std::memcpy(item.bytes, frame.bytes, frame.size);
  return xQueueSendToBack(gApiTxQueue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t queue_target_height(uint16_t height_tenths_cm) {
  if (!supports_target_height() || gApiTxQueue == nullptr) {
    return gReady.load() ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_STATE;
  }
  if (gChildLock.load() == 1) return ESP_ERR_NOT_ALLOWED;
  const auto frame = jiecang_rj12::make_target_height_command_mm(height_tenths_cm);
  TxItem item{};
  item.generation = gCommandGeneration.load();
  item.size = static_cast<uint8_t>(frame.size);
  std::memcpy(item.bytes, frame.bytes, frame.size);
  return xQueueSendToBack(gApiTxQueue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t queue_preset(uint8_t preset, uint16_t minimum, uint16_t maximum) {
  if (preset < 1 || preset > 4 || minimum >= maximum) return ESP_ERR_INVALID_ARG;
  if (!supports_presets() || gApiTxQueue == nullptr) return ESP_ERR_NOT_SUPPORTED;
  if (gChildLock.load() == 1) return ESP_ERR_NOT_ALLOWED;
  uint16_t height = 0;
  if (!preset_height_tenths_cm(preset, &height)) return ESP_ERR_NOT_FOUND;
  if (height < minimum || height > maximum) return ESP_ERR_INVALID_ARG;
  const auto frame = jiecang_rj12::make_preset_command(preset);
  TxItem item{};
  item.generation = gCommandGeneration.load();
  item.preset = preset;
  item.minimum = minimum;
  item.maximum = maximum;
  item.size = static_cast<uint8_t>(frame.size);
  std::memcpy(item.bytes, frame.bytes, frame.size);
  return xQueueSendToBack(gApiTxQueue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t queue_save_preset(uint8_t preset) {
  if (preset < 1 || preset > 4) return ESP_ERR_INVALID_ARG;
  if (!supports_presets() || gApiTxQueue == nullptr) return ESP_ERR_NOT_SUPPORTED;
  if (gChildLock.load() == 1) return ESP_ERR_NOT_ALLOWED;
  const auto frame = jiecang_rj12::make_save_preset_command(preset);
  TxItem item{};
  item.generation = gCommandGeneration.load();
  item.preset = preset;
  item.save_preset = true;
  item.size = static_cast<uint8_t>(frame.size);
  std::memcpy(item.bytes, frame.bytes, frame.size);
  return xQueueSendToBack(gApiTxQueue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

}  // namespace pandadesk::jiecang_transport
