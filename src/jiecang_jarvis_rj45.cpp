#include "pandadesk/jiecang_jarvis_rj45.hpp"

namespace pandadesk::jiecang_jarvis_rj45 {
namespace {
constexpr Frame kRaiseSelfCheck = make_raise();
static_assert(kRaiseSelfCheck.size == 6 && kRaiseSelfCheck.bytes[0] == 0xF1 &&
              kRaiseSelfCheck.bytes[2] == kRaise && kRaiseSelfCheck.bytes[4] == 0x01 &&
              kRaiseSelfCheck.bytes[5] == 0x7E);
constexpr Frame kStopSelfCheck = make_stop();
static_assert(kStopSelfCheck.size == 6 && kStopSelfCheck.bytes[2] == kStop &&
              kStopSelfCheck.bytes[4] == kStop);
constexpr Frame kHeightSelfCheck = make_target_height(725);
static_assert(kHeightSelfCheck.size == 8 && kHeightSelfCheck.bytes[2] == kGotoHeight &&
              kHeightSelfCheck.bytes[4] == 0x02 && kHeightSelfCheck.bytes[5] == 0xD5);
constexpr Frame kPositionFourSelfCheck = make_move_to_position(4);
static_assert(kPositionFourSelfCheck.size == 6 && kPositionFourSelfCheck.bytes[2] == kMovePosition4);
static_assert(make_move_to_position(5).size == 0);
}  // namespace
}  // namespace pandadesk::jiecang_jarvis_rj45
