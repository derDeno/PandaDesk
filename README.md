# PandaDesk

Add local wireless control to your height-adjustable desk. PandaDesk lets you use
a browser, Elgato Stream Deck, Home Assistant, or your own integration while keeping
the original handset on supported desk/controller combinations.

**PandaDesk's standout feature: one board for all three common desk controller types.**
It is the only board on the market that supports Jiecang RJ12, Jiecang/Jarvis RJ45,
and Loctek/FlexiSpot RJ45, while most alternatives support just one.

> [!IMPORTANT]
> PandaDesk V1 is a prototype. Check the [hardware and compatibility guide](https://github.com/derDeno/PandaDesk/wiki/Hardware-and-compatibility)
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

1. Check [compatibility and wiring](https://github.com/derDeno/PandaDesk/wiki/Hardware-and-compatibility). For an unflashed board,
   install both images using the [flashing guide](https://github.com/derDeno/PandaDesk/wiki/Building-and-flashing).
2. Power PandaDesk, join **PandaDesk-XXXX**, and open the setup portal.
3. Save your Wi-Fi details, reconnect to your local network, and open `http://pandadesk.local`.
4. Choose your desk profile/model and height limits in **Settings → Desk settings**,
   save, and restart the device.

Follow the [setup guide](https://github.com/derDeno/PandaDesk/wiki/Setup) for detailed steps and connection troubleshooting.

## Documentation

| I want to… | Guide |
| --- | --- |
| Set up PandaDesk | [Setup and network access](https://github.com/derDeno/PandaDesk/wiki/Setup) |
| Use desk controls, presets, logs, or updates | [Using the web UI](https://github.com/derDeno/PandaDesk/wiki/Web-UI) |
| Control my desk with Stream Deck | [Stream Deck plugin](https://github.com/derDeno/PandaDesk/wiki/Stream-Deck-plugin) — available on the Elgato Marketplace; direct link coming soon. |
| Use the API | [HTTP API reference and examples](https://github.com/derDeno/PandaDesk/wiki/HTTP-API) |
| Connect Home Assistant | [Home Assistant / MQTT](https://github.com/derDeno/PandaDesk/wiki/Home-Assistant-and-MQTT) |
| Check connectors, power, or hardware details | [Hardware and compatibility](https://github.com/derDeno/PandaDesk/wiki/Hardware-and-compatibility) |
| Build or flash the firmware | [Building, flashing, and updating](https://github.com/derDeno/PandaDesk/wiki/Building-and-flashing) |
| Explore protocols, GPIOs, or firmware internals | [Firmware architecture](https://github.com/derDeno/PandaDesk/wiki/Firmware-architecture) |

All guides are maintained directly in the [public PandaDesk wiki](https://github.com/derDeno/PandaDesk/wiki).

## Contributing

Protocol captures, desk profiles, compatibility reports, and hardware testing are
welcome. Include exact controller/handset identifiers and measurements; see the
[technical contribution notes](https://github.com/derDeno/PandaDesk/wiki/Firmware-architecture#contributing).

## License

See the [project license](LICENSE.md) for software/firmware and hardware terms.
