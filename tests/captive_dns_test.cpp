// Host check: clang++ -std=c++17 -Iinclude tests/captive_dns_test.cpp -o captive_dns_test.exe
#include "pandadesk/captive_dns.hpp"

#include <assert.h>
#include <stdio.h>

constexpr uint8_t kAddress[] = {192, 168, 4, 1};

size_t query(uint8_t *packet, const char *name, uint8_t type = 1) {
  memset(packet, 0, 512);
  packet[0] = 0x12;
  packet[1] = 0x34;
  packet[2] = 1;
  packet[5] = 1;
  size_t cursor = 12;
  while (*name) {
    const char *dot = strchr(name, '.');
    const size_t length = dot ? static_cast<size_t>(dot - name) : strlen(name);
    packet[cursor++] = static_cast<uint8_t>(length);
    memcpy(packet + cursor, name, length);
    cursor += length;
    name += length + (dot != nullptr);
  }
  packet[cursor++] = 0;
  packet[cursor++] = 0;
  packet[cursor++] = type;
  packet[cursor++] = 0;
  packet[cursor++] = 1;
  return cursor;
}

int main() {
  uint8_t packet[512];
  const char *hosts[] = {"connectivitycheck.gstatic.com", "captive.apple.com",
                         "www.msftconnecttest.com", "detectportal.firefox.com"};
  for (const char *host : hosts) {
    const size_t length = query(packet, host);
    packet[3] = 0x30;  // AD/CD flags in real clients must not reject a standard query.
    assert(pandadesk::captive_dns::reply(packet, length, sizeof(packet), kAddress) == length + 16);
    assert(packet[0] == 0x12 && packet[1] == 0x34);
    assert(packet[2] == 0x85 && packet[3] == 0 && packet[7] == 1);
    const uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4, 192, 168, 4, 1};
    assert(memcmp(packet + length, answer, sizeof(answer)) == 0);
  }

  size_t length = query(packet, hosts[0], 28);  // AAAA: valid empty answer, no fake IPv6.
  assert(pandadesk::captive_dns::reply(packet, length, sizeof(packet), kAddress) == length);
  assert(packet[7] == 0);

  length = query(packet, hosts[0]);
  packet[11] = 1;  // EDNS OPT additional record is not copied into the response.
  const uint8_t opt[] = {0, 0, 41, 4, 208, 0, 0, 0, 0, 0, 0};
  memcpy(packet + length, opt, sizeof(opt));
  assert(pandadesk::captive_dns::reply(packet, length + sizeof(opt), sizeof(packet), kAddress) == length + 16);
  assert(packet[10] == 0 && packet[11] == 0 && packet[length] == 0xc0);

  length = query(packet, hosts[0]);
  for (size_t truncated = 0; truncated < length; ++truncated) {
    query(packet, hosts[0]);
    assert(pandadesk::captive_dns::reply(packet, truncated, sizeof(packet), kAddress) == 0);
  }
  assert(pandadesk::captive_dns::reply(packet, length, length + 15, kAddress) == 0);
  assert(pandadesk::captive_dns::reply(packet, length, length - 1, kAddress) == 0);
  const uint8_t invalid_flags[] = {0x80, 0x08, 0x10, 0x78};
  for (uint8_t flags : invalid_flags) {
    length = query(packet, hosts[0]);
    packet[2] = flags;
    assert(pandadesk::captive_dns::reply(packet, length, sizeof(packet), kAddress) == 0);
  }
  length = query(packet, hosts[0]);
  packet[5] = 2;
  assert(pandadesk::captive_dns::reply(packet, length, sizeof(packet), kAddress) == 0);
  packet[5] = 1;
  packet[12] = 0xc0;  // Invalid/compressed query names must not loop or read out of bounds.
  assert(pandadesk::captive_dns::reply(packet, length, sizeof(packet), kAddress) == 0);
  packet[12] = 63;
  assert(pandadesk::captive_dns::reply(packet, length, sizeof(packet), kAddress) == 0);

  // Malformed packet smoke test with guards around the writable buffer.
  uint8_t guarded[514];
  uint32_t random = 42;
  for (unsigned trial = 0; trial < 10000; ++trial) {
    for (uint8_t &byte : guarded) {
      random = random * 1664525U + 1013904223U;
      byte = static_cast<uint8_t>(random >> 24);
    }
    guarded[0] = guarded[513] = 0xa5;
    if (trial % 2 == 0) {
      guarded[3] = 1;
      guarded[5] = 0;
      guarded[6] = 1;
    }
    assert(pandadesk::captive_dns::reply(guarded + 1, trial % 513, 512, kAddress) <= 512);
    assert(guarded[0] == 0xa5 && guarded[513] == 0xa5);
  }
  puts("Captive DNS: OS probes, AAAA, EDNS, malformed packets and bounds checks passed.");
}
