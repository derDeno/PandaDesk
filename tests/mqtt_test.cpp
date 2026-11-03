#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NVS_NOT_FOUND = 1;
constexpr int ESP_ERR_INVALID_STATE = 2, ESP_ERR_NOT_ALLOWED = 3;
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
using EventBits_t = uint32_t;
using EventGroupHandle_t = void *;
constexpr unsigned BIT0 = 1, BIT1 = 2;
constexpr int pdTRUE = 1, pdFALSE = 0, pdPASS = 1;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;
constexpr int ESP_MAC_WIFI_STA = 0, MQTT_TRANSPORT_OVER_TCP = 0;
constexpr int MQTT_EVENT_ANY = -1, MQTT_EVENT_CONNECTED = 1, MQTT_EVENT_DISCONNECTED = 2;
constexpr int MQTT_EVENT_DATA = 3, MQTT_EVENT_ERROR = 4;
using nvs_handle_t = int;
using esp_event_base_t = const char *;
using esp_mqtt_client_handle_t = void *;
struct esp_mqtt_event_t {
  const char *topic = nullptr;
  int topic_len = 0;
  const char *data = nullptr;
  int data_len = 0, total_data_len = 0, current_data_offset = 0;
  bool retain = false;
  esp_mqtt_client_handle_t client = reinterpret_cast<void *>(1);
};
using esp_mqtt_event_handle_t = esp_mqtt_event_t *;
struct esp_mqtt_client_config_t {
  struct { struct { const char *hostname; uint16_t port; int transport; } address; } broker;
  struct { const char *username; struct { const char *password; } authentication; } credentials;
  struct { int keepalive; struct { const char *topic; const char *msg; int qos; int retain; } last_will; } session;
};

struct Publications {
  struct Message { std::string topic, payload; };
  std::vector<Message> messages;
  std::string &operator[](const std::string &topic) {
    for (auto &message : messages) if (message.topic == topic) return message.payload;
    messages.push_back({topic, ""});
    return messages.back().payload;
  }
  size_t count(const std::string &topic) const {
    for (const auto &message : messages) if (message.topic == topic) return 1;
    return 0;
  }
  const std::string &at(const std::string &topic) { assert(count(topic)); return (*this)[topic]; }
};
static Publications published;
static std::vector<std::string> subscribed;
static EventBits_t pending_bits = 0;
static void (*pending_task)(void *) = nullptr;
static bool task_failure = false, read_failure = false, write_failure = false;
static uint16_t min_height = 600, max_height = 1250;
static bool ready = true, loctek = false, support_target = true, support_presets = true;
static bool support_lock = true, locked = false, lock_known = true, restart_ok = true;
static unsigned moves = 0, saves = 0, recalls = 0, targets = 0, locks = 0, restarts = 0;
static uint16_t duration = 0, target = 0, recall_min = 0, recall_max = 0;
static uint8_t preset = 0;
static bool stopped = false;
static esp_err_t lock_result = ESP_OK;

