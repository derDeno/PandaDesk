#include "pandadesk/api.hpp"

#include <cstdio>
#include <cctype>
#include <cstring>
#include <cmath>
#include <cinttypes>
#include <atomic>
#include <charconv>
#include <memory>
#include <new>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pandadesk/jiecang_rj12.hpp"
#include "pandadesk/jiecang_transport.hpp"
#include "pandadesk/loctek_transport.hpp"
#include "pandadesk/logs.hpp"
#include "pandadesk/mqtt.hpp"
#include "pandadesk/pins.hpp"
#include "pandadesk/status_led.hpp"
#include "pandadesk/wifi.hpp"

namespace pandadesk::api {
namespace {
constexpr char kTag[] = "PandaDeskAPI";
constexpr char kPcbRevision[] = "v1.0";
constexpr size_t kMaxBodyLength = 512;
constexpr size_t kMaxRequestIdLength = 64;
constexpr size_t kMaxSsidLength = 32;
constexpr size_t kMaxPasswordLength = 63;
constexpr size_t kMaxMqttHostLength = 128;
constexpr size_t kMaxMqttUsernameLength = 64;
constexpr size_t kMaxMqttPasswordLength = 63;
constexpr size_t kMaxUriLength = CONFIG_HTTPD_MAX_URI_LEN;
constexpr uint16_t kDefaultMinHeightTenths = 600;
constexpr uint16_t kDefaultMaxHeightTenths = 1250;
constexpr char kWebUiRoot[] = "/webui";
constexpr char kLittlefsPartitionLabel[] = "littlefs";
constexpr char kWebUiVersionPath[] = "/webui/version.txt";
httpd_handle_t server = nullptr;

const esp_partition_t *webui_partition() {
  return esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, kLittlefsPartitionLabel);
}

bool desk_ready() {
  return jiecang_transport::is_ready() || loctek_transport::is_ready();
}

jiecang_transport::MotionState desk_motion_state() {
  return loctek_transport::is_ready() ? loctek_transport::motion_state() : jiecang_transport::motion_state();
}

const char *desk_profile_name() {
  return loctek_transport::is_configured() ? "loctek_flexispot_rj45" : jiecang_transport::profile_name();
}

bool desk_height_tenths_cm(uint16_t *height) {
  return loctek_transport::is_ready() ? loctek_transport::height_tenths_cm(height)
                                      : jiecang_transport::height_tenths_cm(height);
}

uint32_t desk_state_sequence() {
  return loctek_transport::is_ready() ? loctek_transport::state_sequence()
                                      : jiecang_transport::state_sequence();
}

esp_err_t queue_desk_move(jiecang_transport::Direction direction, uint16_t duration_ms) {
  return loctek_transport::is_configured() ? loctek_transport::queue_move(direction, duration_ms)
                                           : jiecang_transport::queue_move(direction, duration_ms);
}

esp_err_t send_json(httpd_req_t *request, const char *status, const char *body) {
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_sendstr(request, body);
}

esp_err_t logs_handler(httpd_req_t *request) {
  uint64_t since = 0;
  if (httpd_req_get_url_query_len(request) > 0) {
    char query[96];
    const auto number = [&](const char *name, uint64_t *value) {
      char text[21];
      if (httpd_query_key_value(query, name, text, sizeof(text)) != ESP_OK) return false;
      const char *end = text + std::strlen(text);
      const auto result = std::from_chars(text, end, *value);
      return result.ec == std::errc{} && result.ptr == end;
    };
    uint64_t boot;
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        !number("since", &since) || !number("boot", &boot) || boot > UINT32_MAX) {
      return send_json(request, "400 Bad Request", "{\"error\":\"invalid_log_cursor\"}");
    }
    if (boot != logs::boot_id()) since = 0;
  }

  // A bounded snapshot keeps socket writes outside the logging lock and off the HTTP task stack.
  std::unique_ptr<char[]> text(new (std::nothrow) char[logs::kCapacity]);
  if (!text) return send_json(request, "503 Service Unavailable", "{\"error\":\"log_buffer_unavailable\"}");
  const auto snapshot = logs::read(since, text.get());
  char cursor[21];
  char boot[11];
  std::snprintf(cursor, sizeof(cursor), "%" PRIu64, snapshot.next);
  std::snprintf(boot, sizeof(boot), "%" PRIu32, logs::boot_id());
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "X-Log-Cursor", cursor);
  httpd_resp_set_hdr(request, "X-Log-Boot", boot);
  httpd_resp_set_hdr(request, "X-Log-Dropped", snapshot.dropped ? "1" : "0");
  return httpd_resp_send(request, text.get(), snapshot.size);
}

