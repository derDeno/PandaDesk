# Firmware architecture and protocol details

[Documentation](README.md) · [PandaDesk](../README.md)

Run build and host-check commands from the firmware root, which contains `platformio.ini`.

## Firmware architecture

### Jiecang RJ12 saved presets

The web UI provides Recall and Save controls for the desk's native memory slots
1–4. Save overwrites the selected desk memory with the current position.
`capabilities.presets` becomes true only for a ready RJ12 transport that has
received a memory report. Other transports retain `false`.
`state.presets` contains four heights in cm; unset, unknown, or unconvertible
heights are `null`. The existing conservative raw-height conversion applies.

Send `POST /api/v1/commands/preset` with a nonempty `request_id` (up to 64 bytes),
integer `preset` from 1 to 4, and `operation` equal to `recall` or `save`.
HTTP 202 with `accepted: true` means queued, not completed or confirmed saved.
Both operations reject a reported child lock with HTTP 409 / `child_locked`.
Unsupported controllers return 409 / `presets_unavailable`; empty or unreadable
recalls return 409 / `preset_unavailable`. Recall also enforces the configured
height limits (400 / `height_out_of_range`), including before UART transmission
if a handset has changed the cached slot while the command was queued.

Recall uses native `0x05`, `0x06`, `0x27`, `0x28` commands; Save uses `0x03`,
`0x04`, `0x25`, `0x26`. Saving invalidates the cached slot and requests settings
readback without inventing a saved height. Handset memory saves also trigger
readback. Preset commands use the existing command generation so Stop cancels
queued commands. The controller's own memory movement mode is preserved.

Host codec check: compile `tests/jiecang_preset_test.cpp` with
`src/jiecang_rj12.cpp` and `-std=c++17 -Iinclude`, then run the executable.
Web UI and Stream Deck tests cover request modes, disabled controls, height
limits, child lock, and Stop ordering. Native preset operation still needs
hardware qualification.

### Jiecang RJ12 child lock

The RJ12 transport reads child-lock status using `F1 F1 1F 01 00 20 7E`
once at startup, on valid height-report activity (at most once every three
seconds), and for explicit Lock/Unlock actions. It does not query an idle desk:
those queries wake the handset and prevent standby. Browser status polling
only reads the cached controller state. The zero parameter only queries the state.
Validated controller replies
with opcode `0x1F` and payload `0` or `1` supply `/api/v1/state.locked`.
Until a valid reply arrives, the API returns `null` and the web UI displays
`Unavailable`. Other transports retain that fallback. The `child_lock`
capability becomes true for a ready RJ12 desk that reports its lock state.

The web UI's Lock desk / Unlock desk button sends
`POST /api/v1/commands/child-lock` with a `request_id` and boolean `locked`.
The firmware queries the current state first, sends the `0x1F` parameter-1
toggle only if a change is needed, and waits for a controller report confirming
the requested state. Repeating a request does not toggle an already-matching
state. The API returns 200 with the confirmed `locked` value, or 504 if a query
or confirmation times out. An uncertain toggle is never retried automatically.
Frames use the existing UART command queue. One asynchronous HTTP worker waits
for replies, leaving both the UART owner and HTTP server available for movement,
status and STOP handling. Concurrent lock changes return 409 while it is busy.
While locked, the web UI disables Up, Down and the target-height controls.
The shared Jiecang transport also rejects HTTP/MQTT movement requests and drops
queued movement frames if a lock is reported before transmission. HTTP returns
409 with `child_locked`. Stop and Unlock remain available.

Read-only queries and both reply values were verified over USB with the
Maidesite MDS36DR on 2026-11-02, comparing the response against the handset's
locked/unlocked display. The handset did not broadcast those changes, so a
handset-only lock change with no height traffic leaves the last confirmed state
visible until desk activity or an explicit web lock/unlock action refreshes it.
The startup reply also allows connection detection without desk movement.
Lock/unlock commands, repeated and concurrent requests, and invalid-payload
rejection were verified on the same Maidesite MDS36DR on 2026-11-03.

Run `python tests/check_child_lock_device.py http://pandadesk.local` against a
connected desk to check lock/unlock, repeated requests and payload validation.
This hardware check changes the lock and restores its initial state afterward.

### Scheduling and desk latency

The ESP32-C6 has one application core (`CONFIG_FREERTOS_UNICORE`); its LP core
cannot run a second ordinary FreeRTOS application task. Desk and network tasks
therefore share the application core instead of being pinned to separate cores.

