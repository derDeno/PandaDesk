"""Run the real MQTT implementation against a host-side broker/desk test double.

Run from firmware: python tests/check_mqtt.py
Requires clang++ (or CXX) and cJSON from the normal PlatformIO dependencies.
CXXFLAGS can supply host-specific C++ compiler flags.
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


firmware = Path(__file__).resolve().parents[1]
cjson = firmware / "managed_components/espressif__cjson/cJSON"
with tempfile.TemporaryDirectory(prefix="pandadesk-mqtt-") as directory:
    temporary = Path(directory)
    # SDK calls are implemented by mqtt_test.cpp; these headers stand in for the SDK.
    for header in (
        "esp_err.h", "esp_log.h", "esp_mac.h", "esp_timer.h", "mqtt_client.h", "nvs.h",
        "freertos/FreeRTOS.h", "freertos/event_groups.h", "freertos/task.h",
    ):
        path = temporary / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#pragma once\n", encoding="utf-8")
    compiler = os.environ.get("CXX", "clang++")
    executable = temporary / ("mqtt_test.exe" if os.name == "nt" else "mqtt_test")
    cjson_object = temporary / "cJSON.o"
    subprocess.run([
        compiler, "-x", "c", "-c", str(cjson / "cJSON.c"), "-o", str(cjson_object),
    ], check=True)
    subprocess.run([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-parameter",
        *shlex.split(os.environ.get("CXXFLAGS", "")),
        "-I", str(temporary), "-I", str(firmware / "include"), "-I", str(cjson),
        str(firmware / "tests/mqtt_test.cpp"), str(cjson_object), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
