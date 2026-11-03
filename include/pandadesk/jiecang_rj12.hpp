#pragma once

#include <cstddef>
#include <cstdint>

namespace pandadesk::jiecang_rj12 {

inline constexpr char kGenericModel[] = "JIECANG_GENERIC";
inline constexpr char kNoTargetHeightModel[] = "JCHT35K72C";
const char *normalize_model(const char *model);

inline constexpr uint8_t kHandsetAddress = 0xF1;
inline constexpr uint8_t kControllerAddress = 0xF2;
inline constexpr uint8_t kHeightResponse = 0x01;
inline constexpr uint8_t kPhysicalLimitsResponse = 0x07;
inline constexpr uint8_t kRaiseCommand = 0x01;
inline constexpr uint8_t kLowerCommand = 0x02;
inline constexpr uint8_t kReadSettingsCommand = 0x07;
inline constexpr uint8_t kGotoHeightCommand = 0x1B;
inline constexpr uint8_t kStopCommand = 0x2B;
inline constexpr uint8_t kWakeCommand = 0x29;
inline constexpr uint8_t kChildLockCommand = 0x1F;
inline constexpr uint8_t kPresetCommands[] = {0x05, 0x06, 0x27, 0x28};
inline constexpr uint8_t kSavePresetCommands[] = {0x03, 0x04, 0x25, 0x26};
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

constexpr Frame make_raise_command() {
  return make_handset_command(kRaiseCommand, nullptr, 0);
}

constexpr Frame make_lower_command() {
  return make_handset_command(kLowerCommand, nullptr, 0);
}

constexpr Frame make_preset_command(uint8_t preset) {
  return preset >= 1 && preset <= 4 ? make_handset_command(kPresetCommands[preset - 1], nullptr, 0) : Frame{};
}

constexpr Frame make_save_preset_command(uint8_t preset) {
  return preset >= 1 && preset <= 4 ? make_handset_command(kSavePresetCommands[preset - 1], nullptr, 0) : Frame{};
}

constexpr Frame make_read_settings_command() {
  return make_handset_command(kReadSettingsCommand, nullptr, 0);
}

constexpr Frame make_child_lock_query() {
  const uint8_t read_only[] = {0};
  return make_handset_command(kChildLockCommand, read_only, 1);
}

constexpr Frame make_child_lock_toggle() {
  const uint8_t toggle[] = {1};
  return make_handset_command(kChildLockCommand, toggle, 1);
}

constexpr Frame make_target_height_command_mm(uint16_t height_mm) {
  const uint8_t parameters[] = {
      static_cast<uint8_t>(height_mm >> 8), static_cast<uint8_t>(height_mm & 0xFF)};
  return make_handset_command(kGotoHeightCommand, parameters, 2);
}

bool decode_frame(const uint8_t *bytes, size_t size, Frame *frame);
bool decode_height_raw(const Frame &frame, uint16_t *raw_height);
bool decode_child_lock(const Frame &frame, bool *locked);
bool decode_preset_raw(const Frame &frame, uint8_t *preset, uint16_t *raw_height);
bool decode_physical_limits_raw(const Frame &frame, uint16_t *maximum, uint16_t *minimum);

constexpr uint16_t raw_height_to_tenths_cm(uint16_t raw) {
  if (raw >= 650 && raw <= 1290) return raw;
  if (raw >= 240 && raw <= 530) {
    return static_cast<uint16_t>((static_cast<uint32_t>(raw) * 254 + 50) / 100);
  }
  return 0;
}

}  // namespace pandadesk::jiecang_rj12
