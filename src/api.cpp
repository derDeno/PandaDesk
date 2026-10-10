#include "pandadesk/api.hpp"

#include <cstdio>
#include <cctype>
#include <cstring>
#include <cmath>

#include "driver/ledc.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pandadesk/jiecang_transport.hpp"
#include "pandadesk/loctek_transport.hpp"
#include "pandadesk/mqtt.hpp"
#include "pandadesk/pins.hpp"

namespace pandadesk::api {
namespace {
constexpr char kTag[] = "PandaDeskAPI";
constexpr size_t kMaxBodyLength = 512;
constexpr size_t kMaxRequestIdLength = 64;
constexpr size_t kMaxSsidLength = 32;
constexpr size_t kMaxPasswordLength = 63;
constexpr size_t kMaxMqttHostLength = 128;
constexpr size_t kMaxMqttUsernameLength = 64;
constexpr size_t kMaxMqttPasswordLength = 63;
constexpr size_t kMaxHostnameLength = 63;
constexpr size_t kMaxUriLength = CONFIG_HTTPD_MAX_URI_LEN;
constexpr uint16_t kDefaultMinHeightTenths = 600;
constexpr uint16_t kDefaultMaxHeightTenths = 1250;
constexpr char kWebUiRoot[] = "/webui";
constexpr char kLittlefsPartitionLabel[] = "littlefs";
httpd_handle_t server = nullptr;

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

esp_err_t state_handler(httpd_req_t *request) {
  char filesystem_version[32] = "unknown";
  FILE *version_file = std::fopen("/webui/version.txt", "r");
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
  char profile[40] = "null";
  if (profile_name != nullptr) std::snprintf(profile, sizeof(profile), "\"%s\"", profile_name);
  const char *moving = motion == jiecang_transport::MotionState::moving ? "true" :
                       motion == jiecang_transport::MotionState::stopped ? "false" : "null";
  uint16_t height_tenths_cm = 0;
  const bool has_height = ready && desk_height_tenths_cm(&height_tenths_cm);
  char height[16] = "null";
  if (has_height) {
    std::snprintf(height, sizeof(height), "%u.%u", static_cast<unsigned>(height_tenths_cm / 10),
                  static_cast<unsigned>(height_tenths_cm % 10));
  }
  char response[320];
  std::snprintf(response, sizeof(response),
                "{\"state\":\"%s\",\"profile\":%s,\"ready\":%s,"
                "\"height\":{\"value\":%s,\"unit\":\"cm\",\"quality\":\"%s\"},"
                "\"moving\":%s,\"locked\":null,\"sequence\":%lu,"
                "\"firmware_version\":\"%s\",\"filesystem_version\":\"%s\"}",
                state, profile, ready ? "true" : "false", height,
                has_height ? "measured" : "unknown", moving,
                static_cast<unsigned long>(desk_state_sequence()),
                firmware->version, filesystem_version);
  return send_json(request, "200 OK", response);
}

esp_err_t capabilities_handler(httpd_req_t *request) {
  const bool ready = desk_ready();
  const bool target_height = jiecang_transport::supports_target_height();
  char response[160];
  std::snprintf(response, sizeof(response),
                "{\"profile_configured\":%s,\"move\":%s,\"stop\":%s,"
                "\"target_height\":%s,\"presets\":false,\"child_lock\":false}",
                ready ? "true" : "false", ready ? "true" : "false",
                ready ? "true" : "false", target_height ? "true" : "false");
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
  if (result == ESP_ERR_INVALID_STATE) {
    return send_json(request, "409 Conflict", "{\"error\":\"profile_not_ready\"}");
  }
  if (result != ESP_OK) return send_json(request, "503 Service Unavailable", "{\"error\":\"command_queue_full\"}");
  return send_json(request, "202 Accepted", "{\"accepted\":true}");
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

bool schedule_reboot() {
  return xTaskCreate(reboot_task, "pandadesk_reboot", 2048, nullptr, 5, nullptr) == pdPASS;
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
  if (!schedule_reboot()) {
    return send_json(request, "500 Internal Server Error", "{\"error\":\"reboot_schedule_failed\",\"saved\":true}");
  }
  return send_json(request, "202 Accepted", "{\"saved\":true,\"rebooting\":true}");
}

bool valid_desk_profile(const char *profile) {
  return std::strcmp(profile, "") == 0 || std::strcmp(profile, "jiecang_rj12") == 0 ||
         std::strcmp(profile, "jiecang_jarvis_rj45") == 0 || std::strcmp(profile, "loctek_flexispot_rj45") == 0;
}

bool valid_hostname(const char *hostname) {
  const size_t length = hostname == nullptr ? 0 : std::strlen(hostname);
  if (length == 0 || length > kMaxHostnameLength || hostname[0] == '-' || hostname[length - 1] == '-') return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(hostname[i]);
    if (!std::isalnum(c) && c != '-') return false;
  }
  return true;
}

bool valid_jiecang_rj12_model(const char *model) {
  constexpr const char *models[] = {"JCB35M11C", "JCHT35K72C", "JCB36N2CA", "JCB36N2CA-230",
                                    "JCB36N2HAG-230", "JCHT35K9-003-v4", "JCB36NE2", "JCB36M", "JCB36NE2A-230"};
  for (const char *supported : models) {
    if (std::strcmp(model, supported) == 0) return true;
  }
  return false;
}

bool valid_jiecang_rj45_model(const char *model) {
  return std::strcmp(model, "FullyCB2C-A") == 0;
}

bool valid_loctek_model(const char *model) {
  return std::strcmp(model, "FLEXISPOT_E7_PRO_PLUS") == 0;
}

esp_err_t device_settings_get_handler(httpd_req_t *request) {
  char hostname[kMaxHostnameLength + 1] = "pandadesk";
  char profile[33]{};
  char model[33]{};
  uint8_t brightness = 100;
  uint16_t minimum = kDefaultMinHeightTenths, maximum = kDefaultMaxHeightTenths;
  nvs_handle_t settings;
  esp_err_t result = nvs_open("device", NVS_READONLY, &settings);
  if (result == ESP_OK) {
    size_t profile_size = sizeof(profile);
    size_t model_size = sizeof(model);
    const esp_err_t profile_result = nvs_get_str(settings, "desk_profile", profile, &profile_size);
    const esp_err_t model_result = nvs_get_str(settings, "desk_model", model, &model_size);
    size_t hostname_size = sizeof(hostname);
    const esp_err_t hostname_result = nvs_get_str(settings, "hostname", hostname, &hostname_size);
    const esp_err_t brightness_result = nvs_get_u8(settings, "led_brightness", &brightness);
    const esp_err_t min_result = nvs_get_u16(settings, "min_h_tenths", &minimum);
    const esp_err_t max_result = nvs_get_u16(settings, "max_h_tenths", &maximum);
    nvs_close(settings);
    if ((profile_result != ESP_OK && profile_result != ESP_ERR_NVS_NOT_FOUND) ||
        (model_result != ESP_OK && model_result != ESP_ERR_NVS_NOT_FOUND) ||
        (hostname_result != ESP_OK && hostname_result != ESP_ERR_NVS_NOT_FOUND) ||
        !valid_hostname(hostname) ||
        (brightness_result != ESP_OK && brightness_result != ESP_ERR_NVS_NOT_FOUND) || brightness > 100 ||
        (min_result != ESP_OK && min_result != ESP_ERR_NVS_NOT_FOUND) ||
        (max_result != ESP_OK && max_result != ESP_ERR_NVS_NOT_FOUND)) {
      return send_json(request, "500 Internal Server Error", "{\"error\":\"settings_read_failed\"}");
    }
  } else if (result != ESP_ERR_NVS_NOT_FOUND) {
    return send_json(request, "503 Service Unavailable", "{\"error\":\"settings_unavailable\"}");
  }

  cJSON *response = cJSON_CreateObject();
  if (minimum >= maximum || maximum > 2000 || response == nullptr || !cJSON_AddStringToObject(response, "hostname", hostname) ||
      !cJSON_AddStringToObject(response, "profile", profile) ||
      !cJSON_AddStringToObject(response, "model", model) ||
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
  const cJSON *hostname_item = cJSON_GetObjectItemCaseSensitive(root, "hostname");
  const cJSON *min_item = cJSON_GetObjectItemCaseSensitive(root, "min_height_cm");
  const cJSON *max_item = cJSON_GetObjectItemCaseSensitive(root, "max_height_cm");
  const bool has_desk_settings = profile_item != nullptr || model_item != nullptr || min_item != nullptr || max_item != nullptr;
  const bool has_hostname = hostname_item != nullptr;
  const bool has_brightness = brightness_item != nullptr;
  const bool has_device_settings = has_hostname || has_brightness;
  const char *hostname = cJSON_IsString(hostname_item) ? hostname_item->valuestring : nullptr;
  const bool hostname_valid = hostname_item == nullptr || valid_hostname(hostname);
  const bool brightness_valid = brightness_item == nullptr ||
      (cJSON_IsNumber(brightness_item) && brightness_item->valuedouble >= 0 && brightness_item->valuedouble <= 100 &&
       brightness_item->valuedouble == brightness_item->valueint);
  const char *profile = cJSON_IsString(profile_item) && profile_item->valuestring != nullptr ? profile_item->valuestring : nullptr;
  const char *model = cJSON_IsString(model_item) && model_item->valuestring != nullptr ? model_item->valuestring : "";
  const bool heights_valid = cJSON_IsNumber(min_item) && cJSON_IsNumber(max_item) &&
      std::isfinite(min_item->valuedouble) && std::isfinite(max_item->valuedouble) &&
      min_item->valuedouble >= 0 && max_item->valuedouble <= 200 &&
      min_item->valuedouble < max_item->valuedouble &&
      std::fabs(min_item->valuedouble * 10 - std::round(min_item->valuedouble * 10)) < 1e-7 &&
      std::fabs(max_item->valuedouble * 10 - std::round(max_item->valuedouble * 10)) < 1e-7;
  const bool model_valid = profile != nullptr &&
      (std::strcmp(profile, "jiecang_rj12") == 0 ? valid_jiecang_rj12_model(model) :
       std::strcmp(profile, "jiecang_jarvis_rj45") == 0 ? valid_jiecang_rj45_model(model) :
       std::strcmp(profile, "loctek_flexispot_rj45") == 0 ? valid_loctek_model(model) : model[0] == '\0');
  const bool desk_settings_valid = !has_desk_settings ||
      (profile_item != nullptr && model_item != nullptr && min_item != nullptr && max_item != nullptr &&
       profile != nullptr && cJSON_IsString(model_item) && valid_desk_profile(profile) && model_valid && heights_valid);
  const bool valid = (has_device_settings || has_desk_settings) && hostname_valid && brightness_valid && desk_settings_valid;
  if (!valid) {
    cJSON_Delete(root);
    return send_json(request, "400 Bad Request", "{\"error\":\"invalid_device_settings\"}");
  }
  const uint8_t brightness = brightness_item == nullptr ? 100 : static_cast<uint8_t>(brightness_item->valueint);
  const uint16_t minimum = heights_valid ? static_cast<uint16_t>(std::lround(min_item->valuedouble * 10)) : 0;
  const uint16_t maximum = heights_valid ? static_cast<uint16_t>(std::lround(max_item->valuedouble * 10)) : 0;
  char previous_hostname[kMaxHostnameLength + 1] = "pandadesk";
  nvs_handle_t settings;
  esp_err_t result = nvs_open("device", NVS_READWRITE, &settings);
  if (result != ESP_OK) {
    cJSON_Delete(root);
    return send_json(request, "503 Service Unavailable", "{\"error\":\"settings_unavailable\"}");
  }
  size_t hostname_size = sizeof(previous_hostname);
  const esp_err_t hostname_read = nvs_get_str(settings, "hostname", previous_hostname, &hostname_size);
  if (hostname_read != ESP_OK && hostname_read != ESP_ERR_NVS_NOT_FOUND) result = hostname_read;
  const bool hostname_changed = has_hostname && std::strcmp(hostname, previous_hostname) != 0;
  if (result == ESP_OK && has_hostname) result = nvs_set_str(settings, "hostname", hostname);
  if (result == ESP_OK && profile_item != nullptr) result = nvs_set_str(settings, "desk_profile", profile);
  if (result == ESP_OK && model_item != nullptr) result = nvs_set_str(settings, "desk_model", model);
  if (result == ESP_OK && brightness_item != nullptr) result = nvs_set_u8(settings, "led_brightness", brightness);
  if (result == ESP_OK && has_desk_settings) result = nvs_set_u16(settings, "min_h_tenths", minimum);
  if (result == ESP_OK && has_desk_settings) result = nvs_set_u16(settings, "max_h_tenths", maximum);
  if (result == ESP_OK) result = nvs_commit(settings);
  nvs_close(settings);
  cJSON_Delete(root);
  if (result != ESP_OK) return send_json(request, "500 Internal Server Error", "{\"error\":\"save_failed\"}");

  if (has_brightness) {
    constexpr uint32_t max_duty = (1U << LEDC_TIMER_13_BIT) - 1U;
    result = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, max_duty * brightness / 100);
    if (result == ESP_OK) result = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (result != ESP_OK) {
      ESP_LOGE(kTag, "Could not update status LED brightness: %s", esp_err_to_name(result));
      return send_json(request, "500 Internal Server Error", "{\"error\":\"led_update_failed\",\"saved\":true}");
    }
  }
  if (has_desk_settings) mqtt::reload();
  if (hostname_changed) {
    if (!schedule_reboot()) {
      return send_json(request, "500 Internal Server Error", "{\"error\":\"reboot_schedule_failed\",\"saved\":true}");
    }
    return send_json(request, "202 Accepted", "{\"saved\":true,\"rebooting\":true}");
  }
  return send_json(request, "200 OK", "{\"saved\":true}");
}

esp_err_t device_reset_handler(httpd_req_t *request) {
  const esp_err_t result = nvs_flash_erase();
  if (result != ESP_OK) return send_json(request, "500 Internal Server Error", "{\"error\":\"reset_failed\"}");
  if (!schedule_reboot()) return send_json(request, "500 Internal Server Error", "{\"error\":\"reboot_schedule_failed\",\"reset\":true}");
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
  if (!schedule_reboot()) return send_json(request, "500 Internal Server Error", "{\"error\":\"reboot_schedule_failed\",\"installed\":true}");
  return send_json(request, "202 Accepted", "{\"installed\":true,\"rebooting\":true}");
}

esp_err_t filesystem_update_handler(httpd_req_t *request) {
  const esp_partition_t *partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kLittlefsPartitionLabel);
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
  esp_vfs_littlefs_conf_t config{};
  config.base_path = kWebUiRoot;
  config.partition_label = kLittlefsPartitionLabel;
  config.format_if_mount_failed = false;
  const esp_err_t mount_result = esp_vfs_littlefs_register(&config);
  FILE *version_file = mount_result == ESP_OK ? std::fopen("/webui/version.txt", "r") : nullptr;
  const bool has_version = version_file != nullptr && std::fgetc(version_file) != EOF;
  if (version_file != nullptr) std::fclose(version_file);
  if (result != ESP_OK || offset != request->content_len || mount_result != ESP_OK || !has_version) {
    return send_json(request, "400 Bad Request", "{\"error\":\"invalid_filesystem_image\"}");
  }
  if (!schedule_reboot()) return send_json(request, "500 Internal Server Error", "{\"error\":\"reboot_schedule_failed\",\"installed\":true}");
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
    std::snprintf(path, sizeof(path), "%s/index.html", kWebUiRoot);
    if (stream_file(request, path)) return ESP_OK;
  }
  return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "PandaDesk web files missing");
}

