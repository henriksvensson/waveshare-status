# Kismet Host Scripts

These scripts are the host-side Wi-Fi mode and power-button controls installed on the `kismet` mini-pc.

## Scripts

- `acpi-PWRF-00000080`: ACPI power-button hook installed as `/etc/acpi/PWRF/00000080`; calls `/usr/local/sbin/kismet-power-button`.
- `kismet-power-button`: tiny ACPI entrypoint that detaches `kismet-power-button-action` with `setsid` so it survives `acpid` handler cleanup.
- `kismet-power-button-action`: handles power-button press timing. A single press waits `2s` then powers off. A double press toggles between AP and Wi-Fi client mode.
- `kismet-wifi-ap`: switches `wlan0` to AP mode, starts `hostapd` and `dnsmasq`, then refreshes the Waveshare display.
- `kismet-wifi-client`: switches `wlan0` to Wi-Fi client mode, starts `wpa_supplicant`, brings networking back, then refreshes the Waveshare display.

## Display Refresh

The AP/client scripts read `/run/waveshare-status.pid` and send `SIGUSR1` to the running `waveshare-status-send` daemon after a mode change. This asks the existing daemon to send an immediate display update without starting a second Mumble client.

## Install

```bash
doas install -m 0755 host/kismet/kismet-power-button /usr/local/sbin/kismet-power-button
doas install -m 0755 host/kismet/kismet-power-button-action /usr/local/sbin/kismet-power-button-action
doas install -m 0755 host/kismet/kismet-wifi-ap /usr/local/sbin/kismet-wifi-ap
doas install -m 0755 host/kismet/kismet-wifi-client /usr/local/sbin/kismet-wifi-client
doas install -m 0755 host/kismet/acpi-PWRF-00000080 /etc/acpi/PWRF/00000080
```
