#pragma once

namespace pandadesk::wifi {
const char *hostname();
const char *dhcp_hostname();
const char *captive_portal_url();
bool connected();
bool mdns_available();
struct Status {
  bool connected = false;
  bool access_point = false;
  char ssid[33]{};
  int rssi = 0;
  char mac[18]{};
  char ip[16]{};
};
Status status();
void start(bool nvs_available);
}
