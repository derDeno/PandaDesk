"""Exercise the firmware's reconnect loop with scripted Wi-Fi events (requires clang++)."""

from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src" / "wifi.cpp").read_text(encoding="utf-8")
constants = source[source.index("constexpr EventBits_t kGotIp"):source.index("EventGroupHandle_t wifi_events")]
connect = source[source.index("EventBits_t connect_once("):source.index("void start_mdns(")]
station = source[source.index("void recover_from_access_point("):source.index("void wifi_task(")]

harness = r"""
#define _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
#include <atomic>
#include <cassert>
#include <cstdio>
#include <iterator>
using EventBits_t = unsigned;
using TickType_t = unsigned;
using esp_err_t = int;
constexpr unsigned BIT0 = 1, BIT1 = 2, BIT2 = 4, BIT3 = 8;
constexpr int ESP_OK = 0, ESP_ERR_WIFI_CONN = 1;
constexpr bool pdTRUE = true, pdFALSE = false;
constexpr TickType_t portMAX_DELAY = -1u;
constexpr unsigned CONFIG_PANDA_WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr TickType_t pdMS_TO_TICKS(unsigned ms) { return ms; }
#define ESP_ERROR_CHECK(expression) assert((expression) == ESP_OK)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
"""

mocks = r"""
enum wifi_mode_t { WIFI_MODE_STA, WIFI_MODE_APSTA };
struct esp_netif_t {};
struct Done {};
std::atomic<bool> g_connected{false};
bool mdns_started = false;
EventBits_t wifi_events = 0;
TickType_t ticks = 0;
wifi_mode_t mode = WIFI_MODE_STA;
unsigned starts = 0, attempts = 0, fallback_count = 0, dns_stops = 0;

struct Wait {
  wifi_mode_t mode;
  EventBits_t mask;
  TickType_t timeout;
  EventBits_t event;
};
constexpr auto kConnectEvents = kGotIp | kDisconnected;
constexpr Wait waits[] = {
  {WIFI_MODE_STA, kConnectEvents, kConnectTimeout, kGotIp},
  {WIFI_MODE_STA, kDisconnected, 1000, kDisconnected},
  {WIFI_MODE_STA, kConnectEvents, kConnectTimeout, kGotIp},
  {WIFI_MODE_STA, kDisconnected, 1000, kDisconnected},
  {WIFI_MODE_STA, kConnectEvents, kConnectTimeout, 0},
  {WIFI_MODE_APSTA, kConnectEvents, kConnectTimeout, kDisconnected},
  {WIFI_MODE_APSTA, kGotIp, kSetupRetryDelay, 0},
  {WIFI_MODE_APSTA, kConnectEvents, kConnectTimeout, 0},
  {WIFI_MODE_APSTA, kGotIp, kSetupRetryDelay, kGotIp}, // DHCP completed during the pause.
  {WIFI_MODE_APSTA, kDnsStopped, portMAX_DELAY, kDnsStopped},
  {WIFI_MODE_STA, kDisconnected, 1000, kDisconnected},
  {WIFI_MODE_STA, kConnectEvents, kConnectTimeout, 0},
  {WIFI_MODE_APSTA, kConnectEvents, kConnectTimeout, kGotIp},
  {WIFI_MODE_APSTA, kDnsStopped, portMAX_DELAY, kDnsStopped},
};

void xEventGroupClearBits(EventBits_t &bits, EventBits_t mask) { bits &= ~mask; }
void xEventGroupSetBits(EventBits_t &bits, EventBits_t mask) { bits |= mask; }
EventBits_t xEventGroupWaitBits(EventBits_t &bits, EventBits_t mask, bool clear,
                               bool, TickType_t timeout) {
  static unsigned next = 0;
  if (next == std::size(waits)) throw Done{};
  const auto &wait = waits[next++];
  assert(mode == wait.mode && mask == wait.mask && timeout == wait.timeout);
  assert(!(bits & mask));
  if (wait.event == kDisconnected) g_connected = false;
  if (wait.event == kGotIp) g_connected = true;
  if (wait.event == kDnsStopped) {
    assert(bits & kStopDns);
    ++dns_stops;
  }
  ticks += wait.event ? 1 : timeout;
  bits |= wait.event;
  const auto result = bits;
  if (clear) bits &= ~mask;
  return result;
}

TickType_t xTaskGetTickCount() { return ticks; }
int esp_wifi_start() { ++starts; return ESP_OK; }
int esp_wifi_connect() { assert(!g_connected); ++attempts; return ESP_OK; }
int esp_wifi_set_mode(wifi_mode_t next) {
  assert(mode == WIFI_MODE_APSTA && next == WIFI_MODE_STA && g_connected);
  assert(fallback_count == dns_stops);
  mode = next;
  return ESP_OK;
}
void start_access_point(esp_netif_t *, wifi_mode_t next, bool wifi_started) {
  assert(wifi_started && mode == WIFI_MODE_STA && next == WIFI_MODE_APSTA);
  ++fallback_count;
  xEventGroupClearBits(wifi_events, kStopDns | kDnsStopped);
  mode = next;
}
void start_mdns() { assert(mode == WIFI_MODE_STA); mdns_started = true; }
void mdns_free() { assert(mdns_started); }
void check_mdns_conflict() {}
namespace pandadesk::api { void start() {} }
"""

checks = r"""
int main() {
  try {
    run_station(nullptr);
  } catch (const Done &) {
    assert(mode == WIFI_MODE_STA && g_connected && mdns_started);
    assert(starts == 1 && attempts == 7 && fallback_count == 2 && dns_stops == 2);
    puts("Wi-Fi checks passed: short reconnect, repeated AP fallback, and late DHCP recovery.");
    return 0;
  }
  return 1;
}
"""

with tempfile.TemporaryDirectory(prefix="pandadesk-wifi-") as directory:
    test = Path(directory) / "test.cpp"
    test.write_text(harness + constants + mocks + connect + station + checks, encoding="utf-8")
    executable = test.with_suffix(".exe")
    subprocess.run(
        ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)