esp_err_t info_handler(httpd_req_t *request) {
  const esp_app_desc_t *firmware = esp_app_get_description();
  esp_chip_info_t chip{};
  esp_chip_info(&chip);
  multi_heap_info_t memory{};
  heap_caps_get_info(&memory, MALLOC_CAP_8BIT);
  uint32_t flash_size = 0;
  const bool has_flash_size = esp_flash_get_size(nullptr, &flash_size) == ESP_OK;
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *webui = webui_partition();
  char sha256[65]{};
  esp_app_get_elf_sha256(sha256, sizeof(sha256));
  cJSON *response = cJSON_CreateObject();
  if (response == nullptr ||
      !cJSON_AddStringToObject(response, "target", CONFIG_IDF_TARGET) ||
      !cJSON_AddStringToObject(response, "pcb_revision", kPcbRevision) ||
      !cJSON_AddNumberToObject(response, "chip_revision", chip.revision) ||
      !cJSON_AddNumberToObject(response, "cpu_cores", chip.cores) ||
      !cJSON_AddNumberToObject(response, "cpu_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ) ||
      !(has_flash_size ? cJSON_AddNumberToObject(response, "flash_bytes", flash_size)
                       : cJSON_AddNullToObject(response, "flash_bytes")) ||
      !cJSON_AddStringToObject(response, "project", firmware->project_name) ||
      !cJSON_AddStringToObject(response, "sdk_version", firmware->idf_ver) ||
      !cJSON_AddStringToObject(response, "build_date", firmware->date) ||
      !cJSON_AddStringToObject(response, "build_time", firmware->time) ||
      !cJSON_AddStringToObject(response, "elf_sha256", sha256) ||
      !cJSON_AddStringToObject(response, "firmware_partition", running ? running->label : "unknown") ||
      !(running ? cJSON_AddNumberToObject(response, "firmware_partition_bytes", running->size)
                : cJSON_AddNullToObject(response, "firmware_partition_bytes")) ||
      !(webui ? cJSON_AddNumberToObject(response, "webui_partition_bytes", webui->size)
              : cJSON_AddNullToObject(response, "webui_partition_bytes")) ||
      !cJSON_AddNumberToObject(response, "reset_reason", esp_reset_reason()) ||
      !cJSON_AddNumberToObject(response, "heap_free_bytes", memory.total_free_bytes) ||
      !cJSON_AddNumberToObject(response, "heap_min_free_bytes", memory.minimum_free_bytes) ||
      !cJSON_AddNumberToObject(response, "heap_largest_block_bytes", memory.largest_free_block) ||
      !cJSON_AddNumberToObject(response, "task_count", uxTaskGetNumberOfTasks()) ||
      !cJSON_AddNumberToObject(response, "log_capacity_bytes", logs::kCapacity)) {
    cJSON_Delete(response);
    return send_json(request, "500 Internal Server Error", "{\"error\":\"response_failed\"}");
  }
  char *json = cJSON_PrintUnformatted(response);
  cJSON_Delete(response);
  if (json == nullptr) return send_json(request, "500 Internal Server Error", "{\"error\":\"response_failed\"}");
  const esp_err_t result = send_json(request, "200 OK", json);
  cJSON_free(json);
  return result;
}

esp_err_t state_handler(httpd_req_t *request) {
  char filesystem_version[32] = "unknown";
  FILE *version_file = std::fopen(kWebUiVersionPath, "r");
  if (version_file != nullptr) {
    if (std::fgets(filesystem_version, sizeof(filesystem_version), version_file) != nullptr) {
      filesystem_version[std::strcspn(filesystem_version, "\r\n")] = '\0';
      for (char *cursor = filesystem_version; *cursor != '\0'; ++cursor) {
        if (!(std::isalnum(static_cast<unsigned char>(*cursor)) || *cursor == '.' || *cursor == '-' || *cursor == '+')) {
          std::strcpy(filesystem_version, "unknown");
          break;
        }
      }
      if (filesystem_version[0] == '\0') std::strcpy(filesystem_version, "unknown");
    }
    std::fclose(version_file);
  }
  const esp_app_desc_t *firmware = esp_app_get_description();
  const bool ready = desk_ready();
  const auto motion = desk_motion_state();
  const char *state = ready ? "READY" : "SAFE_UNCONFIGURED";
  const char *profile_name = desk_profile_name();
  uint16_t height_tenths_cm = 0;
  const bool has_height = ready && desk_height_tenths_cm(&height_tenths_cm);
  bool locked = false;
  const bool has_lock = !loctek_transport::is_configured() && jiecang_transport::child_locked(&locked);
  const auto wifi_status = wifi::status();
  const auto mqtt_status = mqtt::status();
  cJSON *response = cJSON_CreateObject();
  cJSON *height = response == nullptr ? nullptr : cJSON_AddObjectToObject(response, "height");
  cJSON *network = response == nullptr ? nullptr : cJSON_AddObjectToObject(response, "network");
  cJSON *mqtt = response == nullptr ? nullptr : cJSON_AddObjectToObject(response, "mqtt");
  // cJSON escapes network names, including quotes and control characters.
  if (response == nullptr || height == nullptr || network == nullptr || mqtt == nullptr ||
      !cJSON_AddStringToObject(response, "state", state) ||
      !(profile_name ? cJSON_AddStringToObject(response, "profile", profile_name)
                     : cJSON_AddNullToObject(response, "profile")) ||
      !cJSON_AddBoolToObject(response, "ready", ready) ||
      !(has_height ? cJSON_AddNumberToObject(height, "value", height_tenths_cm / 10.0)
                   : cJSON_AddNullToObject(height, "value")) ||
      !cJSON_AddStringToObject(height, "unit", "cm") ||
      !cJSON_AddStringToObject(height, "quality", has_height ? "measured" : "unknown") ||
      !(motion == jiecang_transport::MotionState::unknown ? cJSON_AddNullToObject(response, "moving")
          : cJSON_AddBoolToObject(response, "moving", motion == jiecang_transport::MotionState::moving)) ||
      !(has_lock ? cJSON_AddBoolToObject(response, "locked", locked)
                 : cJSON_AddNullToObject(response, "locked")) ||
      !cJSON_AddNumberToObject(response, "sequence", desk_state_sequence()) ||
      !cJSON_AddStringToObject(response, "firmware_version", firmware->version) ||
      !cJSON_AddStringToObject(response, "filesystem_version", filesystem_version) ||
      !cJSON_AddNumberToObject(response, "uptime", esp_timer_get_time() / 1000000) ||
      !cJSON_AddBoolToObject(network, "connected", wifi_status.connected) ||
      !cJSON_AddBoolToObject(network, "mdns_available", wifi::mdns_available()) ||
      !cJSON_AddStringToObject(network, "mode", wifi_status.access_point ? "ap" : "station") ||
      !cJSON_AddStringToObject(network, "ssid", wifi_status.ssid) ||
      !(wifi_status.connected ? cJSON_AddNumberToObject(network, "rssi", wifi_status.rssi)
                             : cJSON_AddNullToObject(network, "rssi")) ||
      !cJSON_AddStringToObject(network, "hostname", wifi::hostname()) ||
      !cJSON_AddStringToObject(network, "mac", wifi_status.mac) ||
      !cJSON_AddStringToObject(network, "ip", wifi_status.ip) ||
      !cJSON_AddBoolToObject(mqtt, "connected", mqtt_status.connected) ||
      !cJSON_AddStringToObject(mqtt, "host", mqtt_status.host) ||
      !cJSON_AddNumberToObject(mqtt, "port", mqtt_status.port) ||
      !cJSON_AddStringToObject(mqtt, "state_topic", mqtt_status.state_topic) ||
      !cJSON_AddStringToObject(mqtt, "availability_topic", mqtt_status.availability_topic)) {
    cJSON_Delete(response);
    return send_json(request, "500 Internal Server Error", "{\"error\":\"response_failed\"}");
  }
  cJSON *presets = cJSON_AddArrayToObject(response, "presets");
  if (presets == nullptr) {
    cJSON_Delete(response);
    return send_json(request, "500 Internal Server Error", "{\"error\":\"response_failed\"}");
  }
  for (uint8_t preset = 1; preset <= 4; ++preset) {
    uint16_t preset_height = 0;
    const bool available = !loctek_transport::is_configured() && jiecang_transport::preset_height_tenths_cm(preset, &preset_height);
    if (!cJSON_AddItemToArray(presets, available ? cJSON_CreateNumber(preset_height / 10.0) : cJSON_CreateNull())) {
      cJSON_Delete(response);
      return send_json(request, "500 Internal Server Error", "{\"error\":\"response_failed\"}");
    }
  }
  char *json = cJSON_PrintUnformatted(response);
  cJSON_Delete(response);
  if (json == nullptr) return send_json(request, "500 Internal Server Error", "{\"error\":\"response_failed\"}");
  const esp_err_t send_result = send_json(request, "200 OK", json);
  cJSON_free(json);
  return send_result;
}

esp_err_t capabilities_handler(httpd_req_t *request) {
  const bool ready = desk_ready();
  const bool target_height = jiecang_transport::supports_target_height();
  char response[160];
  std::snprintf(response, sizeof(response),
                "{\"profile_configured\":%s,\"move\":%s,\"stop\":%s,"
                "\"target_height\":%s,\"presets\":%s,\"child_lock\":%s}",
                ready ? "true" : "false", ready ? "true" : "false",
                ready ? "true" : "false", target_height ? "true" : "false",
                !loctek_transport::is_configured() && jiecang_transport::supports_presets() ? "true" : "false",
                !loctek_transport::is_configured() && jiecang_transport::supports_child_lock() ? "true" : "false");
  return send_json(request, "200 OK", response);
}

bool receive_body(httpd_req_t *request, char *body, size_t capacity) {
  if (request->content_len == 0 || request->content_len >= capacity) return false;
  size_t received = 0;
  while (received < request->content_len) {
    const int count = httpd_req_recv(request, body + received, request->content_len - received);
    if (count <= 0) return false;
    received += static_cast<size_t>(count);
  }
  body[received] = '\0';
  return true;
}

bool parse_move_request(httpd_req_t *request, jiecang_transport::Direction *out_direction,
                        uint16_t *duration_ms) {
  if (out_direction == nullptr || duration_ms == nullptr) return false;
  char body[kMaxBodyLength + 1]{};
  if (!receive_body(request, body, sizeof(body))) return false;

  cJSON *root = cJSON_Parse(body);
  if (root == nullptr || !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return false;
  }
  const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
  const cJSON *direction = cJSON_GetObjectItemCaseSensitive(root, "direction");
  const cJSON *expires_ms = cJSON_GetObjectItemCaseSensitive(root, "expires_ms");
  const bool id_valid = cJSON_IsString(request_id) && request_id->valuestring != nullptr &&
                        request_id->valuestring[0] != '\0' &&
                        std::strlen(request_id->valuestring) <= kMaxRequestIdLength;
  const bool direction_valid = cJSON_IsString(direction) && direction->valuestring != nullptr &&
                               (std::strcmp(direction->valuestring, "up") == 0 ||
                                std::strcmp(direction->valuestring, "down") == 0 ||
                                std::strcmp(direction->valuestring, "stop") == 0);
  bool expiry_valid = false;
  if (direction_valid && std::strcmp(direction->valuestring, "stop") == 0) {
    expiry_valid = expires_ms == nullptr;
  } else if (cJSON_IsNumber(expires_ms) && expires_ms->valuedouble >= 1 &&
             expires_ms->valuedouble <= 5000 && expires_ms->valuedouble == expires_ms->valueint) {
    expiry_valid = true;
    *duration_ms = static_cast<uint16_t>(expires_ms->valueint);
  }
  if (direction_valid && std::strcmp(direction->valuestring, "stop") == 0) {
    *out_direction = jiecang_transport::Direction::stop;
    *duration_ms = 0;
  } else if (direction_valid && std::strcmp(direction->valuestring, "up") == 0) {
    *out_direction = jiecang_transport::Direction::up;
  } else if (direction_valid && std::strcmp(direction->valuestring, "down") == 0) {
    *out_direction = jiecang_transport::Direction::down;
  }
  cJSON_Delete(root);
  return id_valid && direction_valid && expiry_valid;
}

esp_err_t move_handler(httpd_req_t *request) {
  jiecang_transport::Direction direction{};
  uint16_t duration_ms = 0;
  if (!parse_move_request(request, &direction, &duration_ms)) {
    return send_json(request, "400 Bad Request", "{\"error\":\"invalid_request\"}");
  }
  const esp_err_t result = queue_desk_move(direction, duration_ms);
  if (result == ESP_ERR_NOT_ALLOWED) return send_json(request, "409 Conflict", "{\"error\":\"child_locked\"}");
  if (result == ESP_ERR_INVALID_STATE) {
    return send_json(request, "409 Conflict", "{\"error\":\"profile_not_ready\"}");
  }
  if (result != ESP_OK) return send_json(request, "503 Service Unavailable", "{\"error\":\"command_queue_full\"}");
  return send_json(request, "202 Accepted", "{\"accepted\":true}");
}

bool read_height_bounds(uint16_t *minimum, uint16_t *maximum) {
  *minimum = kDefaultMinHeightTenths;
  *maximum = kDefaultMaxHeightTenths;
  nvs_handle_t settings;
  const esp_err_t result = nvs_open("device", NVS_READONLY, &settings);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return false;
  const esp_err_t min_result = nvs_get_u16(settings, "min_h_tenths", minimum);
  const esp_err_t max_result = nvs_get_u16(settings, "max_h_tenths", maximum);
  nvs_close(settings);
  return (min_result == ESP_OK || min_result == ESP_ERR_NVS_NOT_FOUND) &&
         (max_result == ESP_OK || max_result == ESP_ERR_NVS_NOT_FOUND) &&
         *minimum < *maximum && *maximum <= 2000;
}

esp_err_t target_height_handler(httpd_req_t *request) {
  char body[kMaxBodyLength + 1]{};
  if (!receive_body(request, body, sizeof(body))) return send_json(request, "400 Bad Request", "{\"error\":\"invalid_request\"}");
  cJSON *root = cJSON_Parse(body);
  if (root == nullptr || !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return send_json(request, "400 Bad Request", "{\"error\":\"invalid_request\"}");
  }
  const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
  const cJSON *height = cJSON_GetObjectItemCaseSensitive(root, "height_cm");
  const bool valid = cJSON_IsString(request_id) && request_id->valuestring != nullptr &&
                     request_id->valuestring[0] != '\0' && std::strlen(request_id->valuestring) <= kMaxRequestIdLength &&
                     cJSON_IsNumber(height) && std::isfinite(height->valuedouble) &&
                     height->valuedouble >= 0 && height->valuedouble <= 200 &&
                     std::fabs(height->valuedouble * 10 - std::round(height->valuedouble * 10)) < 1e-7;
  if (!valid) {
    cJSON_Delete(root);
    return send_json(request, "400 Bad Request", "{\"error\":\"invalid_request\"}");
  }
  uint16_t minimum = 0, maximum = 0;
  if (!read_height_bounds(&minimum, &maximum)) {
    cJSON_Delete(root);
    return send_json(request, "500 Internal Server Error", "{\"error\":\"settings_read_failed\"}");
  }
  const double target_tenths = height->valuedouble * 10;
  cJSON_Delete(root);
  if (target_tenths < minimum || target_tenths > maximum) {
    return send_json(request, "400 Bad Request", "{\"error\":\"height_out_of_range\"}");
  }
  if (!desk_ready()) {
    return send_json(request, "409 Conflict", "{\"error\":\"profile_not_ready\"}");
  }
  if (!jiecang_transport::supports_target_height()) {
    return send_json(request, "409 Conflict", "{\"error\":\"target_height_unavailable\"}");
  }
  const esp_err_t result = jiecang_transport::queue_target_height(
      static_cast<uint16_t>(std::lround(target_tenths)));
  if (result == ESP_ERR_NOT_ALLOWED) return send_json(request, "409 Conflict", "{\"error\":\"child_locked\"}");
  if (result == ESP_ERR_INVALID_STATE) {
    return send_json(request, "409 Conflict", "{\"error\":\"profile_not_ready\"}");
  }
  if (result != ESP_OK) return send_json(request, "503 Service Unavailable", "{\"error\":\"command_queue_full\"}");
  return send_json(request, "202 Accepted", "{\"accepted\":true}");
}

esp_err_t preset_handler(httpd_req_t *request) {
  char body[kMaxBodyLength + 1]{};
  if (!receive_body(request, body, sizeof(body))) return send_json(request, "400 Bad Request", "{\"error\":\"invalid_request\"}");
  cJSON *root = cJSON_Parse(body);
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
  const cJSON *slot = cJSON_GetObjectItemCaseSensitive(root, "preset");
  const cJSON *operation = cJSON_GetObjectItemCaseSensitive(root, "operation");
  const bool save = cJSON_IsString(operation) && std::strcmp(operation->valuestring, "save") == 0;
  const bool valid = cJSON_IsObject(root) && cJSON_IsString(id) && id->valuestring[0] != '\0' &&
      std::strlen(id->valuestring) <= kMaxRequestIdLength && cJSON_IsNumber(slot) &&
      slot->valuedouble >= 1 && slot->valuedouble <= 4 && slot->valuedouble == slot->valueint &&
      cJSON_IsString(operation) && (save || std::strcmp(operation->valuestring, "recall") == 0);
  const uint8_t preset = valid ? static_cast<uint8_t>(slot->valueint) : 0;
  cJSON_Delete(root);
  if (!valid) return send_json(request, "400 Bad Request", "{\"error\":\"invalid_request\"}");
  if (!desk_ready()) return send_json(request, "409 Conflict", "{\"error\":\"profile_not_ready\"}");
  if (loctek_transport::is_configured() || !jiecang_transport::supports_presets()) {
    return send_json(request, "409 Conflict", "{\"error\":\"presets_unavailable\"}");
  }
  uint16_t minimum = 0, maximum = 0;
  if (!save && !read_height_bounds(&minimum, &maximum)) {
    return send_json(request, "500 Internal Server Error", "{\"error\":\"settings_read_failed\"}");
  }
  const esp_err_t result = save ? jiecang_transport::queue_save_preset(preset)
                              : jiecang_transport::queue_preset(preset, minimum, maximum);
  if (result == ESP_ERR_NOT_ALLOWED) return send_json(request, "409 Conflict", "{\"error\":\"child_locked\"}");
  if (result == ESP_ERR_NOT_SUPPORTED) return send_json(request, "409 Conflict", "{\"error\":\"presets_unavailable\"}");
  if (result == ESP_ERR_NOT_FOUND) return send_json(request, "409 Conflict", "{\"error\":\"preset_unavailable\"}");
  if (result == ESP_ERR_INVALID_ARG) return send_json(request, "400 Bad Request", "{\"error\":\"height_out_of_range\"}");
  if (result != ESP_OK) return send_json(request, "503 Service Unavailable", "{\"error\":\"command_queue_full\"}");
  return send_json(request, "202 Accepted", "{\"accepted\":true}");
}

struct ChildLockJob {
  httpd_req_t *request = nullptr;
  bool wanted = false;
};
ChildLockJob gChildLockJob;
std::atomic<bool> gChildLockJobActive{false};

esp_err_t send_child_lock_result(httpd_req_t *request, bool wanted, esp_err_t result) {
  if (result == ESP_ERR_TIMEOUT) return send_json(request, "504 Gateway Timeout", "{\"error\":\"child_lock_not_confirmed\"}");
  if (result == ESP_ERR_NOT_SUPPORTED) return send_json(request, "409 Conflict", "{\"error\":\"child_lock_unavailable\"}");
  if (result == ESP_ERR_INVALID_STATE) return send_json(request, "409 Conflict", "{\"error\":\"child_lock_busy\"}");
  if (result != ESP_OK) return send_json(request, "503 Service Unavailable", "{\"error\":\"command_queue_full\"}");
  return send_json(request, "200 OK", wanted ? "{\"locked\":true}" : "{\"locked\":false}");
}

void child_lock_worker(void *context) {
  const ChildLockJob job = *static_cast<ChildLockJob *>(context);
  const esp_err_t result = jiecang_transport::set_child_lock(job.wanted);
  send_child_lock_result(job.request, job.wanted, result);
  httpd_req_async_handler_complete(job.request);
  gChildLockJobActive.store(false);
  vTaskDelete(nullptr);
}

esp_err_t child_lock_handler(httpd_req_t *request) {
  char body[kMaxBodyLength + 1]{};
  if (!receive_body(request, body, sizeof(body))) return send_json(request, "400 Bad Request", "{\"error\":\"invalid_request\"}");
  cJSON *root = cJSON_Parse(body);
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
  const cJSON *locked = cJSON_GetObjectItemCaseSensitive(root, "locked");
  const bool valid = cJSON_IsObject(root) && cJSON_IsString(id) && id->valuestring != nullptr &&
                     id->valuestring[0] != '\0' && std::strlen(id->valuestring) <= kMaxRequestIdLength &&
                     cJSON_IsBool(locked);
  const bool wanted = cJSON_IsTrue(locked);
  cJSON_Delete(root);
  if (!valid) return send_json(request, "400 Bad Request", "{\"error\":\"invalid_request\"}");
  if (loctek_transport::is_configured() || !jiecang_transport::supports_child_lock()) {
    return send_json(request, "409 Conflict", "{\"error\":\"child_lock_unavailable\"}");
  }
  bool idle = false;
  if (!gChildLockJobActive.compare_exchange_strong(idle, true)) {
    return send_json(request, "409 Conflict", "{\"error\":\"child_lock_busy\"}");
  }
  if (httpd_req_async_handler_begin(request, &gChildLockJob.request) != ESP_OK) {
    gChildLockJobActive.store(false);
    return send_json(request, "503 Service Unavailable", "{\"error\":\"child_lock_busy\"}");
  }
  gChildLockJob.wanted = wanted;
  // Confirmation must not occupy the HTTP server's status/STOP handler task.
  if (xTaskCreate(child_lock_worker, "desk_lock", 4096, &gChildLockJob, 5, nullptr) != pdPASS) {
    send_json(gChildLockJob.request, "503 Service Unavailable", "{\"error\":\"child_lock_busy\"}");
    httpd_req_async_handler_complete(gChildLockJob.request);
    gChildLockJobActive.store(false);
  }
  return ESP_OK;
}

esp_err_t wifi_settings_get_handler(httpd_req_t *request) {
  char ssid[kMaxSsidLength + 1]{};
  char password[kMaxPasswordLength + 1]{};
  nvs_handle_t settings;
  esp_err_t result = nvs_open("wifi", NVS_READONLY, &settings);
  if (result == ESP_OK) {
    size_t ssid_size = sizeof(ssid);
    size_t password_size = sizeof(password);
    const esp_err_t ssid_result = nvs_get_str(settings, "ssid", ssid, &ssid_size);
    const esp_err_t password_result = nvs_get_str(settings, "password", password, &password_size);
    nvs_close(settings);
    if ((ssid_result != ESP_OK && ssid_result != ESP_ERR_NVS_NOT_FOUND) ||
        (password_result != ESP_OK && password_result != ESP_ERR_NVS_NOT_FOUND)) {
      return send_json(request, "500 Internal Server Error", "{\"error\":\"settings_read_failed\"}");
    }
  } else if (result != ESP_ERR_NVS_NOT_FOUND) {
    return send_json(request, "503 Service Unavailable", "{\"error\":\"settings_unavailable\"}");
  }

  cJSON *response = cJSON_CreateObject();
  if (response == nullptr || !cJSON_AddStringToObject(response, "ssid", ssid) ||
      !cJSON_AddBoolToObject(response, "password_configured", password[0] != '\0')) {
    cJSON_Delete(response);
    return send_json(request, "500 Internal Server Error", "{\"error\":\"response_failed\"}");
  }
  char *json = cJSON_PrintUnformatted(response);
  cJSON_Delete(response);
  if (json == nullptr) return send_json(request, "500 Internal Server Error", "{\"error\":\"response_failed\"}");
  const esp_err_t send_result = send_json(request, "200 OK", json);
  cJSON_free(json);
  return send_result;
}

void reboot_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(1500));
  esp_restart();
}

