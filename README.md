# Waveshare Status Display

Status display firmware for a Waveshare ESP32-S3 LCD board, intended to show host-provided Murmur/server state over USB serial.

## Build

```bash
docker compose run --rm idf idf.py build
```

## Flash And Monitor

The board currently appears as `/dev/ttyACM0`.

```bash
docker compose run --rm idf idf.py -p /dev/ttyACM0 flash monitor
```

## Send Status Over Serial

The firmware accepts one JSON object per line on `/dev/ttyACM0`. After flashing, sending status does not require Docker if your user has serial-device permissions.

On Linux, add your user to `dialout` if direct writes fail with `Permission denied`:

```bash
sudo usermod -aG dialout "$USER"
```

Log out and back in, or reboot, then verify `dialout` appears in:

```bash
groups
```

Send a status line directly:

```bash
printf '%s\n' '{"service":"MURMUR","mode":"client","wifi":"StatusNet","ip":"192.168.1.42","users":3,"user_names":["alice","bob","charlie"]}' > /dev/ttyACM0
```

For repeated sends, a JSON Lines file also works:

```bash
cat status.jsonl > /dev/ttyACM0
```

## Project Layout

```text
firmware/   ESP-IDF firmware
host/       Host-side status sender and OpenRC service files
```

## Host Sender On Alpine/OpenRC

The `host/` directory contains a supervised Alpine/OpenRC sender for the mini-pc. It writes the current Kismet/Murmur status to the attached display every few seconds.

Install on the target host:

```bash
doas install -m 0755 host/waveshare-status-send /usr/local/bin/waveshare-status-send
doas install -m 0755 host/waveshare-status.openrc /etc/init.d/waveshare-status
doas rc-update add waveshare-status default
doas rc-service waveshare-status start
```

Check status:

```bash
rc-service waveshare-status status
```

The sender detects:

- serial port: `/dev/serial/by-id/*Espressif*`, `/dev/serial/by-id/*JTAG*`, `/dev/serial/by-id/*serial*`, then `/dev/ttyACM0`
- AP/client mode: `rc-service hostapd status`
- AP SSID: `/etc/hostapd/kismet-ap.conf`
- client SSID: `iw dev wlan0 link`
- IP address: `ip -4 -o addr show dev wlan0`

Run a single update manually:

```bash
doas /usr/local/bin/waveshare-status-send --once
```

The first version sends `users: 0` and an empty `user_names` list. Real uMurmur user detection can be added later.

## Current Milestone

The firmware logs a startup line and a periodic heartbeat over the ESP32-S3 USB serial/JTAG console.

## Future Direction

A possible next step is to generalize the serial protocol into a small semantic dashboard API. Instead of only rendering one Murmur-specific status payload, the firmware could accept updates for multiple service cards such as Murmur, SSH, web, or backups.

Each service update would be an upsert-style JSON object with bounded semantic fields, for example:

```json
{
  "type": "service",
  "id": "murmur",
  "title": "MURMUR",
  "state": "ok",
  "lines": [
    {"text": "StatusNet", "kind": "network", "marquee": true},
    {"text": "192.168.1.42", "kind": "address", "marquee": true},
    {"text": "Users: 3", "kind": "metric"},
    {"text": "alice, bob, charlie", "kind": "detail", "size": "small"}
  ],
  "rotate_ms": 5000,
  "stale_ms": 30000
}
```

The firmware should still own layout, colors, fonts, and safe-area behavior. Payloads should describe semantics and limited presentation hints rather than raw coordinates or arbitrary colors. A first version could support a small fixed number of service cards, rotate between active cards, and show a stale warning when a card has not been updated within its configured timeout.
