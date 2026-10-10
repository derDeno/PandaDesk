#pragma once

#include "pandadesk/jiecang_rj12.hpp"

namespace pandadesk::jiecang_jarvis_rj45 {

using Frame = jiecang_rj12::Frame;

inline constexpr uint8_t kRaise = 0x01;
inline constexpr uint8_t kLower = 0x02;
inline constexpr uint8_t kStop = 0x2B;
inline constexpr uint8_t kGotoHeight = 0x1B;
inline constexpr uint8_t kSavePosition1 = 0x03;
inline constexpr uint8_t kSavePosition2 = 0x04;
inline constexpr uint8_t kMovePosition1 = 0x05;
inline constexpr uint8_t kMovePosition2 = 0x06;
inline constexpr uint8_t kReadSettings = 0x07;
inline constexpr uint8_t kSetUnits = 0x0E;
inline constexpr uint8_t kSetMemoryMode = 0x19;
inline constexpr uint8_t kSetCollisionSensitivity = 0x1D;
inline constexpr uint8_t kSetMaximum = 0x21;
inline constexpr uint8_t kSetMinimum = 0x22;
inline constexpr uint8_t kClearLimits = 0x23;
inline constexpr uint8_t kSavePosition3 = 0x25;
inline constexpr uint8_t kSavePosition4 = 0x26;
inline constexpr uint8_t kMovePosition3 = 0x27;
inline constexpr uint8_t kMovePosition4 = 0x28;
inline constexpr uint8_t kWake = 0x29;

constexpr Frame make_command(uint8_t command, const uint8_t *parameters = nullptr, size_t count = 0) {
  return jiecang_rj12::make_handset_command(command, parameters, count);
}

constexpr Frame make_raise() { return make_command(kRaise); }
constexpr Frame make_lower() { return make_command(kLower); }
constexpr Frame make_stop() { return make_command(kStop); }
constexpr Frame make_wake() { return make_command(kWake); }
constexpr Frame make_read_settings() { return make_command(kReadSettings); }
constexpr Frame make_target_height(uint16_t height_tenths_cm) {
  const uint8_t parameters[] = {static_cast<uint8_t>(height_tenths_cm >> 8),
                                static_cast<uint8_t>(height_tenths_cm & 0xFF)};
  return make_command(kGotoHeight, parameters, 2);
}
constexpr Frame make_move_to_position(uint8_t position) {
  switch (position) {
    case 1: return make_command(kMovePosition1);
    case 2: return make_command(kMovePosition2);
    case 3: return make_command(kMovePosition3);
    case 4: return make_command(kMovePosition4);
    default: return {};
  }
}

inline bool decode_frame(const uint8_t *bytes, size_t size, Frame *frame) {
  return jiecang_rj12::decode_frame(bytes, size, frame);
}

inline bool decode_height_raw(const Frame &frame, uint16_t *raw_height) {
  return jiecang_rj12::decode_height_raw(frame, raw_height);
}

}  // namespace pandadesk::jiecang_jarvis_rj45