esp_err_t wifi_settings_post_handler(httpd_req_t *request) {
  char body[kMaxBodyLength + 1]{};
  if (!receive_body(request, body, sizeof(body))) {
    return send_json(request, "400 Bad Request", "{\"error\":\"invalid_request\"}");
  }
  cJSON *root = cJSON_Parse(body);
  if (root == nullptr || !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return send_json(request, "400 Bad Request", "{\"error\":\"invalid_request\"}");
  }
  const cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
  const cJSON *password_item = cJSON_GetObjectItemCaseSensitive(root, "password");
  const char *ssid = cJSON_IsString(ssid_item) ? ssid_item->valuestring : nullptr;
  const char *password = cJSON_IsString(password_item) ? password_item->valuestring : nullptr;
  const size_t ssid_length = ssid == nullptr ? 0 : std::strlen(ssid);
  const size_t password_length = password == nullptr ? 0 : std::strlen(password);
  const bool valid = ssid != nullptr && password != nullptr && ssid_length > 0 &&
                     ssid_length <= kMaxSsidLength &&
                     (password_length == 0 || (password_length >= 8 && password_length <= kMaxPasswordLength));
  if (!valid) {
    cJSON_Delete(root);
    return send_json(request, "400 Bad Request", "{\"error\":\"invalid_wifi_settings\"}");
  }

  nvs_handle_t settings;
  esp_err_t result = nvs_open("wifi", NVS_READWRITE, &settings);
  if (result != ESP_OK) {
    cJSON_Delete(root);
    return send_json(request, "503 Service Unavailable", "{\"error\":\"settings_unavailable\"}");
  }
  result = nvs_set_str(settings, "ssid", ssid);
  if (result == ESP_OK) result = nvs_set_str(settings, "password", password);
  if (result == ESP_OK) result = nvs_commit(settings);
  nvs_close(settings);
  cJSON_Delete(root);
  if (result != ESP_OK) return send_json(request, "500 Internal Server Error", "{\"error\":\"save_failed\"}");
  if (!schedule_restart()) {
    return send_json(request, "500 Internal Server Error", "{\"error\":\"reboot_schedule_failed\",\"saved\":true}");
  }
  return send_json(request, "202 Accepted", "{\"saved\":true,\"rebooting\":true}");
}

