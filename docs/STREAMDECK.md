# Stream Deck plugin

[Documentation](README.md) · [PandaDesk](../README.md)

Control PandaDesk from Elgato Stream Deck over your local network. No Home
Assistant, MQTT broker, cloud account, or separate background service is required.

## Install from the Elgato Marketplace

The PandaDesk plugin is available on the **Elgato Marketplace**.
The direct Marketplace link will be added here later.

<!-- Add the PandaDesk Elgato Marketplace listing URL here when available. -->

Search for **PandaDesk** in the Marketplace and install it through Stream Deck.

## Requirements

- Windows 10 or later.
- Elgato Stream Deck software 7.1 or later and a compatible key device.
- A [configured PandaDesk](SETUP.md) reachable from the PC over the local network.
- Firmware with HTTP API v1. Available actions depend on the desk profile's capabilities.

Stream Deck supplies Node.js; end users do not need a separate installation.
The plugin supports key actions; USB control, automatic board discovery, and dial
actions are not supported. It uses German when Stream Deck is set to German and
English for other application languages.

## Connect and add controls

1. Drag a **PandaDesk** action onto a Stream Deck key.
2. Select the key, enter the device hostname or IP address in **Board address**
   (for example `http://pandadesk.local`), and click **Save**.
3. The address is shared across all PandaDesk keys and profiles. Set target
   heights, child-lock modes, and preset slots separately for each key.
4. Leave Stream Deck's custom title empty to see live height/status text.
5. Add a dedicated **Stop** key alongside movement and target-height keys.

## Available actions

| Action | How to use it |
| --- | --- |
| Up / Down · hold | Hold to move; release sends Stop. |
| Stop | Stop movement and cancel held-key repetition. |
| Target height · Sit / Stand | Save a target height in cm within the board's limits. Movement continues after releasing the key. |
| Child lock | Choose Lock, Unlock, or Toggle. The icon follows the confirmed controller state. |
| Preset · Recall | Select an existing desk memory 1–4; the plugin recalls it without overwriting it. |
| Height & status | Show measured height and movement status; press to refresh. |

Target-height keys are unavailable when the profile does not support them or the
limits cannot be read. Presets require Jiecang RJ12 memory reports and firmware
exposing preset support; use the [web UI](WEBUI.md#home-move-the-desk-and-use-presets)
to save desk memories. Recall follows the desk's own memory movement mode.
Child-lock Toggle requires a known controller state. Stop remains available while
a lock change is awaiting confirmation.

## Troubleshooting

| Key display | Next step |
| --- | --- |
| Set up | Enter and save the board address. |
| Offline | Check power, address, local network access, and [API availability](API.md#quick-start). |
| Not ready | Check desk profile/model, power, and cabling. |
| Not supported | The profile or installed firmware does not provide the action. |
| State unknown | Wait for a confirmed lock state or request Lock/Unlock explicitly. |
| Please wait | A child-lock change is awaiting confirmation. |
| Alert triangle | Check `logs/plugin.log` in the installed plugin folder for details. |

