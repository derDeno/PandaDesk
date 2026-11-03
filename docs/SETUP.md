# Setup and network access

[Documentation](README.md) · [PandaDesk](../README.md)

## Before you start

Check [hardware compatibility, wiring, and voltage limits](HARDWARE.md) for your
exact controller/handset pair. Connect only one desk/interface pair at a time.
For inline operation, connect the handset to **IN** and the controller to **OUT**;
a supported accessory-port installation uses **OUT** only. PandaDesk must remain
powered for an inline handset to work. Use USB-C if desk power is unknown or insufficient.

If the board has not been flashed yet, install both the firmware and web UI using
the [build and flashing guide](BUILDING.md#build-and-install-over-usb).

## Connect to Wi-Fi

1. Power PandaDesk and join its open **PandaDesk-XXXX** Wi-Fi network.
2. Open the sign-in/setup window offered by your phone or computer. If it does
   not appear, open the access point's gateway address in a browser using HTTP;
   the USB serial log also prints the exact portal URL.
3. Go to **Settings → Wi-Fi setup**, enter your network name and password, and
   select **Save and reboot**. A blank password selects an open network; saved
   passwords are never displayed, so enter the password again when saving.
4. Reconnect your phone or computer to the same local network as PandaDesk.
5. Open `http://pandadesk.local`. If that name is taken, use
   `http://pandadesk-xxxx.local` with your board's four-digit suffix, or find its
   IP address in your router's client list under **PandaDesk-XXXX**.

## Configure your desk

1. Open **Settings → Desk settings**.
2. Select the matching **Desk profile** and **Desk model**. Entries marked
   unqualified or not verified still need hardware validation; a connector or
   brand name alone does not prove compatibility.
3. Enter your desk's minimum and maximum heights in cm, with one decimal place.
4. Select **Save desk settings**, then **Restart device** to apply the profile.
5. Return to **Home**, wait for the desk controller to respond, and check its
   status before using the controls. Available actions depend on the selected
   profile and the controller data PandaDesk has received.

Continue with [using the web UI](WEBUI.md), the [Stream Deck plugin](STREAMDECK.md),
or [Home Assistant / MQTT](INTEGRATIONS.md).

## Connection troubleshooting

- If the setup network returns, PandaDesk could not connect or reconnect to
  the saved Wi-Fi network. Check the network name, password, and router availability.
- If the `.local` address does not resolve, use the device IP address. The fallback
  name and DHCP name are automatic and appear under **Settings → Device settings**.
- If controls remain disabled, check the selected profile/model, restart after
  changing desk settings, and check desk power and cabling.
- If no web UI loads after flashing, upload the filesystem image too. A firmware
  upload alone does not include the browser interface.

## Setup access point and captive portal

The DHCP hostname is always `PandaDesk-XXXX`, using the final four uppercase
hexadecimal digits of the station MAC, exactly like the setup AP's SSID.
It is set before Wi-Fi connects, so DHCP servers/network controllers can learn
it from the first lease request. Home Assistant also uses this stable device name.

mDNS first checks and registers `pandadesk.local`. If the name is already in use,
it registers `pandadesk-xxxx.local`, with the same four MAC digits in lowercase.
The mDNS probe also handles simultaneous starts: if it detects a later collision,
the Wi-Fi task selects the MAC suffix. If even that fallback collides, mDNS stops
and logs the conflict; the web UI remains accessible by IP. DHCP and AP names
stay fixed throughout. The device settings UI displays both automatic names;
previously saved custom hostnames no longer override them.

After installing the firmware, reboot/reconnect to obtain a new DHCP lease and
check the reported hostname in UniFi. A controller's saved device alias or old
client entry may need to be updated separately; the firmware advertises its
hostname but cannot force the controller's display name.

Without saved Wi-Fi credentials, or when connecting/reconnecting times out,
PandaDesk opens the `PandaDesk-XXXX` setup network. DHCP advertises the AP itself
as DNS server. Its DNS server answers IPv4 lookups with the AP address, and HTTP
connectivity probes (such as Android `/generate_204`, Apple `/hotspot-detect.html`,
and Windows `/connecttest.txt`) redirect to the web UI at the AP's root URL.
The same portal runs when an existing station connection falls back to AP mode.
DNS listens only on the AP address; station-mode HTTP routing stays unchanged.

After connecting, clients with captive-portal detection enabled should open
the setup window or show a "Sign in to Wi-Fi" notification. Automatic opening
depends on the client's OS and settings; HTTPS requests cannot be redirected.
The serial log reports the portal URL and DNS startup failures.

Host checks (C++17 compiler required), from `firmware/`:

```sh
clang++ -std=c++17 -Iinclude tests/captive_dns_test.cpp -o captive_dns_test.exe
./captive_dns_test.exe
python tests/check_hostname.py
```

Hardware check: forget the setup network on Android, iOS and Windows, reconnect,
and verify that the setup UI opens or is offered without entering an IP address.
Repeat after disconnecting the saved router to exercise AP fallback. Verify that
Wi-Fi settings can still be saved inside the captive window and that the device
then reboots into station mode.
