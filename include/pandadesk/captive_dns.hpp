#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pandadesk::captive_dns {
// Reply in place to a single DNS question; unsupported record types get no answers.
inline size_t reply(uint8_t *packet, size_t length, size_t capacity, const uint8_t address[4]) {
  if (length < 12 || length > capacity || (packet[2] & 0xf8) != 0 ||
      packet[4] != 0 || packet[5] != 1) return 0;

  size_t cursor = 12;
  while (true) {
    if (cursor >= length) return 0;
    const size_t label_length = packet[cursor++];
    if (label_length == 0) break;
    if (label_length > 63 || label_length > length - cursor) return 0;
    cursor += label_length;
  }
  if (cursor - 12 > 255 || length - cursor < 4) return 0;
  const bool answer = packet[cursor] == 0 && packet[cursor + 1] == 1 &&
                      packet[cursor + 2] == 0 && packet[cursor + 3] == 1;
  cursor += 4;
  if (answer && capacity - cursor < 16) return 0;

  packet[2] = 0x84 | (packet[2] & 1);  // Authoritative response, preserve RD.
  packet[3] = 0;
  memset(packet + 6, 0, 6);  // Drop answers and additional records (including EDNS).
  if (answer) {
    packet[7] = 1;
    const uint8_t record[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4};
    memcpy(packet + cursor, record, sizeof(record));  // TTL 0 avoids stale portal DNS.
    memcpy(packet + cursor + sizeof(record), address, 4);
    cursor += 16;
  }
  return cursor;
}
}  // namespace pandadesk::captive_dns