static unsigned pdMS_TO_TICKS(unsigned value) { return value; }
static EventGroupHandle_t xEventGroupCreate() { return reinterpret_cast<void *>(1); }
static EventBits_t xEventGroupSetBits(EventGroupHandle_t, EventBits_t bits) { return pending_bits |= bits; }
static EventBits_t xEventGroupWaitBits(EventGroupHandle_t, EventBits_t, int, int, unsigned) { return 0; }
static int xTaskCreate(void (*task)(void *), const char *, unsigned, void *, unsigned, void *) {
  if (task_failure) return 0;
  pending_task = task;
  return pdPASS;
}
static void vTaskDelete(void *) {}
static void esp_read_mac(uint8_t *mac, int) { std::memset(mac, 0xab, 6); }
static int64_t esp_timer_get_time() { return 1000000; }
static esp_err_t nvs_open(const char *, int, nvs_handle_t *handle) {
  *handle = 1;
  return read_failure ? ESP_FAIL : ESP_OK;
}
static int nvs_get_str(nvs_handle_t, const char *, char *, size_t *) { return ESP_ERR_NVS_NOT_FOUND; }
static int nvs_get_u8(nvs_handle_t, const char *, uint8_t *) { return ESP_ERR_NVS_NOT_FOUND; }
static int nvs_get_u16(nvs_handle_t, const char *key, uint16_t *value) {
  if (std::strcmp(key, "min_h_tenths") == 0) *value = min_height;
  else if (std::strcmp(key, "max_h_tenths") == 0) *value = max_height;
  else return ESP_ERR_NVS_NOT_FOUND;
  return ESP_OK;
}
static int nvs_set_u16(nvs_handle_t, const char *key, uint16_t value) {
  if (write_failure) return ESP_FAIL;
  (std::strcmp(key, "min_h_tenths") == 0 ? min_height : max_height) = value;
  return ESP_OK;
}
static int nvs_commit(nvs_handle_t) { return ESP_OK; }
static void nvs_close(nvs_handle_t) {}
static int esp_mqtt_client_publish(void *, const char *topic, const char *payload, int, int, int retain) {
  assert(retain == 1);  // Discovery, state and availability are retained; commands are never published here.
  published[topic] = payload;
  return 1;
}
static int esp_mqtt_client_subscribe(void *, const char *topic, int) { subscribed.emplace_back(topic); return 1; }
static void *esp_mqtt_client_init(const esp_mqtt_client_config_t *) { return reinterpret_cast<void *>(1); }
static int esp_mqtt_client_register_event(void *, int, void (*)(void *, esp_event_base_t, int32_t, void *), void *) { return ESP_OK; }
static int esp_mqtt_client_start(void *) { return ESP_OK; }
static int esp_mqtt_client_stop(void *) { return ESP_OK; }
static int esp_mqtt_client_destroy(void *) { return ESP_OK; }

#include "../src/mqtt.cpp"

namespace pandadesk::wifi {
const char *dhcp_hostname() { return "PandaDesk-test"; }
}
namespace pandadesk::api {
bool schedule_restart() { ++restarts; return restart_ok; }
}
namespace pandadesk::jiecang_transport {
bool is_ready() { return ready && !loctek; }
bool supports_target_height() { return is_ready() && support_target; }
bool supports_presets() { return is_ready() && support_presets; }
bool supports_child_lock() { return is_ready() && support_lock; }
uint32_t state_sequence() { return 1; }
bool height_tenths_cm(uint16_t *height) { *height = 800; return ready; }
MotionState motion_state() { return MotionState::stopped; }
const char *profile_name() { return is_ready() ? "jiecang_rj12" : nullptr; }
bool child_locked(bool *value) { *value = locked; return is_ready() && lock_known; }
bool preset_height_tenths_cm(uint8_t slot, uint16_t *height) { *height = 700 + slot * 100; return slot != 2; }
esp_err_t queue_move(Direction direction, uint16_t milliseconds) {
  if (!ready) return ESP_ERR_INVALID_STATE;
  if (locked && direction != Direction::stop) return ESP_ERR_NOT_ALLOWED;
  ++moves; duration = milliseconds; stopped = direction == Direction::stop; return ESP_OK;
}
esp_err_t queue_target_height(uint16_t height) {
  if (locked) return ESP_ERR_NOT_ALLOWED;
  ++targets; target = height; return ESP_OK;
}
esp_err_t queue_preset(uint8_t slot, uint16_t minimum, uint16_t maximum) {
  if (locked) return ESP_ERR_NOT_ALLOWED;
  ++recalls; preset = slot; recall_min = minimum; recall_max = maximum; return ESP_OK;
}
esp_err_t queue_save_preset(uint8_t slot) {
  if (locked) return ESP_ERR_NOT_ALLOWED;
  ++saves; preset = slot; return ESP_OK;
}
esp_err_t set_child_lock(bool wanted) { ++locks; if (lock_result == ESP_OK) locked = wanted; return lock_result; }
}
namespace pandadesk::loctek_transport {
bool is_configured() { return loctek; }
bool is_ready() { return loctek && ready; }
uint32_t state_sequence() { return 1; }
bool height_tenths_cm(uint16_t *height) { *height = 800; return ready; }
jiecang_transport::MotionState motion_state() { return jiecang_transport::MotionState::stopped; }
esp_err_t queue_move(jiecang_transport::Direction direction, uint16_t milliseconds) {
  return jiecang_transport::queue_move(direction, milliseconds);
}
}

