# PandaDesk

Add local wireless control to your height-adjustable desk. PandaDesk lets you use
a browser, Elgato Stream Deck, Home Assistant, or your own integration while keeping
the original handset on supported desk/controller combinations.

> [!IMPORTANT]
> PandaDesk V1 is a prototype. Check the [hardware and compatibility guide](docs/HARDWARE.md)
> for your exact controller and handset before connecting it. Connect only one
> desk/interface pair at a time; connected signals and supplies must stay within 0–5 V.

## What you can do

- Move your desk from the built-in web UI and view its height and status.
- Set sitting/standing target heights on supported profiles.
- Recall and save desk presets, and use controller child lock where supported.
- Add Stream Deck keys for everyday desk control.
- Connect Home Assistant through MQTT or build a local integration with the HTTP API.
- Update firmware and the web UI from your browser.

Available controls depend on the selected desk profile and data reported by the controller.

## Get started

1. Check [compatibility and wiring](docs/HARDWARE.md). For an unflashed board,
   install both images using the [flashing guide](docs/BUILDING.md).
2. Power PandaDesk, join **PandaDesk-XXXX**, and open the setup portal.
3. Save your Wi-Fi details, reconnect to your local network, and open `http://pandadesk.local`.
4. Choose your desk profile/model and height limits in **Settings → Desk settings**,
   save, and restart the device.

Follow the [setup guide](docs/SETUP.md) for detailed steps and connection troubleshooting.

## Documentation

| I want to… | Guide |
| --- | --- |
| Set up PandaDesk | [Setup and network access](docs/SETUP.md) |
| Use desk controls, presets, logs, or updates | [Using the web UI](docs/WEBUI.md) |
| Control my desk with Stream Deck | [Stream Deck plugin](docs/STREAMDECK.md) — available on the Elgato Marketplace; direct link coming soon. |
| Use the API | [HTTP API reference and examples](docs/API.md) |
| Connect Home Assistant | [Home Assistant / MQTT](docs/INTEGRATIONS.md) |
| Check connectors, power, or hardware details | [Hardware and compatibility](docs/HARDWARE.md) |
| Build or flash the firmware | [Building, flashing, and updating](docs/BUILDING.md) |
| Explore protocols, GPIOs, or firmware internals | [Firmware architecture](docs/ARCHITECTURE.md) |

Browse the [documentation index](docs/README.md) for all guides.

## Contributing

Protocol captures, desk profiles, compatibility reports, and hardware testing are
welcome. Include exact controller/handset identifiers and measurements; see the
[technical contribution notes](docs/ARCHITECTURE.md#contributing).

## License

See the [project license](LICENSE.md) for software/firmware and hardware terms.
