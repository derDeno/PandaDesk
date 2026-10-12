# PandaDesk firmware

The firmware targets the ESP32-C6-MINI-1-N4 board. It enters `SAFE_UNCONFIGURED`, sets the desk outputs to their safe levels, and leaves the UART level translator disabled unless a supported saved desk profile/model starts successfully.

The 4 MiB flash layout has two 1.625 MiB OTA app slots (`app0`, `app1`), one 384 KiB LittleFS partition (`littlefs`), and a 256 KiB coredump partition. Both app slots mount the same LittleFS volume at `/webui`. Mounting never formats a failed partition. The OTA updater must keep the shared filesystem compatible with both the current and candidate firmware because it is not independently rolled back. Existing SPIFFS contents are not migrated automatically.

Build from this directory with PlatformIO:

```sh
pio run -e pandadesk-c6
pio run -e pandadesk-c6 -t buildfs
```

Build the LittleFS image with `pio run -e pandadesk-c6 -t buildfs`; its version is stored in `data/version.txt`. The firmware version is stored in `version.txt` at this directory’s root. The Settings → Firmware update page shows the running firmware and filesystem versions and accepts either image. Filesystem OTA replaces the shared LittleFS partition in place, so a power loss during that upload can require reflashing the filesystem over USB. The PandaDesk page is `data/index.html` and reads the current API status; movement buttons stay disabled until the firmware reports a ready profile and capabilities.

For a development-board compile check, use `pio run -e esp32-c6-devkitc-1`. Production uploads must use `pandadesk-c6` and a PandaDesk V1 board.

The firmware uses USB Serial/JTAG for diagnostics. GPIO19 is the desk TX signal and is initialized to UART idle HIGH; no logs are sent through it. The UART translator is enabled only by a supported saved profile.

The Jiecang RJ12 codec is in `include/pandadesk/jiecang_rj12.hpp` and `src/jiecang_rj12.cpp`; the shared UART bridge is in `src/jiecang_transport.cpp`. For the nine RJ12 model IDs listed under device settings, a saved profile starts the documented 9600 8N1 NULL/BREAK/WAKE negotiation. After a valid controller response, it forwards handset frames, accepts API up/down/stop and target-height commands, and requests the current settings/height. Height responses in millimeters or tenths of inches are converted to centimeters. JCHT35K72C target-height control remains unavailable because its reference compatibility notes report that this feature does not work; up/down/stop remain available. These are reference-based software implementations and compatibility candidates, not hardware qualification for each model.

The RJ45 protocol codecs are `include/pandadesk/jiecang_jarvis_rj45.hpp` / `src/jiecang_jarvis_rj45.cpp` and `include/pandadesk/jiecang_jcb35n2.hpp` / `src/jiecang_jcb35n2.cpp`. The shared transport configures UART1 for 9600/8N1 on GPIO19 TX / GPIO18 RX and UART0 as RX-only on GPIO5. It preserves the existing FullyCB2C-A/Jarvis startup, handset forwarding, and API movement behavior. JCB35N2 remains disabled because it uses a different protocol. A successful startup response only shows that the selected serial profile responded; physical validation is still required.

JCB35N2 has a separate stream decoder for its observed `01 01 01 <raw-height>` reports and `01 05 01 AA` idle message; its height value remains raw because a desk-specific calibration is required. This evidence does not document a JCB35N2 serial movement command: community implementations actuate the HS up/down lines instead. JCB35N2 remains unconnected to live UART/control pending its distinct transport, per-model HS mapping, and hardware qualification.

Loctek/FlexiSpot RJ45 has a separate transport in `src/loctek_transport.cpp`. Only FlexiSpot E7 Pro Plus (HS13M-1C0 / CB38M2L) is selectable; other listed model families are disabled pending verification. The implementation uses 9600/8N1 `9B … 9D` frames, buffers handset button responses, responds to controller `0x11` polls, decodes `0x12` display-height frames, and drives the dedicated active-high wake output. API up/down commands are poll-synchronized and time-limited; stop sends an idle button response on the next poll. Target-height movement is not available for this protocol. The reference reports testing the E7 Pro Plus pairing, but it has not been qualified on PandaDesk hardware.

