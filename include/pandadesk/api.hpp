#pragma once

#include "esp_err.h"

namespace pandadesk::api {
esp_err_t mount_webui();
bool schedule_restart();
void start();
}
