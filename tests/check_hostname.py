"""Host checks of actual DHCP/mDNS logic with mocked MAC/mDNS (requires clang++)."""

from pathlib import Path
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
source = (root / "src" / "wifi.cpp").read_text(encoding="utf-8")
names = source[source.index("constexpr char kMdnsHostname"):source.index("char g_portal_url")]
initialize = source[source.index("void initialize_hostnames("):source.index("void event_handler(")]
mdns = source[source.index("void start_mdns("):source.index("void dns_task(")]
harness = r"""
#define _CRT_SECURE_NO_WARNINGS
#define _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cassert>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=1, ESP_MAC_WIFI_STA=0, MDNS_TYPE_ANY=255, MDNS_NAME_BUF_LEN=69;
#define ESP_ERROR_CHECK(expression) assert((expression) == ESP_OK)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
bool mdns_started=false, occupied=false;
int init_error=0, query_error=0, get_error=0, init_calls=0, set_calls=0, free_calls=0, results_freed=0;
uint8_t mac[] = {0, 1, 2, 3, 0xa1, 0xb2};
char registered[MDNS_NAME_BUF_LEN]{};
struct mdns_result_t {} result;
int esp_read_mac(uint8_t *out, int) { memcpy(out,mac,6); return 0; }
const char *esp_err_to_name(int) { return "mock error"; }
int mdns_init() { ++init_calls; return init_error; }
int mdns_query(const char *name, const char *service, const char *proto, int type,
               int timeout, int max_results, mdns_result_t **out) {
  assert(!strcmp(name,"pandadesk") && !service && !proto && type==MDNS_TYPE_ANY);
  assert(timeout>0 && max_results==1);
  *out=occupied?&result:nullptr; return query_error;
}
void mdns_query_results_free(mdns_result_t *value) { if(value) ++results_freed; }
void mdns_free() { ++free_calls; }
int mdns_hostname_set(const char *value) { ++set_calls; strcpy(registered,value); return 0; }
int mdns_hostname_get(char *out) { if(get_error) return get_error; strcpy(out,registered); return 0; }
"""
checks = r"""
int main() {
  initialize_hostnames();
  assert(!strcmp(g_dhcp_hostname,"PandaDesk-A1B2"));
  assert(!strcmp(g_mdns_fallback,"pandadesk-a1b2"));
  assert(!strcmp(g_hostname.load(),"pandadesk"));
  start_mdns(); // Base name is free.
  assert(mdns_started && !strcmp(registered,"pandadesk"));
  assert(!strcmp(g_hostname.load(),"pandadesk"));
  start_mdns();check_mdns_conflict(); // Already running; no re-registration.
  assert(init_calls==1 && set_calls==1);

  strcpy(registered,"pandadesk-2"); // Late probe collision after simultaneous boots.
  check_mdns_conflict();
  assert(!strcmp(registered,"pandadesk-a1b2") && !strcmp(g_hostname.load(),registered));
  assert(!strcmp(g_dhcp_hostname,"PandaDesk-A1B2"));
  check_mdns_conflict();assert(set_calls==2);
  strcpy(registered,"pandadesk-a1b2-2"); // Even the fallback is occupied.
  check_mdns_conflict();assert(!mdns_started && free_calls==1);
  assert(!strcmp(g_dhcp_hostname,"PandaDesk-A1B2"));

  initialize_hostnames();occupied=true;start_mdns(); // Base already registered by another host.
  assert(mdns_started && !strcmp(registered,"pandadesk-a1b2"));
  assert(!strcmp(g_hostname.load(),registered) && results_freed==1);
  get_error=ESP_FAIL;check_mdns_conflict();assert(mdns_started);get_error=0;
  assert(!strcmp(g_dhcp_hostname,"PandaDesk-A1B2"));

  mdns_started=false;initialize_hostnames();occupied=false;start_mdns(); // Retry base on reboot.
  assert(!strcmp(registered,"pandadesk") && !strcmp(g_hostname.load(),"pandadesk"));
  mdns_started=false;query_error=ESP_FAIL;int before=set_calls;start_mdns();
  assert(!mdns_started && set_calls==before); // Errors are not evidence of a free name.
  query_error=0;init_error=ESP_FAIL;start_mdns();assert(!mdns_started && set_calls==before);

  mac[4]=0;mac[5]=0x0f;initialize_hostnames();
  assert(!strcmp(g_dhcp_hostname,"PandaDesk-000F") && !strcmp(g_mdns_fallback,"pandadesk-000f"));
  mac[4]=mac[5]=0xff;initialize_hostnames();
  assert(!strcmp(g_dhcp_hostname,"PandaDesk-FFFF") && !strcmp(g_mdns_fallback,"pandadesk-ffff"));
  puts("Hostname checks passed: fixed DHCP suffix, free/occupied mDNS, late conflicts and errors.");
}
"""
with tempfile.TemporaryDirectory(prefix="pandadesk-hostname-") as directory:
    path = Path(directory)
    test = path / "test.cpp"
    test.write_text(harness + names + initialize + mdns + checks, encoding="utf-8")
    executable = path / "test.exe"
    subprocess.run(
        ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)

assert 'esp_netif_set_hostname(station_netif, g_dhcp_hostname)' in source
assert '"%s", g_dhcp_hostname' in source  # AP uses the same fixed name.