bool valid_desk_profile(const char *profile) {
  return std::strcmp(profile, "") == 0 || std::strcmp(profile, "jiecang_rj12") == 0 ||
         std::strcmp(profile, "jiecang_jarvis_rj45") == 0 || std::strcmp(profile, "loctek_flexispot_rj45") == 0;
}

bool valid_jiecang_rj45_model(const char *model) {
  return std::strcmp(model, "FullyCB2C-A") == 0;
}

bool valid_loctek_model(const char *model) {
  return std::strcmp(model, "FLEXISPOT_E7_PRO_PLUS") == 0;
}

esp_err_t device_settings_get_handler(httpd_req_t *request) {
  char profile[33]{};
  char model[33]{};
  uint8_t brightness = status_led::kDefaultBrightness;
  uint16_t minimum = kDefaultMinHeightTenths, maximum = kDefaultMaxHeightTenths;
  nvs_handle_t settings;
  esp_err_t result = nvs_open("device", NVS_READONLY, &settings);
  if (result == ESP_OK) {
    size_t profile_size = sizeof(profile);
    size_t model_size = sizeof(model);
    const esp_err_t profile_result = nvs_get_str(settings, "desk_profile", profile, &profile_size);
    const esp_err_t model_result = nvs_get_str(settings, "desk_model", model, &model_size);
    const esp_err_t brightness_result = nvs_get_u8(settings, "led_brightness", &brightness);
    const esp_err_t min_result = nvs_get_u16(settings, "min_h_tenths", &minimum);
    const esp_err_t max_result = nvs_get_u16(settings, "max_h_tenths", &maximum);
    nvs_close(settings);
    if ((profile_result != ESP_OK && profile_result != ESP_ERR_NVS_NOT_FOUND) ||
        (model_result != ESP_OK && model_result != ESP_ERR_NVS_NOT_FOUND) ||
        (brightness_result != ESP_OK && brightness_result != ESP_ERR_NVS_NOT_FOUND) || brightness > 100 ||
        (min_result != ESP_OK && min_result != ESP_ERR_NVS_NOT_FOUND) ||
        (max_result != ESP_OK && max_result != ESP_ERR_NVS_NOT_FOUND)) {
      return send_json(request, "500 Internal Server Error", "{\"error\":\"settings_read_failed\"}");
    }
  } else if (result != ESP_ERR_NVS_NOT_FOUND) {
    return send_json(request, "503 Service Unavailable", "{\"error\":\"settings_unavailable\"}");
  }

  const char *normalized_model = std::strcmp(profile, "jiecang_rj12") == 0
      ? jiecang_rj12::normalize_model(model) : nullptr;
  cJSON *response = cJSON_CreateObject();
  if (minimum >= maximum || maximum > 2000 || response == nullptr ||
      !cJSON_AddStringToObject(response, "hostname", wifi::hostname()) ||
      !cJSON_AddStringToObject(response, "dhcp_hostname", wifi::dhcp_hostname()) ||
      !cJSON_AddStringToObject(response, "profile", profile) ||
      !cJSON_AddStringToObject(response, "model", normalized_model != nullptr ? normalized_model : model) ||
      !cJSON_AddNumberToObject(response, "led_brightness", brightness) ||
      !cJSON_AddNumberToObject(response, "min_height_cm", minimum / 10.0) ||
      !cJSON_AddNumberToObject(response, "max_height_cm", maximum / 10.0)) {
    cJSON_Delete(response);
    return send_json(request, "500 Internal Server Error", "{\"error\":\"response_failed\"}");
  }
  char *json = cJSON_PrintUnformatted(response);
  cJSON_Delete(response);
  if (json == nullptr) return send_json(request, "500 Internal Server Error", "{\"error\":\"response_failed\"}");
  const esp_err_t send_result = send_json(request, "200 OK", json);
  cJSON_free(json);
  return send_result;
}

