#pragma once

#include <cstddef>
#include <cstdint>

namespace pandadesk::jiecang_jcb35n2 {

inline constexpr size_t kFrameSize = 4;

enum class MessageType : uint8_t { height, idle };

struct Message {
  MessageType type = MessageType::idle;
  uint8_t raw_height = 0;
};

class Decoder {
 public:
  bool push(uint8_t byte, Message *message);

 private:
  uint8_t window_[kFrameSize]{};
  size_t size_ = 0;
};

}  // namespace pandadesk::jiecang_jcb35n2
