# Building, flashing, and updating

[Documentation](README.md) · [PandaDesk](../README.md)

Run the commands below from the firmware root (the directory containing
`platformio.ini`). In the PandaDesk firmware repository, this is the repository root.
Install PlatformIO with ESP-IDF support before building.

## Build and install over USB

Connect PandaDesk by USB and run:

```sh
pio run -e pandadesk-c6
pio run -e pandadesk-c6 -t buildfs
pio run -e pandadesk-c6 -t upload
pio run -e pandadesk-c6 -t uploadfs
```

Both uploads are required for a first installation: the firmware and web UI are
separate images. Continue with [first-time setup](SETUP.md) afterward.

## Update an existing installation

Use [Settings → Firmware update](WEBUI.md#updates-restart-and-reset) to install
application and filesystem `.bin` images separately. Keep power connected until
the device restarts. Partition-layout changes require a USB/UART installation
as described below.

## Flash layout and builds

The `pandadesk-c6` environment targets ESP32-C6-WROOM-1-N8 with 8 MB flash.
[`boards/pandadesk-c6.json`](../boards/pandadesk-c6.json),
[`sdkconfig.defaults`](../sdkconfig.defaults), and [`partitions.csv`](../partitions.csv) define
the board, flash size, and partition layout.

| Partition | Offset | Size |
|---|---:|---:|
| NVS | `0x9000` | 24 KiB |
| OTA metadata | `0xF000` | 8 KiB |
| PHY initialization | `0x11000` | 4 KiB |
| App / OTA slot 0 | `0x20000` | 3 MiB |
| App / OTA slot 1 | `0x320000` | 3 MiB |
| LittleFS web UI | `0x620000` | 1,664 KiB |
| Coredump | `0x7C0000` | 256 KiB |

The final partition ends at `0x800000` (8 MiB). Firmware OTA has two app
slots; the web UI currently uses one LittleFS partition.

From the `firmware` directory, build firmware with `pio run -e pandadesk-c6`
and the filesystem image with `pio run -e pandadesk-c6 -t buildfs`.
Upload firmware with `pio run -e pandadesk-c6 -t upload`, then upload the web
files separately with `pio run -e pandadesk-c6 -t uploadfs`. A firmware upload
alone does not upload the web UI. Both filesystem build and upload derive the
size and offset from `partitions.csv`.

For Jiecang protocol captures, build `pio run -e pandadesk-c6-trace` and upload
that environment, then use `pio device monitor -b 115200`. This diagnostic
build logs transmitted bytes as `JiecangTX`, raw received bytes as `JiecangRXRaw`,
and validated controller frames as `JiecangRX` over USB Serial/JTAG. Controller frames with more than four
parameter bytes are reported as unsupported and are not decoded. Record the
handset's lock/unlock actions alongside the capture before assigning any
meaning to undocumented messages. USB provides logs, not a command shell;
the existing movement commands use HTTP/MQTT. Restore the normal build after
capturing because serial logging can affect UART timing.

At boot and after a web UI update, the firmware mounts only the `littlefs`
partition with type `data/littlefs` at `/webui`. The serial log reports its
actual offset and size (expected: `0x00620000` and `0x001a0000`). Mount failures
are logged without automatically formatting the partition.
After both builds, run `python tests/check_webui_partition.py <path-to-mklittlefs>`
to check the generated partition table, image size, and packed web files.

For an existing checkout with a generated `sdkconfig.pandadesk-c6`, select
8 MB flash using `pio run -e pandadesk-c6 -t menuconfig`; defaults only apply
to settings that are not already saved.

When migrating a board from the old 4 MB layout, flash the bootloader,
partition table, firmware, and newly built LittleFS image over USB/UART.
An application-only OTA update does not install the new partition layout.
Use this layout on the N8 hardware with 8 MB flash.
