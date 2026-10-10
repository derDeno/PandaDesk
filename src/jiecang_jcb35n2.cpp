#include "pandadesk/jiecang_jcb35n2.hpp"

namespace pandadesk::jiecang_jcb35n2 {

bool Decoder::push(uint8_t byte, Message *message) {
  if (message == nullptr) return false;
  window_[size_++] = byte;
  if (size_ != kFrameSize) return false;

  if (window_[0] == 0x01 && window_[1] == 0x01 && window_[2] == 0x01) {
    message->type = MessageType::height;
    message->raw_height = window_[3];
    size_ = 0;
    return true;
  }
  if (window_[0] == 0x01 && window_[1] == 0x05 && window_[2] == 0x01 && window_[3] == 0xAA) {
    message->type = MessageType::idle;
    message->raw_height = 0;
    size_ = 0;
    return true;
  }

  window_[0] = window_[1];
  window_[1] = window_[2];
  window_[2] = window_[3];
  size_ = kFrameSize - 1;
  return false;
}

}  // namespace pandadesk::jiecang_jcb35n2