Jiecang TX/handset and Loctek controller/handset tasks run at priority 19. Their
UART/queue waits release the CPU when idle. This preempts the TCP/IP task (18),
HTTP/Wi-Fi management/DNS tasks (5), and MQTT management (4), while leaving the
Wi-Fi driver (23), system events (20), and system timer (22) above the desk tasks.
Transport initialization remains at priority 6. This follows Espressif's
[task-priority guidance](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/performance/speed.html#choosing-task-priorities-of-the-application).

`CONFIG_FREERTOS_HZ=1000` gives a 1 ms tick. At the previous 100 Hz, Loctek's
4 ms handset reply wait rounded down to zero ticks; a compile-time assertion
now rejects that configuration. For an existing generated `sdkconfig.*`, select
1000 Hz under Component config > FreeRTOS > Kernel in `pio run -e pandadesk-c6
-t menuconfig`; saved values override `sdkconfig.defaults`.

These settings improve scheduling but do not guarantee an end-to-end deadline.
The 1 kHz tick adds scheduling overhead. Wi-Fi/system interrupts, UART/protocol
timing, and flash erase/write operations can still delay desk handling. Validate
latency and missed-frame counts on hardware with a logic analyzer under HTTP,
MQTT and Wi-Fi load, and test flash updates separately. Flash-cache restrictions
also apply to a dual-core design; pinning alone does not provide full isolation.

### Desk profiles

Firmware is organized around desk profiles. Each profile defines:

- Interface family and operating mode
- UART baud rate and framing
- Packet boundaries and checksums
- Commands, responses, polling, and timeouts
- Wake method and timing
- Jiecang control-line functions
- Movement and stop behavior
- Recovery after framing errors, reset, or communication loss

### GPIO and module wiring

The firmware needs two simultaneous receive paths and one transmit path:

| Signal | ESP32-C6 GPIO | WROOM module pad | Purpose |
|---|---:|---:|---|
| `E_TX` | 19 | 17 | UART output to the desk controller |
| `E_RX` | 18 | 16 | UART input from the desk controller |
| `E_RX_HANDSET` | 5 | 5 | Separate UART input from the original handset |
| `EN_TX` | 15 | 23 | Common enable for the UART translator |
| `E_WAKE` | 10 | 11 | Loctek wake output |
| `E_HS0` | 23 | 21 | Jiecang open-drain control 0 |
| `E_HS1` | 22 | 20 | Jiecang open-drain control 1 |
| `E_HS2` | 21 | 19 | Jiecang open-drain control 2 |
| `E_HS3` | 20 | 18 | Jiecang open-drain control 3 |
| `LED` | 7 | 7 | Status LED |
| `SDA` | 0 | 8 | I²C data |
| `SCL` | 1 | 9 | I²C clock |
| USB D− | 12 | 13 | USB data through R18 = 22 Ω |
| USB D+ | 13 | 14 | USB data through R17 = 22 Ω |
| BOOT / `IO9` | 9 | 15 | R11 = 10 kΩ to 3.3 V; button to GND |
| `IO8` | 8 | 10 | R19 = 10 kΩ to 3.3 V for download mode |
| `TXD` / TXD0 | 16 | 25 | Programming UART output |
| `RXD` / RXD0 | 17 | 24 | Programming UART input |

GPIO numbers and physical module pad numbers are different. Firmware GPIO
assignments are defined in [pins.hpp](../include/pandadesk/pins.hpp).
The WROOM-1 does not expose GPIO14, so `E_WAKE` now uses GPIO10.

Connect 3.3 V to pad 2, EN to pad 3, and GND to pads 1, 28, and exposed
pad 29; leave NC pad 22 unconnected. Libraries may split the exposed ground
pad into pads 29–37; verify their symbol-to-footprint mapping and ground all
subpads. Retain EN's R12 = 10 kΩ pull-up to 3.3 V, C10 = 1 µF to GND, and
RESET button to GND. GPIO8's R19 is a separate pull-up: at reset, download
mode requires GPIO8 HIGH and GPIO9 LOW; normal flash boot requires GPIO9
HIGH and accepts either GPIO8 level. See the
[Espressif WROOM datasheet](https://documentation.espressif.com/esp32-c6-wroom-1_wroom-1u_datasheet_en.html).

`EN_TX` enables the translator's transmit output and both receive outputs together. It must remain enabled while waiting for handset UART activity. Pulling it low also prevents the ESP32 from receiving the handset and controller UART signals.

Manual handset actions should take priority over automations. All traffic to the controller should pass through one transmit queue so that only one complete packet is sent at a time. Firmware must not replay stale movement commands after reset, buffer overflow, or loss of synchronization.

### Loctek wake handling

Loctek OUT pin 4 is driven by the dedicated wake circuit. Handset IN pin 4 is intentionally not connected or monitored in V1. The intended wake sequence is:

1. Keep handset UART reception active while the controller is asleep.
2. Detect valid handset UART activity or a local PandaDesk command.
3. Buffer the first packet and assert the wake output.
4. Wait for the model-specific ready condition or delay.
5. Send the buffered packet and release or hold wake as required by the profile.

The firmware now keeps handset UART reception active, buffers handset button responses, drives the wake output for the documented startup interval, and sends button responses in reply to controller polls. A handset that only toggles IN pin 4 and waits before sending UART data cannot be detected by V1. Wake timing, passthrough, and sleep behavior still need PandaDesk hardware validation per controller/handset pair.

### Safe startup

At startup, firmware should:

1. Keep the UART translator disabled.
2. Keep Loctek wake inactive.
3. Release all Jiecang open-drain outputs.
4. Load and validate the selected desk profile.
5. Initialize both UART receivers and the transmit queue.
6. Set desk TX to the normal UART idle level.
7. Enable the translator and perform profile-specific wake or initialization.

Debug and boot messages must never be routed to the desk UART output. Runtime diagnostics should preferably use native USB.

## Contributing

Contributions are welcome, especially:

- Verified protocol captures and documentation
- New desk profiles
- Tests for UART forwarding, wake behavior, and command arbitration
- Confirmed controller/handset compatibility reports
- Hardware review and prototype measurements

Please avoid publishing unverified pinouts as working configurations. Include measurements and exact hardware identifiers whenever possible.
