#include "pandadesk/logs.hpp"

#include <cstdarg>
#include <cstdio>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"

namespace pandadesk::logs {
namespace {
Buffer buffer;
portMUX_TYPE buffer_lock = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t previous_output = nullptr;
uint32_t boot = 0;

int capture(const char *format, va_list arguments) {
  char text[512];
  va_list copy;
  va_copy(copy, arguments);
  const int length = std::vsnprintf(text, sizeof(text), format, copy);
  va_end(copy);
  if (length > 0) {
    const size_t size = static_cast<size_t>(length) < sizeof(text) ? length : sizeof(text) - 1;
    // shortcut: cap each log fragment at 511 bytes; increase if diagnostics need longer messages.
    if (static_cast<size_t>(length) >= sizeof(text)) text[size - 1] = '\n';
    portENTER_CRITICAL(&buffer_lock);
    buffer.append(text, size);
    portEXIT_CRITICAL(&buffer_lock);
  }
  return previous_output(format, arguments);
}
}  // namespace

void start() {
  if (previous_output != nullptr) return;
  boot = esp_random();
  previous_output = &std::vprintf;
  previous_output = esp_log_set_vprintf(capture);
}

uint32_t boot_id() { return boot; }

Snapshot read(uint64_t since, char *output) {
  portENTER_CRITICAL(&buffer_lock);
  const Snapshot snapshot = buffer.read(since, output);
  portEXIT_CRITICAL(&buffer_lock);
  return snapshot;
}
}  // namespace pandadesk::logs
