#include "pandadesk/wifi.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <cerrno>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "pandadesk/api.hpp"
#include "pandadesk/captive_dns.hpp"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

namespace pandadesk::wifi {
namespace {
constexpr char kTag[] = "PandaDeskWiFi";
constexpr EventBits_t kGotIp = BIT0;
constexpr EventBits_t kDisconnected = BIT1;
EventGroupHandle_t wifi_events;
std::atomic<bool> mdns_started{false};
std::atomic<bool> g_connected{false};
bool g_nvs_available = false;
constexpr char kMdnsHostname[] = "pandadesk";
char g_dhcp_hostname[sizeof("PandaDesk-FFFF")]{};
char g_mdns_fallback[sizeof("pandadesk-ffff")]{};
std::atomic<const char *> g_hostname{kMdnsHostname};
char g_portal_url[32]{};

struct Credentials {
  char ssid[33]{};
  char password[64]{};
};

void initialize_hostnames() {
  uint8_t mac[6];
  ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
  std::snprintf(g_dhcp_hostname, sizeof(g_dhcp_hostname), "PandaDesk-%02X%02X", mac[4], mac[5]);
  std::snprintf(g_mdns_fallback, sizeof(g_mdns_fallback), "pandadesk-%02x%02x", mac[4], mac[5]);
  g_hostname.store(kMdnsHostname);
}

void event_handler(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto *event = static_cast<ip_event_got_ip_t *>(data);
    ESP_LOGI(kTag, "Wi-Fi connected; got IP " IPSTR, IP2STR(&event->ip_info.ip));
    g_connected.store(true);
    xEventGroupSetBits(wifi_events, kGotIp);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    g_connected.store(false);
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
  mdns_result_t *results = nullptr;
  const esp_err_t query_result = mdns_query(kMdnsHostname, nullptr, nullptr, MDNS_TYPE_ANY, 1000, 1, &results);
  const bool occupied = results != nullptr;
  mdns_query_results_free(results);
  if (query_result != ESP_OK) {
    ESP_LOGW(kTag, "Could not check mDNS hostname: %s", esp_err_to_name(query_result));
    mdns_free();
    return;
  }
  const char *hostname = occupied ? g_mdns_fallback : kMdnsHostname;
  ESP_ERROR_CHECK(mdns_hostname_set(hostname));
  g_hostname.store(hostname);
  mdns_started = true;
  ESP_LOGI(kTag, "mDNS hostname: %s.local", hostname);
}

void check_mdns_conflict() {
  if (!mdns_started) return;
  char actual[MDNS_NAME_BUF_LEN]{};
  if (mdns_hostname_get(actual) != ESP_OK || std::strcmp(actual, g_hostname.load()) == 0) return;
  // The mDNS probe can detect a collision after the initial query (simultaneous boots).
  if (g_hostname.load() == kMdnsHostname) {
    ESP_ERROR_CHECK(mdns_hostname_set(g_mdns_fallback));
    g_hostname.store(g_mdns_fallback);
    ESP_LOGW(kTag, "mDNS name conflict; using %s.local", g_mdns_fallback);
  } else {
    ESP_LOGE(kTag, "mDNS fallback %s.local also in use; stopping mDNS", g_mdns_fallback);
    mdns_free();
    mdns_started = false;
  }
}

void dns_task(void *argument) {
  esp_netif_ip_info_t ip_info{};
  ESP_ERROR_CHECK(esp_netif_get_ip_info(static_cast<esp_netif_t *>(argument), &ip_info));
  const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_fd < 0) {
    ESP_LOGE(kTag, "Could not create captive DNS socket: %d", errno);
    vTaskDelete(nullptr);
    return;
  }
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(53);
  local.sin_addr.s_addr = ip_info.ip.addr;
  if (bind(socket_fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
    ESP_LOGE(kTag, "Could not bind captive DNS socket: %d", errno);
    close(socket_fd);
    vTaskDelete(nullptr);
    return;
  }
  ESP_LOGI(kTag, "Captive DNS listening on " IPSTR ":53", IP2STR(&ip_info.ip));
  uint8_t packet[512];
  while (true) {
    sockaddr_in client{};
    socklen_t client_length = sizeof(client);
    const int received = recvfrom(socket_fd, packet, sizeof(packet), 0,
                                  reinterpret_cast<sockaddr *>(&client), &client_length);
    if (received < 0) {
      if (errno == EINTR) continue;
      ESP_LOGE(kTag, "Captive DNS receive failed: %d", errno);
      break;
    }
    const size_t length = captive_dns::reply(packet, static_cast<size_t>(received), sizeof(packet),
                                            reinterpret_cast<const uint8_t *>(&ip_info.ip.addr));
    if (length != 0 && sendto(socket_fd, packet, length, 0,
                            reinterpret_cast<sockaddr *>(&client), client_length) < 0) {
      ESP_LOGW(kTag, "Captive DNS response failed: %d", errno);
    }
  }
  close(socket_fd);
  vTaskDelete(nullptr);
}

void start_access_point(esp_netif_t *ap_netif) {
  g_connected.store(false);
  esp_netif_ip_info_t ip_info{};
  ESP_ERROR_CHECK(esp_netif_get_ip_info(ap_netif, &ip_info));
  std::snprintf(g_portal_url, sizeof(g_portal_url), "http://" IPSTR "/", IP2STR(&ip_info.ip));
  const esp_err_t stop_result = esp_netif_dhcps_stop(ap_netif);
  if (stop_result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) ESP_ERROR_CHECK(stop_result);
  esp_netif_dns_info_t dns{};
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4.addr = ip_info.ip.addr;
  ESP_ERROR_CHECK(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns));
  uint8_t offer_dns = 1;
  ESP_ERROR_CHECK(esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                                        ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns)));
  ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

  wifi_config_t access_point = {};
  std::snprintf(reinterpret_cast<char *>(access_point.ap.ssid), sizeof(access_point.ap.ssid),
                "%s", g_dhcp_hostname);
  access_point.ap.ssid_len = std::strlen(reinterpret_cast<char *>(access_point.ap.ssid));
  access_point.ap.channel = 1;
  access_point.ap.max_connection = 4;
  access_point.ap.authmode = WIFI_AUTH_OPEN;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &access_point));
  pandadesk::api::start();
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGW(kTag, "Setup AP started: %s (open network)", access_point.ap.ssid);
  if (xTaskCreate(dns_task, "pandadesk_dns", 3072, ap_netif, 5, nullptr) != pdPASS) {
    ESP_LOGE(kTag, "Could not create captive DNS task");
  }
  ESP_LOGI(kTag, "Captive portal: %s", g_portal_url);
}