esp_err_t device_settings_post_handler(httpd_req_t *request) {
  char body[kMaxBodyLength + 1]{};
  if (!receive_body(request, body, sizeof(body))) return send_json(request, "400 Bad Request", "{\"error\":\"invalid_request\"}");
  cJSON *root = cJSON_Parse(body);
  if (root == nullptr || !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return send_json(request, "400 Bad Request", "{\"error\":\"invalid_request\"}");
  }
  const cJSON *profile_item = cJSON_GetObjectItemCaseSensitive(root, "profile");
  const cJSON *model_item = cJSON_GetObjectItemCaseSensitive(root, "model");
  const cJSON *brightness_item = cJSON_GetObjectItemCaseSensitive(root, "led_brightness");
  const cJSON *min_item = cJSON_GetObjectItemCaseSensitive(root, "min_height_cm");
  const cJSON *max_item = cJSON_GetObjectItemCaseSensitive(root, "max_height_cm");
  const bool has_desk_settings = profile_item != nullptr || model_item != nullptr || min_item != nullptr || max_item != nullptr;
  const bool has_brightness = brightness_item != nullptr;
  const bool brightness_valid = brightness_item == nullptr ||
      (cJSON_IsNumber(brightness_item) && brightness_item->valuedouble >= 0 && brightness_item->valuedouble <= 100 &&
       brightness_item->valuedouble == brightness_item->valueint);
  const char *profile = cJSON_IsString(profile_item) && profile_item->valuestring != nullptr ? profile_item->valuestring : nullptr;
  const char *model = cJSON_IsString(model_item) && model_item->valuestring != nullptr ? model_item->valuestring : "";
  if (profile != nullptr && std::strcmp(profile, "jiecang_rj12") == 0) {
    model = jiecang_rj12::normalize_model(model);
  }
  const bool heights_valid = cJSON_IsNumber(min_item) && cJSON_IsNumber(max_item) &&
      std::isfinite(min_item->valuedouble) && std::isfinite(max_item->valuedouble) &&
      min_item->valuedouble >= 0 && max_item->valuedouble <= 200 &&
      min_item->valuedouble < max_item->valuedouble &&
      std::fabs(min_item->valuedouble * 10 - std::round(min_item->valuedouble * 10)) < 1e-7 &&
      std::fabs(max_item->valuedouble * 10 - std::round(max_item->valuedouble * 10)) < 1e-7;
  const bool model_valid = profile != nullptr &&
      (std::strcmp(profile, "jiecang_rj12") == 0 ? model != nullptr :
       std::strcmp(profile, "jiecang_jarvis_rj45") == 0 ? valid_jiecang_rj45_model(model) :
       std::strcmp(profile, "loctek_flexispot_rj45") == 0 ? valid_loctek_model(model) : model[0] == '\0');
  const bool desk_settings_valid = !has_desk_settings ||
      (profile_item != nullptr && model_item != nullptr && min_item != nullptr && max_item != nullptr &&
       profile != nullptr && cJSON_IsString(model_item) && valid_desk_profile(profile) && model_valid && heights_valid);
  const bool valid = (has_brightness || has_desk_settings) && brightness_valid && desk_settings_valid;
  if (!valid) {
    cJSON_Delete(root);
    return send_json(request, "400 Bad Request", "{\"error\":\"invalid_device_settings\"}");
  }
  const uint8_t brightness = brightness_item == nullptr ? status_led::kDefaultBrightness : static_cast<uint8_t>(brightness_item->valueint);
  const uint16_t minimum = heights_valid ? static_cast<uint16_t>(std::lround(min_item->valuedouble * 10)) : 0;
  const uint16_t maximum = heights_valid ? static_cast<uint16_t>(std::lround(max_item->valuedouble * 10)) : 0;
  nvs_handle_t settings;
  esp_err_t result = nvs_open("device", NVS_READWRITE, &settings);
  if (result != ESP_OK) {
    cJSON_Delete(root);
    return send_json(request, "503 Service Unavailable", "{\"error\":\"settings_unavailable\"}");
  }
  if (result == ESP_OK && profile_item != nullptr) result = nvs_set_str(settings, "desk_profile", profile);
  if (result == ESP_OK && model_item != nullptr) result = nvs_set_str(settings, "desk_model", model);
  if (result == ESP_OK && brightness_item != nullptr) result = nvs_set_u8(settings, "led_brightness", brightness);
  if (result == ESP_OK && has_desk_settings) result = nvs_set_u16(settings, "min_h_tenths", minimum);
  if (result == ESP_OK && has_desk_settings) result = nvs_set_u16(settings, "max_h_tenths", maximum);
  if (result == ESP_OK) result = nvs_commit(settings);
  nvs_close(settings);
  cJSON_Delete(root);
  if (result != ESP_OK) return send_json(request, "500 Internal Server Error", "{\"error\":\"save_failed\"}");

  if (has_brightness) status_led::set_brightness(brightness);
  if (has_desk_settings) mqtt::reload();
  return send_json(request, "200 OK", "{\"saved\":true}");
}