using namespace pandadesk::mqtt;
static void *client = reinterpret_cast<void *>(1);
static void command(const std::string &name, const std::string &payload, bool retain = false, bool fragment = false) {
  const std::string topic = std::string(gCommandTopic) + "/" + name;
  esp_mqtt_event_t event;
  event.topic = topic.c_str(); event.topic_len = static_cast<int>(topic.size());
  event.data = payload.data(); event.data_len = static_cast<int>(payload.size());
  event.total_data_len = event.data_len + (fragment ? 1 : 0); event.retain = retain;
  handle_command(&event);
}
static cJSON *config(const char *component, const char *id) {
  const std::string topic = std::string("homeassistant/") + component + "/" + gDeviceId + "/" + id + "/config";
  assert(published.count(topic));
  return cJSON_Parse(published.at(topic).c_str());
}
static const cJSON *field(const cJSON *json, const char *key) { return cJSON_GetObjectItemCaseSensitive(json, key); }

int main() {
  set_device_id(); gEvents = xEventGroupCreate();
  esp_mqtt_event_t connected;
  mqtt_event_handler(nullptr, nullptr, MQTT_EVENT_CONNECTED, &connected);
  assert(subscribed.size() == 2 && subscribed[0] == std::string(gCommandTopic) + "/+");
  for (const char *id : {"preset_1", "preset_4", "save_preset_1", "save_preset_4", "restart"}) {
    cJSON *json = config("button", id);
    assert(json && field(json, "command_topic") && !field(json, "state_topic"));
    assert(cJSON_IsFalse(field(json, "retain")));
    if (std::strcmp(id, "restart") == 0) assert(std::strcmp(field(json, "device_class")->valuestring, "restart") == 0);
    cJSON_Delete(json);
  }
  cJSON *json = config("lock", "child_lock");
  assert(json && cJSON_IsFalse(field(json, "optimistic")));
  cJSON_Delete(json);
  json = cJSON_Parse(published.at(gStateTopic).c_str());
  assert(cJSON_IsFalse(field(json, "locked")));
  assert(cJSON_GetArraySize(field(json, "presets")) == 4);
  assert(cJSON_IsNull(cJSON_GetArrayItem(field(json, "presets"), 1)));
  assert(cJSON_IsTrue(field(field(json, "capabilities"), "presets")));
  cJSON_Delete(json);

  for (uint8_t slot = 1; slot <= 4; ++slot) {
    command("preset_" + std::to_string(slot), "PRESS"); assert(preset == slot);
    command("save_preset_" + std::to_string(slot), "PRESS"); assert(preset == slot);
  }
  assert(saves == 4 && recalls == 4 && recall_min == 600 && recall_max == 1250);
  command("preset_0", "PRESS"); command("preset_5", "PRESS"); command("preset_1", "bad");
  command("preset_1", "PRESS", true); command("save_preset_1", "PRESS", false, true);
  assert(saves == 4 && recalls == 4);

  command("move_duration_ms", "2500"); command("up", "PRESS"); assert(moves == 1 && duration == 2500);
  for (const char *invalid : {"0", "5001", "1.1", "nan", "inf", "2x"}) command("move_duration_ms", invalid);
  command("down", "PRESS"); assert(moves == 2 && duration == 2500);
  command("stop", "PRESS"); assert(moves == 3 && stopped && duration == 0);
  command("target_height", "80.1"); assert(targets == 1 && target == 801);
  for (const char *invalid : {"59.9", "125.1", "80.11", "nan", "inf", "80x"}) command("target_height", invalid);
  command("target_height", std::string("80\0junk", 7)); assert(targets == 1);
  command("min_height_cm", "65.1"); command("max_height_cm", "120");
  assert(min_height == 651 && max_height == 1200);
  publish_discovery(client);
  json = config("number", "target_height");
  assert(field(json, "min")->valuedouble == 65.1 && field(json, "max")->valuedouble == 120);
  cJSON_Delete(json);
  json = cJSON_Parse(published.at(gStateTopic).c_str());
  assert(field(json, "min_height_cm")->valuedouble == 65.1 && field(json, "move_duration_ms")->valueint == 2500);
  cJSON_Delete(json);
  command("min_height_cm", "120"); command("max_height_cm", "65.1");
  command("max_height_cm", "200.1"); command("min_height_cm", "60.01");
  assert(min_height == 651 && max_height == 1200);
  write_failure = true; command("min_height_cm", "70"); write_failure = false; assert(min_height == 651);
  read_failure = true; command("target_height", "80"); command("preset_1", "PRESS"); read_failure = false;
  assert(targets == 1 && recalls == 4);

  command("child_lock", "bad"); command("child_lock", "LOCK", true); assert(locks == 0 && !gChildLockBusy.load());
  task_failure = true; command("child_lock", "LOCK"); assert(!gChildLockBusy.load()); task_failure = false;
  command("child_lock", "LOCK"); assert(gChildLockBusy.load() && locks == 0);
  auto lock_task = pending_task;
  command("child_lock", "UNLOCK"); assert(pending_task == lock_task);
  command("stop", "PRESS"); assert(moves == 4);  // Lock waits never block MQTT Stop handling.
  lock_task(nullptr); assert(locks == 1 && locked && !gChildLockBusy.load() && (pending_bits & kRefresh));
  command("up", "PRESS"); command("target_height", "80"); command("preset_1", "PRESS"); command("save_preset_1", "PRESS");
  assert(moves == 4 && targets == 1 && recalls == 4 && saves == 4);
  command("stop", "PRESS"); assert(moves == 5 && stopped);
  lock_result = ESP_FAIL; command("child_lock", "UNLOCK"); pending_task(nullptr); assert(locked);
  lock_result = ESP_OK; command("child_lock", "UNLOCK"); pending_task(nullptr); assert(!locked);
  lock_known = false; publish_state(client);
  json = cJSON_Parse(published.at(gStateTopic).c_str()); assert(cJSON_IsNull(field(json, "locked"))); cJSON_Delete(json);

  command("restart", "PRESS", true); command("restart", "PRESS", false, true); command("restart", "bad");
  assert(restarts == 0);
  restart_ok = false; published[gAvailabilityTopic] = "online"; command("restart", "PRESS");
  assert(restarts == 1 && published[gAvailabilityTopic] == "online");
  restart_ok = true; command("restart", "PRESS"); assert(restarts == 2 && published[gAvailabilityTopic] == "offline");

  loctek = true; publish_discovery(client);
  assert(!config("button", "preset_1") && !config("lock", "child_lock") && !config("number", "target_height"));
  assert(!config("sensor", "preset_1_height"));
  command("preset_1", "PRESS"); command("child_lock", "LOCK"); command("target_height", "80");
  assert(recalls == 4 && locks == 3 && targets == 1);
  command("up", "PRESS"); assert(moves == 6);
  ready = false; publish_discovery(client); assert(!config("button", "up"));
  json = config("button", "restart"); assert(json); cJSON_Delete(json);
  loctek = false; ready = true; publish_discovery(client);
  json = config("button", "preset_1"); assert(json); cJSON_Delete(json);
  pending_bits = 0;
  esp_mqtt_event_t birth;
  birth.topic = kHaStatusTopic; birth.topic_len = static_cast<int>(std::strlen(kHaStatusTopic));
  birth.data = "online"; birth.data_len = birth.total_data_len = 6; birth.retain = true;
  handle_command(&birth); assert((pending_bits & kRefresh) != 0);
  std::puts("MQTT checks passed: discovery, presets, confirmed async lock, Stop, limits, duration, restart, malformed/retained messages and profile changes.");
}
