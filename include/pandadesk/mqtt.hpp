#pragma once

#include <cstdint>

namespace pandadesk::mqtt {

struct Status {
  bool connected = false;
  char host[129]{};
  uint16_t port = 1883;
  char state_topic[48]{};
  char availability_topic[48]{};
};
Status status();
void start();
void reload();

}  // namespace pandadesk::mqtt
