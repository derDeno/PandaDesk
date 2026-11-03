# PandaDesk HTTP API

[Documentation](README.md) · [PandaDesk](../README.md)

PandaDesk exposes a local HTTP API for reading desk status and controlling a
configured desk. Use the device's IP address or automatic mDNS hostname as the
base URL, for example `http://192.168.1.42`.

This reference covers status, device logs and desk-control endpoints. Responses
below are examples; fields and available
capabilities depend on the firmware and selected desk profile.

## Quick start

Read the device state and capabilities before sending desk commands:

```sh
curl http://192.168.1.42/api/v1/state
curl http://192.168.1.42/api/v1/capabilities
```

Send command bodies as JSON with `Content-Type: application/json`. Commands
require a nonempty `request_id` of up to 64 bytes. For example, stop the desk:

```sh
curl -X POST http://192.168.1.42/api/v1/commands/move \
  -H 'Content-Type: application/json' \
  -d '{"request_id":"stop-1","direction":"stop"}'
```

A `202 Accepted` response means the command was accepted for processing;
it does not confirm that the desk reached the requested position.

## Read device state

`GET /api/v1/state`

### Request

No body.

### Response · 200 OK

```json
{
  "state": "READY",
  "profile": "jiecang_jarvis_rj45",
  "ready": true,
  "height": {
    "value": null,
    "quality": "unknown"
  },
  "moving": null,
  "locked": null,
  "presets": [
    null,
    null,
    null,
    null
  ],
  "sequence": 1,
  "firmware_version": "0.1.0",
  "filesystem_version": "0.1.0"
}
```

## Read device information

`GET /api/v1/info`

Returns an on-demand JSON diagnostic snapshot with `Cache-Control: no-store`.
The Info page requests it when Device information opens or Refresh is selected.
It creates no background task or additional periodic poll. Firmware/filesystem
versions, uptime, network and MQTT details remain available in `/api/v1/state`.

| Fields | Meaning |
| --- | --- |
| `target`, `chip_revision`, `cpu_cores`, `cpu_mhz` | Build target, detected silicon revision (`major * 100 + minor`), detected core count and configured CPU frequency |
| `pcb_revision` | PCB revision string, currently `v1.0`; set by `kPcbRevision` in [`src/api.cpp`](../src/api.cpp) |
| `flash_bytes` | Detected flash capacity, or null if unavailable |
| `project`, `sdk_version`, `build_date`, `build_time`, `elf_sha256` | Application and build metadata; build date/time may be empty if omitted by the build |
| `firmware_partition`, `firmware_partition_bytes`, `webui_partition_bytes` | Running OTA slot label and partition capacities; unavailable sizes are null |
| `reset_reason` | ESP-IDF `esp_reset_reason_t` value |
| `heap_free_bytes`, `heap_min_free_bytes`, `heap_largest_block_bytes` | Current free heap, minimum free heap and largest free allocation for 8-bit capable memory |
| `task_count`, `log_capacity_bytes` | Current FreeRTOS task count and retained log buffer capacity |

## Read device logs

`GET /api/v1/logs`

Returns recent application logs as `text/plain; charset=utf-8` with
`Cache-Control: no-store`. The device stores the latest 8 KiB in RAM, captures
from the start of `app_main`, and keeps USB log output enabled. Individual
captured log fragments are limited to 511 bytes. Logs clear on reboot; early
bootloader output and crash dumps are not captured.

Response headers:

| Header | Meaning |
| --- | --- |
| `X-Log-Cursor` | Byte position immediately after this response's output |
| `X-Log-Boot` | Identifier for this boot; treat as an opaque decimal string |
| `X-Log-Dropped` | `1` if earlier requested output has been overwritten, otherwise `0` |

For incremental reads, pass both values from the previous response:
`GET /api/v1/logs?since=1234&boot=5678`. An unchanged buffer returns an empty
body with status 200. After a reboot, a different boot identifier returns the
available new logs; clients should reset their displayed history. Malformed
cursors return 400; snapshot allocation failure returns 503. Readers have
independent cursors and do not consume each other's logs.

## Read capabilities

`GET /api/v1/capabilities`

### Request

No body.

### Response · 200 OK

```json
{
  "profile_configured": true,
  "move": true,
  "stop": true,
  "target_height": true,
  "presets": true,
  "child_lock": true
}
```

## Move desk up, down, or stop

`POST /api/v1/commands/move`

### Request · application/json

```json
{
  "request_id": "unique-id",
  "direction": "up",
  "expires_ms": 1000
}
```

### Response · 202 Accepted

```json
{
  "accepted": true
}
```

direction is up, down, or stop. Up/down run for expires_ms (1–5000 ms) and then stop automatically. A reported child lock rejects Up/down with 409 and child_locked. Stop remains available and takes priority over queued API moves.

## Lock or unlock the desk

`POST /api/v1/commands/child-lock`

### Request · application/json

```json
{
  "request_id": "unique-id",
  "locked": true
}
```

### Response · 200 OK

```json
{
  "locked": true
}
```

Set locked to true to lock or false to unlock. Success means the desk confirmed the requested state. Repeating the same request leaves that state unchanged. Requires the child_lock capability; unavailable control returns 409 and missing confirmation returns 504.

## Recall or save a desk preset

`POST /api/v1/commands/preset`

### Request · application/json

```json
{
  "request_id": "unique-id",
  "preset": 1,
  "operation": "recall"
}
```

### Response · 202 Accepted

```json
{
  "accepted": true
}
```

preset is an integer from 1 to 4. operation is recall or save. Save overwrites the selected controller memory with the current position; acceptance only means queued. Read the four heights in state.presets (cm; null means unavailable). Requires the presets capability and Jiecang RJ12 memory reports. Both operations reject a reported child lock with 409 / child_locked. Recall rejects unreadable or unset memories with 409 / preset_unavailable and positions outside the configured height limits with 400 / height_out_of_range.

## Move desk to a target height

`POST /api/v1/commands/height`

### Request · application/json

```json
{
  "request_id": "unique-id",
  "height_cm": 72.5
}
```

### Response · 202 Accepted

```json
{
  "accepted": true
}
```

height_cm must have one decimal and be within the saved limits. Out-of-range requests return 400 with height_out_of_range. A reported child lock returns 409 with child_locked. Target-height control is unavailable for Loctek/FlexiSpot and JCHT35K72C.
