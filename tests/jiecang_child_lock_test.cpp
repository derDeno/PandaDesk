// Host check: clang++ -std=c++17 -Iinclude tests/jiecang_child_lock_test.cpp src/jiecang_rj12.cpp -o child_lock_test.exe
#include <cassert>
#include <cstring>
#include "pandadesk/jiecang_rj12.hpp"

int main() {
  using namespace pandadesk::jiecang_rj12;
  // Captured from the Maidesite MDS36DR with the handset unlocked, then locked.
  const uint8_t unlocked[] = {0xF2, 0xF2, 0x1F, 0x01, 0x00, 0x20, 0x7E};
  const uint8_t locked[] = {0xF2, 0xF2, 0x1F, 0x01, 0x01, 0x21, 0x7E};
  Frame frame{};
  bool value = true;
  assert(decode_frame(unlocked, sizeof(unlocked), &frame));
  assert(decode_child_lock(frame, &value) && !value);
  assert(decode_frame(locked, sizeof(locked), &frame));
  assert(decode_child_lock(frame, &value) && value);
  assert(!decode_child_lock(frame, nullptr));

  const auto query = make_child_lock_query();
  const uint8_t expected[] = {0xF1, 0xF1, 0x1F, 0x01, 0x00, 0x20, 0x7E};
  assert(query.size == sizeof(expected));
  assert(std::memcmp(query.bytes, expected, sizeof(expected)) == 0);
  assert(!decode_child_lock(query, &value)); // A request is not a confirmed state.
  const auto toggle = make_child_lock_toggle();
  const uint8_t expected_toggle[] = {0xF1, 0xF1, 0x1F, 0x01, 0x01, 0x21, 0x7E};
  assert(toggle.size == sizeof(expected_toggle));
  assert(std::memcmp(toggle.bytes, expected_toggle, sizeof(expected_toggle)) == 0);
  assert(!decode_child_lock(toggle, &value));

  const uint8_t unknown_value[] = {0xF2, 0xF2, 0x1F, 0x01, 0x02, 0x22, 0x7E};
  assert(decode_frame(unknown_value, sizeof(unknown_value), &frame));
  assert(!decode_child_lock(frame, &value));
  auto corrupt = query;
  corrupt.bytes[0] = corrupt.bytes[1] = 0xF2;
  corrupt.bytes[5] ^= 1;
  assert(!decode_frame(corrupt.bytes, corrupt.size, &frame));
  assert(!decode_frame(locked, sizeof(locked) - 1, &frame));
  frame = make_read_settings_command();
  assert(!decode_child_lock(frame, &value));
  frame = make_target_height_command_mm(769);
  frame.bytes[0] = frame.bytes[1] = 0xF2;
  frame.bytes[2] = kChildLockCommand;
  assert(!decode_child_lock(frame, &value));
}
