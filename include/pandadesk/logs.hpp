#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pandadesk::logs {
constexpr size_t kCapacity = 8192;

struct Snapshot {
  uint64_t next;
  size_t size;
  bool dropped;
};

// Callers synchronize access; storage never grows with log volume.
class Buffer {
 public:
  void append(const char *text, size_t length) {
    const uint64_t next = total_ + length;
    if (length > kCapacity) {
      text += length - kCapacity;
      length = kCapacity;
    }
    const size_t offset = (next - length) % kCapacity;
    const size_t first = length < kCapacity - offset ? length : kCapacity - offset;
    std::memcpy(data_ + offset, text, first);
    std::memcpy(data_, text + first, length - first);
    total_ = next;
  }

  Snapshot read(uint64_t since, char *output) const {
    const uint64_t oldest = total_ > kCapacity ? total_ - kCapacity : 0;
    if (since > total_) since = 0;
    const bool dropped = since < oldest;
    if (dropped) since = oldest;
    const size_t size = total_ - since;
    const size_t offset = since % kCapacity;
    const size_t first = size < kCapacity - offset ? size : kCapacity - offset;
    std::memcpy(output, data_ + offset, first);
    std::memcpy(output + first, data_, size - first);
    return {total_, size, dropped};
  }

 private:
  char data_[kCapacity]{};
  uint64_t total_ = 0;
};

void start();
uint32_t boot_id();
Snapshot read(uint64_t since, char *output);
}  // namespace pandadesk::logs