httpd_uri_t state_uri = {.uri = "/api/v1/state", .method = HTTP_GET, .handler = state_handler, .user_ctx = nullptr};
httpd_uri_t capabilities_uri = {.uri = "/api/v1/capabilities", .method = HTTP_GET, .handler = capabilities_handler, .user_ctx = nullptr};
httpd_uri_t move_uri = {.uri = "/api/v1/commands/move", .method = HTTP_POST, .handler = move_handler, .user_ctx = nullptr};
httpd_uri_t target_height_uri = {.uri = "/api/v1/commands/height", .method = HTTP_POST, .handler = target_height_handler, .user_ctx = nullptr};
httpd_uri_t wifi_settings_get_uri = {.uri = "/api/v1/settings/wifi", .method = HTTP_GET, .handler = wifi_settings_get_handler, .user_ctx = nullptr};
httpd_uri_t wifi_settings_post_uri = {.uri = "/api/v1/settings/wifi", .method = HTTP_POST, .handler = wifi_settings_post_handler, .user_ctx = nullptr};
httpd_uri_t device_settings_get_uri = {.uri = "/api/v1/settings/device", .method = HTTP_GET, .handler = device_settings_get_handler, .user_ctx = nullptr};
httpd_uri_t device_settings_post_uri = {.uri = "/api/v1/settings/device", .method = HTTP_POST, .handler = device_settings_post_handler, .user_ctx = nullptr};
httpd_uri_t mqtt_settings_get_uri = {.uri = "/api/v1/settings/mqtt", .method = HTTP_GET, .handler = mqtt_settings_get_handler, .user_ctx = nullptr};
httpd_uri_t mqtt_settings_post_uri = {.uri = "/api/v1/settings/mqtt", .method = HTTP_POST, .handler = mqtt_settings_post_handler, .user_ctx = nullptr};
httpd_uri_t device_reset_uri = {.uri = "/api/v1/device/reset", .method = HTTP_POST, .handler = device_reset_handler, .user_ctx = nullptr};
httpd_uri_t firmware_update_uri = {.uri = "/api/v1/update/firmware", .method = HTTP_POST, .handler = firmware_update_handler, .user_ctx = nullptr};
httpd_uri_t filesystem_update_uri = {.uri = "/api/v1/update/filesystem", .method = HTTP_POST, .handler = filesystem_update_handler, .user_ctx = nullptr};
httpd_uri_t webui_uri = {.uri = "/*", .method = HTTP_GET, .handler = webui_handler, .user_ctx = nullptr};
}  // namespace

void start() {
  if (server != nullptr) return;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.lru_purge_enable = true;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 16;
  if (httpd_start(&server, &config) != ESP_OK) {
    server = nullptr;
    ESP_LOGE(kTag, "Could not start HTTP server");
    return;
  }
  if (httpd_register_uri_handler(server, &state_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &capabilities_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &move_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &target_height_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &wifi_settings_get_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &wifi_settings_post_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &device_settings_get_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &device_settings_post_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &mqtt_settings_get_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &mqtt_settings_post_uri) != ESP_OK ||
      httpd_register_uri_handler(server, &device_reset_uri) != ESP_OK ||
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