void wifi_task(void *) {
  const bool nvs_available = g_nvs_available;
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t *station_netif = esp_netif_create_default_wifi_sta();
  if (station_netif == nullptr) {
    ESP_LOGE(kTag, "Could not create station network interface");
    vTaskDelete(nullptr);
    return;
  }
  ESP_ERROR_CHECK(esp_netif_set_hostname(station_netif, g_dhcp_hostname));
  ESP_LOGI(kTag, "DHCP hostname: %s", g_dhcp_hostname);
  esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
  if (ap_netif == nullptr) {
    ESP_LOGE(kTag, "Could not create access point network interface");
    vTaskDelete(nullptr);
    return;
  }
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
    start_access_point(ap_netif);
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
      const EventBits_t bits = xEventGroupWaitBits(
          wifi_events, kDisconnected, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
      check_mdns_conflict();
      if (!(bits & kDisconnected)) continue;
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
  start_access_point(ap_netif);
  vTaskDelete(nullptr);
}
}  // namespace

void start(bool nvs_available) {
  g_nvs_available = nvs_available;
  initialize_hostnames();
  if (xTaskCreate(wifi_task, "pandadesk_wifi", 4096, nullptr, 5, nullptr) != pdPASS) {
    ESP_LOGE(kTag, "Could not create Wi-Fi task");
  }
}

const char *hostname() { return g_hostname.load(); }
const char *dhcp_hostname() { return g_dhcp_hostname; }
bool connected() { return g_connected.load(); }
bool mdns_available() { return mdns_started.load() && connected(); }

Status status() {
  Status result;
  wifi_mode_t mode;
  if (esp_wifi_get_mode(&mode) != ESP_OK) return result;
  result.access_point = mode == WIFI_MODE_AP;
  wifi_ap_record_t access_point{};
  result.connected = connected() && esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;
  if (result.connected) {
    std::memcpy(result.ssid, access_point.ssid, sizeof(access_point.ssid));
    result.rssi = access_point.rssi;
  } else if (result.access_point) {
    std::snprintf(result.ssid, sizeof(result.ssid), "%s", g_dhcp_hostname);
  }
  uint8_t mac[6]{};
  if (esp_read_mac(mac, result.access_point ? ESP_MAC_WIFI_SOFTAP : ESP_MAC_WIFI_STA) == ESP_OK) {
    std::snprintf(result.mac, sizeof(result.mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
  auto *netif = esp_netif_get_handle_from_ifkey(result.access_point ? "WIFI_AP_DEF" : "WIFI_STA_DEF");
  esp_netif_ip_info_t ip_info{};
  if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
    std::snprintf(result.ip, sizeof(result.ip), IPSTR, IP2STR(&ip_info.ip));
  }
  return result;
}

const char *captive_portal_url() {
  wifi_mode_t mode;
  return esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP ? g_portal_url : nullptr;
}
}  // namespace pandadesk::wifi