At startup, firmware reads `wifi/ssid` and `wifi/password` from NVS. Without a saved SSID it starts an open setup AP named `PandaDesk-` plus the last four station-MAC hex digits. With saved credentials it tries station mode for the configurable `PANDA_WIFI_CONNECT_TIMEOUT_MS` (default 20 seconds), then falls back to that AP if it cannot connect. Once station mode gets an IP address, it advertises `pandadesk.local`; if that name already resolves on the LAN, it uses `pandadesk-xxxx.local` with the MAC suffix instead. Wi-Fi credentials can be changed in Settings and take effect after reboot.

GPIO7 is driven fully on before settings load. The `device/led_brightness` NVS byte is then applied as a 0–100% PWM value; missing or invalid values keep it at 100%. The web UI can update LED brightness immediately and persist it. Desk profile choices are saved in NVS; movement controls unlock after the selected transport receives its startup/poll response. MQTT settings start an MQTT 3.1.1 client when enabled. Home Assistant discovery publishes height, movement, controller-ready, and profile sensors, plus up/down/stop buttons and a target-height number only when supported. Commands and state use retained discovery/state messages and broker availability. The current broker transport is unencrypted TCP; use it only on a trusted network.

The local REST API listens on port 80 in setup-AP and connected station mode:

- `GET /api/v1/state` returns device state and desk readiness/height when available.
- `GET /api/v1/capabilities` reports controls only after the selected transport is ready.
- `POST /api/v1/commands/move` accepts `request_id` (1–64 characters), `direction` (`up`, `down`, or `stop`), and `expires_ms` (1–5000 for `up`/`down`; omit for `stop`). It requires no password or token. Jiecang movement is stopped with a STOP frame; Loctek movement ends with an idle button response to the controller's next poll.
- `POST /api/v1/commands/height` accepts `request_id` and a one-decimal `height_cm`. It checks the saved minimum/maximum, then sends only for profiles with target-height support; Loctek/FlexiSpot returns `409 target_height_unavailable`.
- `GET`/`POST /api/v1/settings/wifi` reads or saves the SSID/password. `GET` never returns the password; a blank password saves an open network. Saving schedules a reboot.
- `GET`/`POST /api/v1/settings/device` reads or saves the desk profile, its model, LED brightness (`0`–`100`), and minimum/maximum heights in cm (one decimal, `0.0`–`200.0`). RJ12 accepts the nine model IDs shown in Settings; Jarvis RJ45 accepts FullyCB2C-A; Loctek/FlexiSpot RJ45 accepts E7 Pro Plus. JCB35N2 and unverified Loctek variants are disabled and rejected by the API. Defaults for height are `60.0` and `125.0`; the minimum must be below the maximum. At LED brightness `0`, the status LED is off. JCHT35K72C target-height and Loctek target-height control are unavailable.
- `GET`/`POST /api/v1/settings/mqtt` reads or saves broker settings in NVS and starts/reloads the MQTT client. Credentials are not returned; a blank password keeps the current saved password unless `clear_password` is true. MQTT uses plain TCP, default port `1883`, and Home Assistant MQTT discovery prefix `homeassistant`. Up/down button presses request one second of movement; stop sends an immediate stop request. The target-height number appears only when the selected, ready protocol supports it.
- `POST /api/v1/update/firmware` accepts a raw `application/octet-stream` PandaDesk firmware image, validates it, writes the inactive OTA app slot, and reboots into it. The shared LittleFS volume is not updated by this endpoint.
- `POST /api/v1/update/filesystem` accepts a raw `application/octet-stream` LittleFS image, replaces the shared filesystem partition, and reboots into it. Keep `data/version.txt` updated when building a filesystem release.
- `POST /api/v1/device/reset` erases the NVS partition and reboots to setup mode.

HTTP `GET /` and other non-API paths serve static files from LittleFS at `/webui`; `/` maps to `/index.html`, and missing routes fall back to that page for single-page apps.

This API is intentionally unauthenticated for simple local integrations. The setup AP is open and HTTP is unencrypted, so any client on the AP or reachable local network can change settings, upload firmware, or reset the device. Keep it off untrusted networks and do not expose it directly to the internet. A qualified desk profile and command transport are still required before movement can be enabled. URL-based authentication may be considered later; it is not implemented now.