esp_err_t device_restart_handler(httpd_req_t *request) {
  if (!schedule_restart()) return send_json(request, "500 Internal Server Error", "{\"error\":\"reboot_schedule_failed\"}");
  return send_json(request, "202 Accepted", "{\"rebooting\":true}");
}

esp_err_t device_reset_handler(httpd_req_t *request) {
  const esp_err_t result = nvs_flash_erase();
  if (result != ESP_OK) return send_json(request, "500 Internal Server Error", "{\"error\":\"reset_failed\"}");
  if (!schedule_restart()) return send_json(request, "500 Internal Server Error", "{\"error\":\"reboot_schedule_failed\",\"reset\":true}");
  return send_json(request, "202 Accepted", "{\"reset\":true,\"rebooting\":true}");
}

esp_err_t mqtt_settings_get_handler(httpd_req_t *request) {
  uint8_t enabled = 0;
  uint16_t port = 1883;
  char host[kMaxMqttHostLength + 1]{};
  char username[kMaxMqttUsernameLength + 1]{};
  char password[kMaxMqttPasswordLength + 1]{};
  nvs_handle_t settings;
  esp_err_t result = nvs_open("mqtt", NVS_READONLY, &settings);
  if (result == ESP_OK) {
    size_t host_size = sizeof(host);
    size_t username_size = sizeof(username);
    size_t password_size = sizeof(password);
    const esp_err_t enabled_result = nvs_get_u8(settings, "enabled", &enabled);
    const esp_err_t port_result = nvs_get_u16(settings, "port", &port);
    const esp_err_t host_result = nvs_get_str(settings, "host", host, &host_size);
    const esp_err_t username_result = nvs_get_str(settings, "username", username, &username_size);
    const esp_err_t password_result = nvs_get_str(settings, "password", password, &password_size);
    nvs_close(settings);
    if ((enabled_result != ESP_OK && enabled_result != ESP_ERR_NVS_NOT_FOUND) ||
        (port_result != ESP_OK && port_result != ESP_ERR_NVS_NOT_FOUND) ||
        (host_result != ESP_OK && host_result != ESP_ERR_NVS_NOT_FOUND) ||
        (username_result != ESP_OK && username_result != ESP_ERR_NVS_NOT_FOUND) ||
        (password_result != ESP_OK && password_result != ESP_ERR_NVS_NOT_FOUND)) {
      return send_json(request, "500 Internal Server Error", "{\"error\":\"settings_read_failed\"}");
    }
  } else if (result != ESP_ERR_NVS_NOT_FOUND) {
    return send_json(request, "503 Service Unavailable", "{\"error\":\"settings_unavailable\"}");
  }
  cJSON *response = cJSON_CreateObject();
  if (response == nullptr || !cJSON_AddBoolToObject(response, "enabled", enabled != 0) ||
      !cJSON_AddNumberToObject(response, "port", port) || !cJSON_AddStringToObject(response, "host", host) ||
      !cJSON_AddStringToObject(response, "username", username) ||
      !cJSON_AddBoolToObject(response, "password_configured", password[0] != '\0')) {
    cJSON_Delete(response);
    return send_json(request, "500 Internal Server Error", "{\"error\":\"response_failed\"}");
  }
  char *json = cJSON_PrintUnformatted(response);
  cJSON_Delete(response);
  if (json == nullptr) return send_json(request, "500 Internal Server Error", "{\"error\":\"response_failed\"}");
  const esp_err_t send_result = send_json(request, "200 OK", json);
  cJSON_free(json);
  return send_result;
}

esp_err_t mqtt_settings_post_handler(httpd_req_t *request) {
  char body[kMaxBodyLength + 1]{};
  if (!receive_body(request, body, sizeof(body))) return send_json(request, "400 Bad Request", "{\"error\":\"invalid_request\"}");
  cJSON *root = cJSON_Parse(body);
  if (root == nullptr || !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return send_json(request, "400 Bad Request", "{\"error\":\"invalid_request\"}");
  }
  const cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(root, "enabled");
  const cJSON *host_item = cJSON_GetObjectItemCaseSensitive(root, "host");
  const cJSON *port_item = cJSON_GetObjectItemCaseSensitive(root, "port");
  const cJSON *username_item = cJSON_GetObjectItemCaseSensitive(root, "username");
  const cJSON *password_item = cJSON_GetObjectItemCaseSensitive(root, "password");
  const cJSON *clear_password_item = cJSON_GetObjectItemCaseSensitive(root, "clear_password");
  const char *host = cJSON_IsString(host_item) && host_item->valuestring != nullptr ? host_item->valuestring : nullptr;
  const char *username = cJSON_IsString(username_item) && username_item->valuestring != nullptr ? username_item->valuestring : nullptr;
  const char *password = cJSON_IsString(password_item) && password_item->valuestring != nullptr ? password_item->valuestring : nullptr;
  const size_t host_length = host == nullptr ? 0 : std::strlen(host);
  const size_t username_length = username == nullptr ? 0 : std::strlen(username);
  const size_t password_length = password == nullptr ? 0 : std::strlen(password);
  const bool enabled_valid = cJSON_IsBool(enabled_item);
  const bool valid = enabled_valid && host != nullptr && username != nullptr && password != nullptr &&
                     cJSON_IsNumber(port_item) && port_item->valuedouble >= 1 && port_item->valuedouble <= 65535 &&
                     port_item->valuedouble == port_item->valueint && host_length <= kMaxMqttHostLength &&
                     username_length <= kMaxMqttUsernameLength && password_length <= kMaxMqttPasswordLength &&
                     (!cJSON_IsTrue(enabled_item) || host_length > 0) &&
                     (clear_password_item == nullptr || cJSON_IsBool(clear_password_item));
  if (!valid) {
    cJSON_Delete(root);
    return send_json(request, "400 Bad Request", "{\"error\":\"invalid_mqtt_settings\"}");
  }

  char saved_password[kMaxMqttPasswordLength + 1]{};
  nvs_handle_t settings;
  esp_err_t result = nvs_open("mqtt", NVS_READWRITE, &settings);
  if (result != ESP_OK) {
    cJSON_Delete(root);
    return send_json(request, "503 Service Unavailable", "{\"error\":\"settings_unavailable\"}");
  }
  if (password_length == 0 && !cJSON_IsTrue(clear_password_item)) {
    size_t saved_password_size = sizeof(saved_password);
    const esp_err_t password_result = nvs_get_str(settings, "password", saved_password, &saved_password_size);
    if (password_result != ESP_OK && password_result != ESP_ERR_NVS_NOT_FOUND) result = password_result;
  }
  if (result == ESP_OK) result = nvs_set_u8(settings, "enabled", cJSON_IsTrue(enabled_item) ? 1 : 0);
  if (result == ESP_OK) result = nvs_set_str(settings, "host", host);
  if (result == ESP_OK) result = nvs_set_u16(settings, "port", static_cast<uint16_t>(port_item->valueint));
  if (result == ESP_OK) result = nvs_set_str(settings, "username", username);
  if (result == ESP_OK) result = nvs_set_str(settings, "password", password_length > 0 ? password : saved_password);
  if (result == ESP_OK) result = nvs_commit(settings);
  nvs_close(settings);
  cJSON_Delete(root);
  if (result != ESP_OK) return send_json(request, "500 Internal Server Error", "{\"error\":\"save_failed\"}");
  mqtt::reload();
  return send_json(request, "200 OK", "{\"saved\":true,\"restarting\":true}");
}

