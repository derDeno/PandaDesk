#include "pandadesk/jiecang_rj12.hpp"

namespace pandadesk::jiecang_rj12 {
namespace {
constexpr uint8_t checksum(const uint8_t *bytes, size_t parameter_count) {
  uint8_t sum = static_cast<uint8_t>(bytes[2] + bytes[3]);
  for (size_t i = 0; i < parameter_count; ++i) sum = static_cast<uint8_t>(sum + bytes[4 + i]);
  return sum;
}

constexpr uint16_t read_u16_be(const uint8_t *bytes) {
  return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
}
}  // namespace

bool decode_frame(const uint8_t *bytes, size_t size, Frame *frame) {
  if (bytes == nullptr || frame == nullptr || size < 6 || size > kMaxFrameSize ||
      (bytes[0] != kHandsetAddress && bytes[0] != kControllerAddress) || bytes[1] != bytes[0]) {
    return false;
  }
  const size_t parameter_count = bytes[3];
  if (parameter_count > kMaxParameters || size != parameter_count + 6 ||
      bytes[size - 1] != 0x7E || bytes[size - 2] != checksum(bytes, parameter_count)) {
    return false;
  }
  Frame decoded{};
  decoded.size = size;
  for (size_t i = 0; i < size; ++i) decoded.bytes[i] = bytes[i];
  *frame = decoded;
  return true;
}

bool decode_height_raw(const Frame &frame, uint16_t *raw_height) {
  if (raw_height == nullptr || frame.size != 9 || frame.bytes[0] != kControllerAddress ||
      frame.bytes[1] != kControllerAddress || frame.bytes[2] != kHeightResponse || frame.bytes[3] != 3) {
    return false;
  }
  *raw_height = read_u16_be(&frame.bytes[4]);
  return true;
}

bool decode_physical_limits_raw(const Frame &frame, uint16_t *maximum, uint16_t *minimum) {
  if (maximum == nullptr || minimum == nullptr || frame.size != 10 ||
      frame.bytes[0] != kControllerAddress || frame.bytes[1] != kControllerAddress ||
      frame.bytes[2] != kPhysicalLimitsResponse || frame.bytes[3] != 4) {
    return false;
  }
  *maximum = read_u16_be(&frame.bytes[4]);
  *minimum = read_u16_be(&frame.bytes[6]);
  return *minimum <= *maximum;
}

namespace {
constexpr Frame kWakeSelfCheck = make_handset_command(kWakeCommand, nullptr, 0);
static_assert(kWakeSelfCheck.size == 6 && kWakeSelfCheck.bytes[0] == 0xF1 &&
              kWakeSelfCheck.bytes[2] == 0x29 && kWakeSelfCheck.bytes[4] == 0x29 &&
              kWakeSelfCheck.bytes[5] == 0x7E);
constexpr Frame kGotoSelfCheck = make_target_height_command_mm(720);
static_assert(kGotoSelfCheck.size == 8 && kGotoSelfCheck.bytes[2] == 0x1B &&
              kGotoSelfCheck.bytes[3] == 2 && kGotoSelfCheck.bytes[4] == 0x02 &&
              kGotoSelfCheck.bytes[5] == 0xD0 && kGotoSelfCheck.bytes[6] == 0xEF);
static_assert(raw_height_to_tenths_cm(1286) == 1286);
static_assert(raw_height_to_tenths_cm(254) == 645);
static_assert(raw_height_to_tenths_cm(600) == 0);
}  // namespace

}  // namespace pandadesk::jiecang_rj12
