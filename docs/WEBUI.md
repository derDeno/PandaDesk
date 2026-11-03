# Using the web UI

[Documentation](README.md) · [PandaDesk](../README.md)

## Open the web UI

On the same network as PandaDesk, open `http://pandadesk.local`, its automatic
fallback hostname, or its IP address. For the initial Wi-Fi and desk configuration,
follow [setup](SETUP.md).

## Home: move the desk and use presets

- **Up / Down:** click for a one-second movement, or hold for at least 350 ms to
  continue moving until release.
- **Stop:** stop movement, including a target-height move or preset recall.
- **Target height:** enter a height within the saved limits and submit it. This
  movement continues toward the target after releasing the control. It is
  unavailable for Loctek/FlexiSpot and JCHT35K72C.
- **Saved presets 1–4:** Recall uses a stored desk position. Save overwrites that
  controller memory with the current position. Presets require a supported
  Jiecang RJ12 desk and controller memory reports; unknown slots are unavailable.
- **Lock desk / Unlock desk:** change the controller child lock when supported.
  While locked, movement controls are disabled; Stop and Unlock remain available.

The interface disables controls when the profile or reported controller capability
is unavailable. A child-lock state of **Unavailable** means it has not been confirmed.
A handset-only lock change may remain cached until desk activity or an explicit
lock/unlock action refreshes it. See [protocol details](ARCHITECTURE.md#jiecang-rj12-child-lock).

## Settings

| Card | What you can do |
| --- | --- |
| Device settings | View the automatic local/DHCP names and set LED brightness. |
| Desk settings | Choose profile/model and height limits; restart to apply the desk profile. |
| Wi-Fi setup | Save the network name/password and reboot onto that network. |
| Home Assistant / MQTT | Enable MQTT and enter broker connection details; see [integrations](INTEGRATIONS.md). |
| Firmware update | Upload firmware or web UI filesystem images separately. |
| Reset | Erase saved settings and return to first-time setup. |

## Updates, restart, and reset

In **Settings → Firmware update**, choose an application `.bin` under **Firmware
file** and select **Upload and install**. For a web UI update, choose the
LittleFS `.bin` under **Filesystem file** and select **Upload filesystem**.
Keep the board powered until it restarts. Use images built for PandaDesk's
current flash layout; see [building and flashing](BUILDING.md) for image and
partition details.

**Restart device** reboots without erasing settings. **Reset device** opens a
confirmation dialog and deletes saved Wi-Fi, device, LED, and MQTT settings;
after confirming, repeat [setup](SETUP.md).

## Info and device logs

The **Info** tab sits between Home and Settings and contains two expandable cards.
Device information opens by default and shows hardware and PCB revision,
firmware/filesystem versions, free/minimum heap, reset reason, uptime, network
and MQTT details. PCB revision is set by `kPcbRevision` in `src/api.cpp`, currently
`v1.0`. Wi-Fi signal uses a friendly strength label followed by dBm in brackets.
Opening this card or selecting Refresh loads a new snapshot from
`/api/v1/info` and `/api/v1/state`; hardware diagnostics have no background poll.
Home focuses on desk status, saved presets and movement controls.
On page load, the browser checks the [latest GitHub release](https://github.com/derDeno/PandaDesk/releases/latest)
once. Newer uploaded `PandaDesk_firmware_v<version>.bin` and
`PandaDesk_filesystem_v<version>.bin` assets show green Update available badges
beside their respective installed versions and a dismissible green toast. Both
link to the release page. Firmware and filesystem versions are compared separately,
using numeric major/minor/patch values. Drafts, prereleases, missing or incomplete
assets and unknown installed versions do not trigger a notice. Offline, timeout
and GitHub rate-limit failures leave indicators hidden until reload. The check runs
in the browser; it adds no device task or flash writes and does not download images.

Click Up or Down for the existing one-second movement, or hold for at least
350 ms to keep moving until release. Mouse, touch, pen, Space and Enter are
supported. Releasing a hold, cancelling the input, leaving Home or losing focus
sends Stop. Held movement renews a one-second command every 250 ms after the
previous response; the device expires it if renewal stops.

The Logs card loads recent device logs and polls for new output every two seconds
only while the card is expanded on Info and the browser tab is visible. Pause stops polling;
Auto-scroll can be disabled while reading, and Download logs saves the currently
displayed text. The old `#logs` link opens Info with Logs expanded.
The badge beside Download logs shows Live with a pulsing dot while updates are
working, and stops pulsing when paused, inactive or retrying a failed connection.

The firmware keeps a fixed 8 KiB RAM ring buffer and temporarily allocates another
8 KiB while serving a snapshot. Browser history is capped at 32 KiB of text.
Older device output is overwritten; logs clear on restart and exclude early
bootloader output and crash dumps. USB logging remains enabled. Individual
captured fragments are capped at 511 bytes, and raw UART tracing remains opt-in.
No log data is written to flash. See [`GET /api/v1/logs`](API.md#read-device-logs).

Host checks, from `firmware/`:

```sh
clang++ -std=c++17 -Iinclude tests/log_buffer_test.cpp -o log_buffer_test.exe
./log_buffer_test.exe
node tests/check_webui.js
node tests/check_updates.js
```

## Status LED and device status

The power LED defaults to 50% brightness; saved brightness settings take precedence,
and 0% keeps it off. Setup AP mode blinks four times per second. Otherwise the LED
pulses over three seconds while connecting to Wi-Fi or waiting for desk setup/data.
It stays steady once local Wi-Fi is connected and the desk has reported a height.
Desk detection uses the received height since boot; it does not detect unplugging
a previously detected desk that may also be legitimately asleep.

Info holds network information, firmware/filesystem versions and uptime from
`/api/v1/state`; opening Device information or selecting Refresh updates them.
Desk status, controls and Home Assistant / MQTT status update automatically every three seconds.
The MQTT section shows "Not configured yet" until connected, then displays the broker,
connection status, state topic and availability topic. When opened by IP on local
Wi-Fi, it offers the mDNS address registered with the latest device snapshot.
Settings saves show a green toast for five seconds; errors remain visible until dismissed.

Additional host checks, from `firmware/`:

```sh
node tests/check_webui.js
clang++ -std=c++17 -Iinclude tests/jiecang_model_test.cpp src/jiecang_rj12.cpp -o jiecang_model_test.exe
./jiecang_model_test.exe
clang++ -std=c++17 -Iinclude tests/jiecang_child_lock_test.cpp src/jiecang_rj12.cpp -o child_lock_test.exe
./child_lock_test.exe
clang++ -std=c++17 -Iinclude tests/status_led_test.cpp -o status_led_test.exe
./status_led_test.exe
```
