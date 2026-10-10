#pragma once

#include <cstddef>
#include <cstdint>

namespace pandadesk::jiecang_rj12 {

inline constexpr uint8_t kHandsetAddress = 0xF1;
inline constexpr uint8_t kControllerAddress = 0xF2;
inline constexpr uint8_t kHeightResponse = 0x01;
inline constexpr uint8_t kPhysicalLimitsResponse = 0x07;
inline constexpr uint8_t kGotoHeightCommand = 0x1B;
inline constexpr uint8_t kStopCommand = 0x2B;
inline constexpr uint8_t kWakeCommand = 0x29;
inline constexpr size_t kMaxParameters = 4;
inline constexpr size_t kMaxFrameSize = kMaxParameters + 6;

struct Frame {
  uint8_t bytes[kMaxFrameSize]{};
  size_t size = 0;
};

constexpr Frame make_handset_command(uint8_t command, const uint8_t *parameters, size_t count) {
  Frame frame{};
  if (count > kMaxParameters || (count != 0 && parameters == nullptr)) return frame;
  frame.bytes[0] = kHandsetAddress;
  frame.bytes[1] = kHandsetAddress;
  frame.bytes[2] = command;
  frame.bytes[3] = static_cast<uint8_t>(count);
  uint8_t checksum = static_cast<uint8_t>(command + count);
  for (size_t i = 0; i < count; ++i) {
    frame.bytes[4 + i] = parameters[i];
    checksum = static_cast<uint8_t>(checksum + parameters[i]);
  }
  frame.bytes[4 + count] = checksum;
  frame.bytes[5 + count] = 0x7E;
  frame.size = 6 + count;
  return frame;
}

constexpr Frame make_stop_command() {
  return make_handset_command(kStopCommand, nullptr, 0);
}

constexpr Frame make_target_height_command_mm(uint16_t height_mm) {
  const uint8_t parameters[] = {
      static_cast<uint8_t>(height_mm >> 8), static_cast<uint8_t>(height_mm & 0xFF)};
  return make_handset_command(kGotoHeightCommand, parameters, 2);
}

bool decode_frame(const uint8_t *bytes, size_t size, Frame *frame);
bool decode_height_raw(const Frame &frame, uint16_t *raw_height);
bool decode_physical_limits_raw(const Frame &frame, uint16_t *maximum, uint16_t *minimum);

}  // namespace pandadesk::jiecang_rj12
