# Hardware and compatibility

[Documentation](README.md) · [PandaDesk](../README.md)

## Features

- ESP32-C6-WROOM-1-N8 with 8 MB flash, Wi-Fi, Bluetooth LE, and native USB
- Jiecang RJ12 / 6P6C IN and OUT ports
- Jiecang/Jarvis RJ45 / 8P8C IN and OUT ports
- Loctek/FlexiSpot RJ45 / 8P8C IN and OUT ports
- Original-handset support through firmware-managed UART forwarding
- Dedicated 3.3 V / 5 V UART level translation
- Four open-drain outputs for Jiecang handset/control signals
- Dedicated Loctek wake output
- USB-C or compatible 5 V desk power, with automatic USB priority
- Efficient 3.3 V switching regulator
- Boot and reset buttons
- Firmware-controlled status LED
- I²C expansion header
- Internal UART production-programming header
- Four-layer PCB with mounting holes and an ESP32 antenna cutout

## Supported interface targets

| Interface | Connector | Intended use | Status |
|---|---|---|---|
| Jiecang | RJ12 / 6P6C | Accessory port or supported inline configuration | Prototype validation required |
| Jiecang / Jarvis | RJ45 / 8P8C | Inline between handset and controller | Prototype validation required |
| Loctek / FlexiSpot | RJ45 / 8P8C | Inline between handset and controller | Prototype validation required |

Connector type and manufacturer name alone do not establish compatibility. Pinouts and protocols can differ between models from the same manufacturer. A verified compatibility list will be maintained as hardware and firmware testing progresses.

Only one desk/interface pair may be connected at a time.

## How it works

For a typical inline installation:

```text
Original handset ──> PandaDesk IN
PandaDesk OUT    ──> Desk controller
```

The controller-to-handset UART direction remains connected through the board and is also monitored by the ESP32. The handset-to-controller direction passes through the ESP32:

```text
Handset TX ──> level translator ──> ESP32 handset RX
ESP32 TX   ──> level translator ──> Controller RX

Controller TX ──> Handset RX
              └─> level translator ──> ESP32 controller RX
```

This lets the firmware forward handset packets and insert PandaDesk commands between complete protocol transactions. It must never mix bytes from two commands or echo controller responses back to the controller.

When PandaDesk is connected to a supported free accessory port, only the corresponding **OUT** connector is used and **IN** remains empty.

> [!NOTE]
> The handset transmit path is not a passive bypass. The board must be powered and the firmware must be running for inline handset commands to reach the controller.

## Power

PandaDesk can be powered from USB-C or from a compatible 5 V desk interface. The TPS2116 power multiplexer automatically gives USB priority. Desk supplies are diode-isolated before they are combined, and a TPS62162 generates the 3.3 V rail.

A desk exposing a 5 V pin does not necessarily provide enough current for PandaDesk, the original handset, and ESP32 Wi-Fi current peaks. Desk-powered operation must be qualified for each supported model. USB-C should be used when the desk supply is unknown or insufficient.

## Voltage limits

> [!CAUTION]
> All signals and supply pins connected to PandaDesk must remain within **0–5 V**. Interfaces carrying higher voltages are not supported and may damage PandaDesk or the desk controller.

The board uses fixed 3.3 V / nominal 5 V interface circuitry. It is not a universal voltage-detection or arbitrary-pin-routing device.

## GPIO and module wiring

See [firmware architecture](ARCHITECTURE.md#gpio-and-module-wiring) for the full
GPIO/module-pad table, UART translator enable behavior, boot straps, and reset circuit.

