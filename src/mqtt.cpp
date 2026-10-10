#include "pandadesk/mqtt.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "pandadesk/jiecang_transport.hpp"
#include "pandadesk/loctek_transport.hpp"
#include "pandadesk/wifi.hpp"

namespace pandadesk::mqtt {
namespace {
constexpr char kTag[] = "PandaDeskMQTT";
constexpr char kDiscoveryPrefix[] = "homeassistant";
constexpr char kHaStatusTopic[] = "homeassistant/status";
constexpr EventBits_t kReload = BIT0;
constexpr size_t kHostLength = 128;
constexpr size_t kUsernameLength = 64;
constexpr size_t kPasswordLength = 63;
constexpr uint16_t kDefaultMinHeight = 600;
constexpr uint16_t kDefaultMaxHeight = 1250;

EventGroupHandle_t gEvents = nullptr;
std::atomic<bool> gConnected{false};
char gDeviceId[13]{};
char gStateTopic[48]{};
char gAvailabilityTopic[48]{};
char gCommandTopic[48]{};

struct Settings {
  bool enabled = false;
  uint16_t port = 1883;
  char host[kHostLength + 1]{};
  char username[kUsernameLength + 1]{};
  char password[kPasswordLength + 1]{};
};

bool read_string(nvs_handle_t handle, const char *key, char *value, size_t capacity) {
  size_t size = capacity;
  const esp_err_t result = nvs_get_str(handle, key, value, &size);
  return result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND;
}

bool load_settings(Settings *settings) {
  if (settings == nullptr) return false;
  nvs_handle_t handle;
  if (nvs_open("mqtt", NVS_READONLY, &handle) != ESP_OK) return true;
  uint8_t enabled = 0;
  nvs_get_u8(handle, "enabled", &enabled);
  nvs_get_u16(handle, "port", &settings->port);
  const bool valid = read_string(handle, "host", settings->host, sizeof(settings->host)) &&
                     read_string(handle, "username", settings->username, sizeof(settings->username)) &&
                     read_string(handle, "password", settings->password, sizeof(settings->password));
  nvs_close(handle);
  settings->enabled = enabled != 0 && settings->host[0] != '\0' && settings->port != 0;
  return valid;
}

bool desk_ready() { return jiecang_transport::is_ready() || loctek_transport::is_ready(); }

bool target_height_supported() { return jiecang_transport::supports_target_height(); }

uint32_t desk_sequence() {
  return loctek_transport::is_ready() ? loctek_transport::state_sequence()
                                      : jiecang_transport::state_sequence();
}

bool read_height(uint16_t *height) {
  return loctek_transport::is_ready() ? loctek_transport::height_tenths_cm(height)
                                      : jiecang_transport::height_tenths_cm(height);
}

jiecang_transport::MotionState desk_motion() {
  return loctek_transport::is_ready() ? loctek_transport::motion_state()
                                      : jiecang_transport::motion_state();
}

esp_err_t queue_desk_move(jiecang_transport::Direction direction, uint16_t duration_ms) {
  return loctek_transport::is_configured() ? loctek_transport::queue_move(direction, duration_ms)
                                           : jiecang_transport::queue_move(direction, duration_ms);
}

const char *desk_profile() {
  if (loctek_transport::is_ready()) return "loctek_flexispot_rj45";
  return jiecang_transport::profile_name();
}

void load_height_limits(uint16_t *minimum, uint16_t *maximum) {
  *minimum = kDefaultMinHeight;
  *maximum = kDefaultMaxHeight;
  nvs_handle_t handle;
  if (nvs_open("device", NVS_READONLY, &handle) != ESP_OK) return;
  nvs_get_u16(handle, "min_h_tenths", minimum);
  nvs_get_u16(handle, "max_h_tenths", maximum);
  nvs_close(handle);
}

int publish(esp_mqtt_client_handle_t client, const char *topic, const char *payload, bool retain = true) {
  if (client == nullptr || topic == nullptr || payload == nullptr) return ESP_FAIL;
  return esp_mqtt_client_publish(client, topic, payload, 0, 0, retain ? 1 : 0);
}

void add_common_config(cJSON *config, const char *entity_name, const char *entity_id) {
  char unique_id[64];
  std::snprintf(unique_id, sizeof(unique_id), "pandadesk_%s_%s", gDeviceId, entity_id);
  cJSON_AddStringToObject(config, "name", entity_name);
  cJSON_AddStringToObject(config, "unique_id", unique_id);
  cJSON_AddStringToObject(config, "state_topic", gStateTopic);
  cJSON_AddStringToObject(config, "availability_topic", gAvailabilityTopic);
  cJSON_AddStringToObject(config, "payload_available", "online");
  cJSON_AddStringToObject(config, "payload_not_available", "offline");
  cJSON *device = cJSON_AddObjectToObject(config, "device");
  cJSON *ids = cJSON_AddArrayToObject(device, "identifiers");
  cJSON_AddItemToArray(ids, cJSON_CreateString(gDeviceId));
  cJSON_AddStringToObject(device, "name", wifi::hostname());
  cJSON_AddStringToObject(device, "manufacturer", "PandaBoards");
  cJSON_AddStringToObject(device, "model", "PandaDesk ESP32-C6");
  cJSON_AddStringToObject(device, "sw_version", "firmware");
}

void publish_discovery_config(esp_mqtt_client_handle_t client, const char *component,
                              const char *entity_id, cJSON *config) {
  char topic[112];
  std::snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", kDiscoveryPrefix, component, gDeviceId, entity_id);
  char *payload = config == nullptr ? nullptr : cJSON_PrintUnformatted(config);
  if (config == nullptr) {
    publish(client, topic, "");
  } else if (payload != nullptr) {
    publish(client, topic, payload);
  }
  cJSON_free(payload);
  cJSON_Delete(config);
}

void publish_state(esp_mqtt_client_handle_t client) {
  uint16_t minimum = 0, maximum = 0, height = 0;
  load_height_limits(&minimum, &maximum);
  const bool has_height = desk_ready() && read_height(&height);
  const auto motion = desk_motion();
  const char *profile = desk_profile();
  cJSON *state = cJSON_CreateObject();
  cJSON_AddBoolToObject(state, "ready", desk_ready());
  if (has_height) cJSON_AddNumberToObject(state, "height_cm", height / 10.0);
  else cJSON_AddNullToObject(state, "height_cm");
  if (motion == jiecang_transport::MotionState::unknown) cJSON_AddNullToObject(state, "moving");
  else cJSON_AddBoolToObject(state, "moving", motion == jiecang_transport::MotionState::moving);
  if (profile == nullptr) cJSON_AddNullToObject(state, "profile");
  else cJSON_AddStringToObject(state, "profile", profile);
  cJSON_AddNumberToObject(state, "min_height_cm", minimum / 10.0);
  cJSON_AddNumberToObject(state, "max_height_cm", maximum / 10.0);
  char *payload = cJSON_PrintUnformatted(state);
  cJSON_Delete(state);
  if (payload != nullptr) {
    publish(client, gStateTopic, payload);
    cJSON_free(payload);
  }
}

void publish_sensor_config(esp_mqtt_client_handle_t client, const char *entity_id, const char *name,
                           const char *value_template, const char *unit = nullptr,
                           const char *device_class = nullptr) {
  cJSON *config = cJSON_CreateObject();
  add_common_config(config, name, entity_id);
  cJSON_AddStringToObject(config, "value_template", value_template);
  if (unit != nullptr) cJSON_AddStringToObject(config, "unit_of_measurement", unit);
  if (device_class != nullptr) cJSON_AddStringToObject(config, "device_class", device_class);
  if (std::strcmp(entity_id, "height") == 0) cJSON_AddStringToObject(config, "state_class", "measurement");
  publish_discovery_config(client, "sensor", entity_id, config);
}

void publish_binary_config(esp_mqtt_client_handle_t client, const char *entity_id, const char *name,
                           const char *value_template) {
  cJSON *config = cJSON_CreateObject();
  add_common_config(config, name, entity_id);
  cJSON_AddStringToObject(config, "value_template", value_template);
  cJSON_AddStringToObject(config, "payload_on", "ON");
  cJSON_AddStringToObject(config, "payload_off", "OFF");
  publish_discovery_config(client, "binary_sensor", entity_id, config);
}

void publish_button_config(esp_mqtt_client_handle_t client, const char *entity_id, const char *name,
                           bool supported) {
  if (!supported) {
    publish_discovery_config(client, "button", entity_id, nullptr);
    return;
  }
  char command_topic[64];
  std::snprintf(command_topic, sizeof(command_topic), "%s/%s", gCommandTopic, entity_id);
  cJSON *config = cJSON_CreateObject();
  add_common_config(config, name, entity_id);
  cJSON_AddStringToObject(config, "command_topic", command_topic);
  cJSON_AddStringToObject(config, "payload_press", "PRESS");
  publish_discovery_config(client, "button", entity_id, config);
}

void publish_target_config(esp_mqtt_client_handle_t client, bool supported) {
  if (!supported) {
    publish_discovery_config(client, "number", "target_height", nullptr);
    return;
  }
  uint16_t minimum = 0, maximum = 0;
  load_height_limits(&minimum, &maximum);
  char command_topic[64];
  std::snprintf(command_topic, sizeof(command_topic), "%s/target_height", gCommandTopic);
  cJSON *config = cJSON_CreateObject();
  add_common_config(config, "Target height", "target_height");
  cJSON_AddStringToObject(config, "command_topic", command_topic);
  cJSON_AddStringToObject(config, "value_template", "{{ value_json.height_cm }}");
  cJSON_AddNumberToObject(config, "min", minimum / 10.0);
  cJSON_AddNumberToObject(config, "max", maximum / 10.0);
  cJSON_AddNumberToObject(config, "step", 0.1);
  cJSON_AddStringToObject(config, "unit_of_measurement", "cm");
  cJSON_AddStringToObject(config, "mode", "slider");
  publish_discovery_config(client, "number", "target_height", config);
}

void publish_discovery(esp_mqtt_client_handle_t client) {
  publish_sensor_config(client, "height", "Desk height", "{{ value_json.height_cm }}", "cm", "distance");
  publish_sensor_config(client, "profile", "Desk profile", "{{ value_json.profile | default('unconfigured', true) }}");
  publish_binary_config(client, "ready", "Desk controller ready", "{{ 'ON' if value_json.ready else 'OFF' }}");
  publish_binary_config(client, "moving", "Desk moving",
                        "{{ 'ON' if value_json.moving == true else ('OFF' if value_json.moving == false else '') }}");
  const bool ready = desk_ready();
  publish_button_config(client, "up", "Move desk up", ready);
  publish_button_config(client, "down", "Move desk down", ready);
  publish_button_config(client, "stop", "Stop desk", ready);
  publish_target_config(client, target_height_supported());
  publish_state(client);
}

bool message_is(const esp_mqtt_event_handle_t event, const char *topic, const char *payload) {
  const size_t payload_size = std::strlen(payload);
  return event->topic != nullptr && event->topic_len == static_cast<int>(std::strlen(topic)) &&
         std::memcmp(event->topic, topic, event->topic_len) == 0 && event->data != nullptr &&
         event->data_len == static_cast<int>(payload_size) && std::memcmp(event->data, payload, payload_size) == 0;
}

void handle_target_command(esp_mqtt_event_handle_t event) {
  if (event->data == nullptr || event->data_len <= 0 || event->data_len >= 24 || !target_height_supported()) return;
  char value[24]{};
  std::memcpy(value, event->data, event->data_len);
  char *end = nullptr;
  const double centimeters = std::strtod(value, &end);
  if (end == value || *end != '\0' || !std::isfinite(centimeters) || centimeters < 0 || centimeters > 200) return;
  const double tenths = centimeters * 10;
  if (std::fabs(tenths - std::round(tenths)) > 1e-7) return;
  uint16_t minimum = 0, maximum = 0;
  load_height_limits(&minimum, &maximum);
  const auto target = static_cast<uint16_t>(std::lround(tenths));
  if (target < minimum || target > maximum) return;
  const esp_err_t result = jiecang_transport::queue_target_height(target);
  if (result != ESP_OK) ESP_LOGW(kTag, "Rejected Home Assistant target height: %s", esp_err_to_name(result));
}

void handle_command(esp_mqtt_event_handle_t event) {
  if (event->topic == nullptr || event->data == nullptr || event->data_len == 0) return;
  char topic[64];
  const char *commands[] = {"up", "down", "stop"};
  for (const char *command : commands) {
    std::snprintf(topic, sizeof(topic), "%s/%s", gCommandTopic, command);
    if (!message_is(event, topic, "PRESS")) continue;
    const auto direction = std::strcmp(command, "up") == 0 ? jiecang_transport::Direction::up :
                           std::strcmp(command, "down") == 0 ? jiecang_transport::Direction::down :
                           jiecang_transport::Direction::stop;
    const esp_err_t result = queue_desk_move(direction, direction == jiecang_transport::Direction::stop ? 0 : 1000);
    if (result != ESP_OK) ESP_LOGW(kTag, "Rejected Home Assistant desk command: %s", esp_err_to_name(result));
    return;
  }
  std::snprintf(topic, sizeof(topic), "%s/target_height", gCommandTopic);
  if (event->topic_len == static_cast<int>(std::strlen(topic)) && std::memcmp(event->topic, topic, event->topic_len) == 0) {
    handle_target_command(event);
    return;
  }
  if (message_is(event, kHaStatusTopic, "online")) publish_discovery(event->client);
}

void mqtt_event_handler(void *, esp_event_base_t, int32_t event_id, void *event_data) {
  auto event = static_cast<esp_mqtt_event_handle_t>(event_data);
  if (event == nullptr) return;
  if (event_id == MQTT_EVENT_CONNECTED) {
    gConnected.store(true);
    publish(event->client, gAvailabilityTopic, "online");
    char topic[64];
    const char *commands[] = {"up", "down", "stop", "target_height"};
    for (const char *command : commands) {
      std::snprintf(topic, sizeof(topic), "%s/%s", gCommandTopic, command);
      esp_mqtt_client_subscribe(event->client, topic, 0);
    }
    esp_mqtt_client_subscribe(event->client, kHaStatusTopic, 0);
    publish_discovery(event->client);
    ESP_LOGI(kTag, "Connected and announced Home Assistant entities");
  } else if (event_id == MQTT_EVENT_DISCONNECTED) {
    gConnected.store(false);
    ESP_LOGW(kTag, "Disconnected from MQTT broker");
  } else if (event_id == MQTT_EVENT_DATA) {
    handle_command(event);
  } else if (event_id == MQTT_EVENT_ERROR) {
    ESP_LOGW(kTag, "MQTT transport error");
  }
}

void set_device_id() {
  uint8_t mac[6]{};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  std::snprintf(gDeviceId, sizeof(gDeviceId), "%02x%02x%02x%02x%02x%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  std::snprintf(gStateTopic, sizeof(gStateTopic), "pandadesk/%s/state", gDeviceId);
  std::snprintf(gAvailabilityTopic, sizeof(gAvailabilityTopic), "pandadesk/%s/availability", gDeviceId);
  std::snprintf(gCommandTopic, sizeof(gCommandTopic), "pandadesk/%s/cmd", gDeviceId);
}

void mqtt_task(void *) {
  while (true) {
    Settings settings{};
    load_settings(&settings);
    esp_mqtt_client_handle_t client = nullptr;
    if (settings.enabled) {
      esp_mqtt_client_config_t config{};
      config.broker.address.hostname = settings.host;
      config.broker.address.port = settings.port;
      config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
      config.credentials.username = settings.username[0] == '\0' ? nullptr : settings.username;
      config.credentials.authentication.password = settings.password[0] == '\0' ? nullptr : settings.password;
      config.session.keepalive = 60;
      config.session.last_will.topic = gAvailabilityTopic;
      config.session.last_will.msg = "offline";
      config.session.last_will.qos = 0;
      config.session.last_will.retain = 1;
      client = esp_mqtt_client_init(&config);
      if (client != nullptr && esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler, nullptr) == ESP_OK &&
          esp_mqtt_client_start(client) == ESP_OK) {
      } else if (client != nullptr) {
        esp_mqtt_client_destroy(client);
        client = nullptr;
      }
    }
    if (client == nullptr && settings.enabled) ESP_LOGE(kTag, "Could not start MQTT client");

    uint32_t last_sequence = UINT32_MAX;
    uint32_t last_state_ms = 0;
    bool last_ready = false;
    bool last_target = false;
    if (client == nullptr) {
      xEventGroupWaitBits(gEvents, kReload, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
      continue;
    }
    while (true) {
      const EventBits_t bits = xEventGroupWaitBits(gEvents, kReload, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
      if ((bits & kReload) != 0) break;
      if (!gConnected.load()) continue;
      const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
      const bool ready = desk_ready();
      const bool target = target_height_supported();
      if (ready != last_ready || target != last_target) {
        publish_button_config(client, "up", "Move desk up", ready);
        publish_button_config(client, "down", "Move desk down", ready);
        publish_button_config(client, "stop", "Stop desk", ready);
        publish_target_config(client, target);
        last_ready = ready;
        last_target = target;
      }
      const uint32_t sequence = desk_sequence();
      if (sequence != last_sequence || now - last_state_ms >= 5000) {
        publish_state(client);
        last_sequence = sequence;
        last_state_ms = now;
      }
    }
    if (client != nullptr) {
      if (gConnected.load()) publish(client, gAvailabilityTopic, "offline");
      gConnected.store(false);
      esp_mqtt_client_stop(client);
      esp_mqtt_client_destroy(client);
    }
  }
}
}  // namespace

void start() {
  set_device_id();
  if (gEvents != nullptr) return;
  gEvents = xEventGroupCreate();
  if (gEvents == nullptr || xTaskCreate(mqtt_task, "pandadesk_mqtt", 6144, nullptr, 4, nullptr) != pdPASS) {
    ESP_LOGE(kTag, "Could not create MQTT task");
    return;
  }
}

void reload() {
  if (gEvents != nullptr) xEventGroupSetBits(gEvents, kReload);
}

}  // namespace pandadesk::mqtt
