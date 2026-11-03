#include <cassert>
#include "pandadesk/jiecang_rj12.hpp"

using namespace pandadesk::jiecang_rj12;

int main() {
  const uint8_t recall_commands[] = {0x05, 0x06, 0x27, 0x28};
  const uint8_t save_commands[] = {0x03, 0x04, 0x25, 0x26};
  for (uint8_t slot = 1; slot <= 4; ++slot) {
    Frame decoded{};
    auto recall = make_preset_command(slot);
    auto save = make_save_preset_command(slot);
    assert(recall.size == 6 && recall.bytes[2] == recall_commands[slot - 1]);
    assert(save.size == 6 && save.bytes[2] == save_commands[slot - 1]);
    assert(decode_frame(recall.bytes, recall.size, &decoded));
    assert(decode_frame(save.bytes, save.size, &decoded));
    assert(recall.bytes[4] == recall.bytes[2] && recall.bytes[5] == 0x7E);
    assert(save.bytes[4] == save.bytes[2] && save.bytes[5] == 0x7E);

    uint8_t preset = 0;
    uint16_t height = 0;
    const uint8_t parameters[] = {0x04, 0x7E}; // Embedded terminator is part of the height.
    auto report = make_handset_command(0x24 + slot, parameters, 2);
    assert(!decode_preset_raw(report, &preset, &height));
    report.bytes[0] = report.bytes[1] = kControllerAddress;
    assert(decode_frame(report.bytes, report.size, &decoded));
    assert(decode_preset_raw(decoded, &preset, &height));
    assert(preset == slot && height == 1150);
    assert(!decode_preset_raw(decoded, nullptr, &height));
    assert(!decode_preset_raw(decoded, &preset, nullptr));
    --report.size;
    assert(!decode_preset_raw(report, &preset, &height));
  }
  const uint8_t invalid_slots[] = {0, 5, 255};
  for (uint8_t slot : invalid_slots) {
    assert(make_preset_command(slot).size == 0);
    assert(make_save_preset_command(slot).size == 0);
  }
  uint8_t preset = 0;
  uint16_t height = 42;
  const uint8_t unset[] = {0, 0};
  auto report = make_handset_command(0x25, unset, 2);
  report.bytes[0] = report.bytes[1] = kControllerAddress;
  assert(decode_preset_raw(report, &preset, &height) && height == 0);
  report.bytes[2] = 0x24;
  assert(!decode_preset_raw(report, &preset, &height));
  report.bytes[2] = 0x29;
  assert(!decode_preset_raw(report, &preset, &height));
  // Four real memory reports in the existing Maidesite standby capture.
  const uint8_t captured[][8] = {
      {0xF2, 0xF2, 0x25, 2, 4, 0x12, 0x3D, 0x7E},
      {0xF2, 0xF2, 0x26, 2, 2, 0xFB, 0x25, 0x7E},
      {0xF2, 0xF2, 0x27, 2, 2, 0xFC, 0x27, 0x7E},
      {0xF2, 0xF2, 0x28, 2, 4, 0xB8, 0xE6, 0x7E}};
  const uint16_t expected[] = {1042, 763, 764, 1208};
  for (uint8_t index = 0; index < 4; ++index) {
    Frame decoded{};
    assert(decode_frame(captured[index], sizeof(captured[index]), &decoded));
    assert(decode_preset_raw(decoded, &preset, &height));
    assert(preset == index + 1 && raw_height_to_tenths_cm(height) == expected[index]);
  }
}
