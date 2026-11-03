# Home Assistant and other integrations

[Documentation](README.md) · [PandaDesk](../README.md)

## Home Assistant / MQTT

MQTT is optional. The web UI, HTTP API, and Stream Deck plugin work directly on
the local network without a broker.

1. Configure your desk using the [setup guide](SETUP.md).
2. Open **Settings → Home Assistant / MQTT** and enable MQTT settings.
3. Enter the broker host, port (default `1883`), and credentials if required.
4. Select **Save MQTT settings**. The broker connection restarts.
5. Check **Info → Device information → Home Assistant / MQTT** for broker status,
   state topic, and availability topic.

The firmware publishes Home Assistant discovery configuration under
`homeassistant`. With Home Assistant's MQTT integration connected to the same
broker, PandaDesk advertises these entities:

| Entity | Behavior |
| --- | --- |
| Desk height, profile, ready and moving | Measured desk status; unavailable measurements remain unknown. |
| Move desk up, Move desk down, Stop desk | Up/down run for the configured movement duration; Stop takes priority. |
| Movement duration | 1–5000 ms, initially 1000 ms; applies to MQTT up/down buttons and resets after a device restart. |
| Target height | Move to a height within the saved limits, where supported. |
| Recall preset 1–4 | Recall the controller's saved memory, enforcing the saved height limits. |
| Save preset 1–4 | Overwrite that controller memory with the current desk position. |
| Preset 1–4 height | Saved heights in cm; unset or unreadable memories remain unknown. |
| Child lock | Lock/unlock with controller-confirmed state; a pending change does not block Stop. |
| Minimum height, Maximum height | Persistent limits in cm, in 0.1 cm increments; minimum must be below maximum, within 0–200 cm. |
| Restart | Restart PandaDesk, including when the desk controller is not ready. |

Available desk controls depend on the selected profile and readiness. Presets
and child lock require controller support, just as with the HTTP API. Unsupported
entities are removed from discovery and reappear when supported. Restart and
configuration controls remain available while PandaDesk is connected to MQTT.
Changing height limits also updates the target-height control's range.

### MQTT topics

The device ID is its 12-character Wi-Fi MAC address without separators. Retained
state is published to `pandadesk/<device-id>/state`, with `height_cm`, `moving`,
`ready`, `profile`, `min_height_cm`, `max_height_cm`, `move_duration_ms`, `locked`,
four `presets` heights in cm, and a `capabilities` object. Unknown lock state or
heights are JSON `null`. Availability is published to
`pandadesk/<device-id>/availability` as `online` or `offline`.

Publish commands to `pandadesk/<device-id>/cmd/<command>`:

| Command | Payload |
| --- | --- |
| `up`, `down`, `stop`, `restart` | `PRESS` |
| `target_height` | Height in cm, for example `105.5` |
| `preset_1` … `preset_4` | `PRESS` to recall |
| `save_preset_1` … `save_preset_4` | `PRESS` to overwrite |
| `child_lock` | `LOCK` or `UNLOCK` |
| `move_duration_ms` | Integer from `1` to `5000` |
| `min_height_cm`, `max_height_cm` | Height in cm, for example `65.0` |

Send commands without retain; retained commands and incomplete messages are
ignored to avoid replaying movement, preset saves or restarts after reconnecting.
An existing child lock rejects movement and preset operations; Stop and Unlock
remain available. Failed commands are logged, and state reports only confirmed
measurements. Preset acceptance means queued, not that the save or move finished.

Wi-Fi/MQTT setup, logs, factory reset and firmware uploads remain in the web UI
and HTTP API.

Host regression check (requires `clang++` or `CXX` and the normal PlatformIO
cJSON dependency): `python tests/check_mqtt.py` from the firmware directory.

## Custom local integrations

Use the [HTTP API reference](API.md) for state, capabilities, diagnostics, logs,
and desk commands. Read capabilities before enabling an action. A command being
accepted does not confirm that the desk reached its target or saved a preset.

For internal transport behavior, child lock, and command ordering, see
[firmware architecture](ARCHITECTURE.md).