esp_err_t firmware_update_handler(httpd_req_t *request) {
  const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
  if (partition == nullptr || request->content_len == 0 || request->content_len > partition->size) {
    return send_json(request, "400 Bad Request", "{\"error\":\"invalid_firmware_size\"}");
  }
  esp_ota_handle_t update_handle = 0;
  esp_err_t result = esp_ota_begin(partition, request->content_len, &update_handle);
  if (result != ESP_OK) return send_json(request, "500 Internal Server Error", "{\"error\":\"update_begin_failed\"}");

  char chunk[1024];
  size_t remaining = request->content_len;
  while (remaining > 0) {
    const size_t wanted = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
    const int received = httpd_req_recv(request, chunk, wanted);
    if (received <= 0) {
      esp_ota_abort(update_handle);
      return send_json(request, "400 Bad Request", "{\"error\":\"update_upload_incomplete\"}");
    }
    result = esp_ota_write(update_handle, chunk, static_cast<size_t>(received));
    if (result != ESP_OK) {
      esp_ota_abort(update_handle);
      return send_json(request, "500 Internal Server Error", "{\"error\":\"update_write_failed\"}");
    }
    remaining -= static_cast<size_t>(received);
  }
  result = esp_ota_end(update_handle);
  if (result != ESP_OK) return send_json(request, "400 Bad Request", "{\"error\":\"invalid_firmware_image\"}");
  esp_app_desc_t description{};
  if (esp_ota_get_partition_description(partition, &description) != ESP_OK ||
      std::strcmp(description.project_name, "pandadesk_firmware") != 0) {
    return send_json(request, "400 Bad Request", "{\"error\":\"not_pandadesk_firmware\"}");
  }
  result = esp_ota_set_boot_partition(partition);
  if (result != ESP_OK) return send_json(request, "500 Internal Server Error", "{\"error\":\"update_select_failed\"}");
  if (!schedule_restart()) return send_json(request, "500 Internal Server Error", "{\"error\":\"reboot_schedule_failed\",\"installed\":true}");
  return send_json(request, "202 Accepted", "{\"installed\":true,\"rebooting\":true}");
}

esp_err_t filesystem_update_handler(httpd_req_t *request) {
  const esp_partition_t *partition = webui_partition();
  if (partition == nullptr || request->content_len == 0 || request->content_len > partition->size) {
    return send_json(request, "400 Bad Request", "{\"error\":\"invalid_filesystem_size\"}");
  }
  esp_err_t result = esp_vfs_littlefs_unregister(kLittlefsPartitionLabel);
  if (result != ESP_OK) return send_json(request, "500 Internal Server Error", "{\"error\":\"filesystem_unmount_failed\"}");
  result = esp_partition_erase_range(partition, 0, partition->size);
  char chunk[1024];
  size_t offset = 0;
  while (result == ESP_OK && offset < request->content_len) {
    const size_t wanted = request->content_len - offset < sizeof(chunk) ? request->content_len - offset : sizeof(chunk);
    const int received = httpd_req_recv(request, chunk, wanted);
    if (received <= 0) {
      result = ESP_FAIL;
      break;
    }
    result = esp_partition_write(partition, offset, chunk, static_cast<size_t>(received));
    offset += static_cast<size_t>(received);
  }
  const esp_err_t mount_result = mount_webui();
  FILE *version_file = mount_result == ESP_OK ? std::fopen(kWebUiVersionPath, "r") : nullptr;
  const bool has_version = version_file != nullptr && std::fgetc(version_file) != EOF;
  if (version_file != nullptr) std::fclose(version_file);
  if (result != ESP_OK || offset != request->content_len || mount_result != ESP_OK || !has_version) {
    return send_json(request, "400 Bad Request", "{\"error\":\"invalid_filesystem_image\"}");
  }
  if (!schedule_restart()) return send_json(request, "500 Internal Server Error", "{\"error\":\"reboot_schedule_failed\",\"installed\":true}");
  return send_json(request, "202 Accepted", "{\"installed\":true,\"rebooting\":true}");
}

const char *content_type(const char *path) {
  const char *extension = std::strrchr(path, '.');
  if (extension == nullptr) return "application/octet-stream";
  if (std::strcmp(extension, ".html") == 0) return "text/html; charset=utf-8";
  if (std::strcmp(extension, ".css") == 0) return "text/css; charset=utf-8";
  if (std::strcmp(extension, ".js") == 0) return "text/javascript; charset=utf-8";
  if (std::strcmp(extension, ".json") == 0) return "application/json";
  if (std::strcmp(extension, ".svg") == 0) return "image/svg+xml";
  if (std::strcmp(extension, ".png") == 0) return "image/png";
  if (std::strcmp(extension, ".jpg") == 0 || std::strcmp(extension, ".jpeg") == 0) return "image/jpeg";
  if (std::strcmp(extension, ".ico") == 0) return "image/x-icon";
  if (std::strcmp(extension, ".woff2") == 0) return "font/woff2";
  return "application/octet-stream";
}

bool safe_asset_path(const char *path) {
  if (path[0] != '/' || std::strchr(path, '%') != nullptr || std::strchr(path, '\\') != nullptr) return false;
  const char *segment = path + 1;
  for (const char *cursor = segment;; ++cursor) {
    if (*cursor == '/' || *cursor == '\0') {
      if (cursor - segment == 2 && segment[0] == '.' && segment[1] == '.') return false;
      if (*cursor == '\0') return true;
      segment = cursor + 1;
    } else if (static_cast<unsigned char>(*cursor) < 0x20) {
      return false;
    }
  }
}

