#include "pandadesk/wifi.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "pandadesk/api.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

namespace pandadesk::wifi {
namespace {
constexpr char kTag[] = "PandaDeskWiFi";
constexpr EventBits_t kGotIp = BIT0;
constexpr EventBits_t kDisconnected = BIT1;
EventGroupHandle_t wifi_events;
bool mdns_started = false;
bool g_nvs_available = false;
char g_hostname[64] = "pandadesk";

struct Credentials {
  char ssid[33]{};
  char password[64]{};
};

bool valid_hostname(const char *hostname) {
  const size_t length = hostname == nullptr ? 0 : std::strlen(hostname);
  if (length == 0 || length >= sizeof(g_hostname) || hostname[0] == '-' || hostname[length - 1] == '-') return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(hostname[i]);
    if (!std::isalnum(c) && c != '-') return false;
  }
  return true;
}

void load_hostname(bool nvs_available) {
  if (!nvs_available) return;
  nvs_handle_t settings;
  if (nvs_open("device", NVS_READONLY, &settings) != ESP_OK) return;
  size_t size = sizeof(g_hostname);
  char hostname[sizeof(g_hostname)]{};
  const esp_err_t result = nvs_get_str(settings, "hostname", hostname, &size);
  nvs_close(settings);
  if (result == ESP_OK && valid_hostname(hostname)) {
    std::snprintf(g_hostname, sizeof(g_hostname), "%s", hostname);
  }
}

void event_handler(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto *event = static_cast<ip_event_got_ip_t *>(data);
    ESP_LOGI(kTag, "Wi-Fi connected; got IP " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(wifi_events, kGotIp);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupSetBits(wifi_events, kDisconnected);
  }
}

bool load_credentials(bool nvs_available, Credentials &credentials) {
  if (!nvs_available) return false;

  nvs_handle_t settings;
  if (nvs_open("wifi", NVS_READONLY, &settings) != ESP_OK) return false;
  size_t ssid_size = sizeof(credentials.ssid);
  size_t password_size = sizeof(credentials.password);
  const esp_err_t ssid_result = nvs_get_str(settings, "ssid", credentials.ssid, &ssid_size);
  const esp_err_t password_result = nvs_get_str(settings, "password", credentials.password, &password_size);
  nvs_close(settings);

  const bool password_valid = password_result == ESP_OK || password_result == ESP_ERR_NVS_NOT_FOUND;
  return ssid_result == ESP_OK && password_valid && ssid_size > 1 &&
         ssid_size <= sizeof(credentials.ssid) && password_size <= sizeof(credentials.password);
}

bool connect_with_timeout() {
  const TickType_t timeout = pdMS_TO_TICKS(CONFIG_PANDA_WIFI_CONNECT_TIMEOUT_MS);
  const TickType_t started = xTaskGetTickCount();
  while (xTaskGetTickCount() - started < timeout) {
    xEventGroupClearBits(wifi_events, kGotIp | kDisconnected);
    const esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK && result != ESP_ERR_WIFI_CONN) return false;

    const TickType_t elapsed = xTaskGetTickCount() - started;
    const TickType_t remaining = timeout > elapsed ? timeout - elapsed : 0;
    const EventBits_t bits = xEventGroupWaitBits(
        wifi_events, kGotIp | kDisconnected, pdTRUE, pdFALSE, remaining);
    if (bits & kGotIp) return true;
    if (!(bits & kDisconnected)) return false;
  }
  return false;
}

void start_mdns() {
  if (mdns_started) return;
  if (mdns_init() != ESP_OK) {
    ESP_LOGW(kTag, "mDNS initialization failed");
    return;
  }
  mdns_started = true;

  esp_ip4_addr_t address{};
  const esp_err_t query_result = mdns_query_a(g_hostname, 1000, &address);
  char hostname[sizeof(g_hostname)]{};
  std::snprintf(hostname, sizeof(hostname), "%s", g_hostname);
  if (query_result == ESP_OK) {
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    std::snprintf(hostname, sizeof(hostname), "%.*s-%02x%02x",
                  static_cast<int>(sizeof(hostname) - 6), g_hostname, mac[4], mac[5]);
  }
  ESP_ERROR_CHECK(mdns_hostname_set(hostname));
  std::snprintf(g_hostname, sizeof(g_hostname), "%s", hostname);
  ESP_LOGI(kTag, "mDNS hostname: %s.local", hostname);
}

void start_access_point() {
  uint8_t mac[6];
  ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
  wifi_config_t access_point = {};
  std::snprintf(reinterpret_cast<char *>(access_point.ap.ssid), sizeof(access_point.ap.ssid),
                "PandaDesk-%02X%02X", mac[4], mac[5]);
  access_point.ap.ssid_len = std::strlen(reinterpret_cast<char *>(access_point.ap.ssid));
  access_point.ap.channel = 1;
  access_point.ap.max_connection = 4;
  access_point.ap.authmode = WIFI_AUTH_OPEN;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &access_point));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGW(kTag, "Setup AP started: %s (open network)", access_point.ap.ssid);
  pandadesk::api::start();
}

void wifi_task(void *) {
  const bool nvs_available = g_nvs_available;
  load_hostname(nvs_available);
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t *station_netif = esp_netif_create_default_wifi_sta();
  if (station_netif == nullptr) {
    ESP_LOGE(kTag, "Could not create station network interface");
    vTaskDelete(nullptr);
    return;
  }
  ESP_ERROR_CHECK(esp_netif_set_hostname(station_netif, g_hostname));
  esp_netif_create_default_wifi_ap();
  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&config));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  wifi_events = xEventGroupCreate();
  if (wifi_events == nullptr) {
    ESP_LOGE(kTag, "Could not create Wi-Fi event group");
    vTaskDelete(nullptr);
    return;
  }
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, event_handler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, nullptr));

  Credentials credentials;
  if (!load_credentials(nvs_available, credentials)) {
    start_access_point();
    vTaskDelete(nullptr);
    return;
  }

  wifi_config_t station = {};
  const size_t ssid_length = std::strlen(credentials.ssid);
  std::memcpy(station.sta.ssid, credentials.ssid, ssid_length);
  std::memcpy(station.sta.password, credentials.password, std::strlen(credentials.password) + 1);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));

  const esp_err_t start_result = esp_wifi_start();
  bool wifi_started = start_result == ESP_OK;
  bool connected = wifi_started && connect_with_timeout();
  if (connected) {
    start_mdns();
    pandadesk::api::start();
    while (true) {
      xEventGroupWaitBits(wifi_events, kDisconnected, pdTRUE, pdFALSE, portMAX_DELAY);
      if (!connect_with_timeout()) break;
    }
  }

  ESP_LOGW(kTag, "Wi-Fi unavailable after %d ms; falling back to setup AP",
           CONFIG_PANDA_WIFI_CONNECT_TIMEOUT_MS);
  if (mdns_started) {
    mdns_free();
    mdns_started = false;
  }
  if (wifi_started) ESP_ERROR_CHECK(esp_wifi_stop());
  start_access_point();
  vTaskDelete(nullptr);
}
}  // namespace

void start(bool nvs_available) {
  g_nvs_available = nvs_available;
  if (xTaskCreate(wifi_task, "pandadesk_wifi", 4096, nullptr, 5, nullptr) != pdPASS) {
    ESP_LOGE(kTag, "Could not create Wi-Fi task");
  }
}

const char *hostname() { return g_hostname; }
}  // namespace pandadesk::wifi