bool stream_file(httpd_req_t *request, const char *path) {
  FILE *file = std::fopen(path, "rb");
  if (file == nullptr) return false;
  httpd_resp_set_type(request, content_type(path));
  httpd_resp_set_hdr(request, "Cache-Control", "no-cache");

  char chunk[1024];
  size_t count;
  while ((count = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
    if (httpd_resp_send_chunk(request, chunk, count) != ESP_OK) {
      std::fclose(file);
      return true;
    }
  }
  const bool read_failed = std::ferror(file) != 0;
  std::fclose(file);
  if (read_failed) return true;
  httpd_resp_send_chunk(request, nullptr, 0);
  return true;
}

esp_err_t webui_handler(httpd_req_t *request) {
  char uri[kMaxUriLength + 1]{};
  const size_t uri_length = ::strnlen(request->uri, sizeof(uri));
  if (uri_length >= sizeof(uri)) return httpd_resp_send_err(request, HTTPD_414_URI_TOO_LONG, "URI too long");
  std::memcpy(uri, request->uri, uri_length);
  if (char *query = std::strchr(uri, '?')) *query = '\0';
  if (uri[0] == '\0') std::strcpy(uri, "/");
  if (!safe_asset_path(uri)) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid path");
  if (std::strcmp(uri, "/") == 0) std::strcpy(uri, "/index.html");

  char path[sizeof(kWebUiRoot) + sizeof(uri)];
  std::snprintf(path, sizeof(path), "%s%s", kWebUiRoot, uri);
  if (std::strncmp(uri, "/api/", 5) != 0 && stream_file(request, path)) return ESP_OK;

  if (std::strcmp(uri, "/index.html") != 0 && std::strncmp(uri, "/api/", 5) != 0) {
    if (const char *portal_url = wifi::captive_portal_url()) {
      httpd_resp_set_status(request, "302 Found");
      httpd_resp_set_hdr(request, "Location", portal_url);
      httpd_resp_set_hdr(request, "Cache-Control", "no-store");
      httpd_resp_set_type(request, "text/html; charset=utf-8");
      // iOS needs a nonempty response to recognize the captive portal.
      return httpd_resp_sendstr(request, "<html><body>Open PandaDesk setup</body></html>");
    }
    std::snprintf(path, sizeof(path), "%s/index.html", kWebUiRoot);
    if (stream_file(request, path)) return ESP_OK;
  }
  return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "PandaDesk web files missing");
}

httpd_uri_t state_uri = {.uri = "/api/v1/state", .method = HTTP_GET, .handler = state_handler, .user_ctx = nullptr};
httpd_uri_t logs_uri = {.uri = "/api/v1/logs", .method = HTTP_GET, .handler = logs_handler, .user_ctx = nullptr};
httpd_uri_t info_uri = {.uri = "/api/v1/info", .method = HTTP_GET, .handler = info_handler, .user_ctx = nullptr};
httpd_uri_t capabilities_uri = {.uri = "/api/v1/capabilities", .method = HTTP_GET, .handler = capabilities_handler, .user_ctx = nullptr};
httpd_uri_t move_uri = {.uri = "/api/v1/commands/move", .method = HTTP_POST, .handler = move_handler, .user_ctx = nullptr};
httpd_uri_t target_height_uri = {.uri = "/api/v1/commands/height", .method = HTTP_POST, .handler = target_height_handler, .user_ctx = nullptr};
httpd_uri_t child_lock_uri = {.uri = "/api/v1/commands/child-lock", .method = HTTP_POST, .handler = child_lock_handler, .user_ctx = nullptr};
httpd_uri_t preset_uri = {.uri = "/api/v1/commands/preset", .method = HTTP_POST, .handler = preset_handler, .user_ctx = nullptr};
httpd_uri_t wifi_settings_get_uri = {.uri = "/api/v1/settings/wifi", .method = HTTP_GET, .handler = wifi_settings_get_handler, .user_ctx = nullptr};
httpd_uri_t wifi_settings_post_uri = {.uri = "/api/v1/settings/wifi", .method = HTTP_POST, .handler = wifi_settings_post_handler, .user_ctx = nullptr};
httpd_uri_t device_settings_get_uri = {.uri = "/api/v1/settings/device", .method = HTTP_GET, .handler = device_settings_get_handler, .user_ctx = nullptr};
httpd_uri_t device_settings_post_uri = {.uri = "/api/v1/settings/device", .method = HTTP_POST, .handler = device_settings_post_handler, .user_ctx = nullptr};
httpd_uri_t mqtt_settings_get_uri = {.uri = "/api/v1/settings/mqtt", .method = HTTP_GET, .handler = mqtt_settings_get_handler, .user_ctx = nullptr};
httpd_uri_t mqtt_settings_post_uri = {.uri = "/api/v1/settings/mqtt", .method = HTTP_POST, .handler = mqtt_settings_post_handler, .user_ctx = nullptr};
httpd_uri_t device_reset_uri = {.uri = "/api/v1/device/reset", .method = HTTP_POST, .handler = device_reset_handler, .user_ctx = nullptr};
httpd_uri_t device_restart_uri = {.uri = "/api/v1/device/restart", .method = HTTP_POST, .handler = device_restart_handler, .user_ctx = nullptr};
httpd_uri_t firmware_update_uri = {.uri = "/api/v1/update/firmware", .method = HTTP_POST, .handler = firmware_update_handler, .user_ctx = nullptr};
httpd_uri_t filesystem_update_uri = {.uri = "/api/v1/update/filesystem", .method = HTTP_POST, .handler = filesystem_update_handler, .user_ctx = nullptr};
httpd_uri_t webui_uri = {.uri = "/*", .method = HTTP_GET, .handler = webui_handler, .user_ctx = nullptr};
}  // namespace

bool schedule_restart() {
  return xTaskCreate(reboot_task, "pandadesk_reboot", 2048, nullptr, 5, nullptr) == pdPASS;
}

esp_err_t mount_webui() {
  const esp_partition_t *partition = webui_partition();
  if (partition == nullptr) {
    ESP_LOGE(kTag, "Web UI data/littlefs partition '%s' not found", kLittlefsPartitionLabel);
    return ESP_ERR_NOT_FOUND;
  }
  esp_vfs_littlefs_conf_t config{};
  config.base_path = kWebUiRoot;
  config.partition_label = kLittlefsPartitionLabel;
  config.format_if_mount_failed = false;
  const esp_err_t result = esp_vfs_littlefs_register(&config);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Could not mount web UI partition '%s' at 0x%08" PRIx32 ": %s",
             partition->label, partition->address, esp_err_to_name(result));
    return result;
  }
  ESP_LOGI(kTag, "Web UI mounted at %s from '%s': offset=0x%08" PRIx32 ", size=0x%08" PRIx32,
           kWebUiRoot, partition->label, partition->address, partition->size);
  return ESP_OK;
}

void start() {
  if (server != nullptr) return;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.lru_purge_enable = true;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 19;
  if (httpd_start(&server, &config) != ESP_OK) {
    server = nullptr;
    ESP_LOGE(kTag, "Could not start HTTP server");
    return;
  }
  if (httpd_register_uri_handler(server, &state_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &logs_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &info_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &capabilities_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &move_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &target_height_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &child_lock_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &preset_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &wifi_settings_get_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &wifi_settings_post_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &device_settings_get_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &device_settings_post_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &mqtt_settings_get_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &mqtt_settings_post_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &device_reset_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &device_restart_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &firmware_update_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &filesystem_update_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &webui_uri) != ESP_OK) {
    ESP_LOGE(kTag, "Could not register REST API routes");
    httpd_stop(server);
    server = nullptr;
    return;
  }
  ESP_LOGI(kTag, "REST API listening on port %d", config.server_port);
}
}  // namespace pandadesk::api
