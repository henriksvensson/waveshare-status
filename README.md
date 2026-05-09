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

## Firmware Assets

The firmware commits converted LVGL-ready assets, including RGB565 images and generated Iosevka font sources. Original Iosevka font packages are not committed; download them from the Iosevka releases page when regenerating fonts:

https://github.com/be5invis/Iosevka/releases

Current dashboard font choices:

- header: Iosevka Bold `24px`
- primary status rows: Iosevka Regular `20px`
- user names: Iosevka Regular `16px`
- stale warning: LVGL UNSCII `8px`

Regenerate Iosevka LVGL fonts from downloaded TTC packages with `fontTools` and `lv_font_conv`. `lv_font_conv` cannot read TTC collections directly, so extract the first face to a temporary TTF before conversion:

```bash
python3 -m pip install --target /tmp/opencode/fonttools fonttools

PYTHONPATH=/tmp/opencode/fonttools python3 - <<'PY'
from fontTools.ttLib import TTCollection

fonts = [
    ("firmware/assets/PkgTTC-SGr-Iosevka-34.5.0/SGr-Iosevka-Regular.ttc", "/tmp/opencode/iosevka-regular.ttf"),
    ("firmware/assets/PkgTTC-SGr-Iosevka-34.5.0/SGr-Iosevka-Bold.ttc", "/tmp/opencode/iosevka-bold.ttf"),
]

for source, target in fonts:
    TTCollection(source).fonts[0].save(target)
PY

npx --yes lv_font_conv --no-compress --no-prefilter --bpp 4 --size 16 \
  --font /tmp/opencode/iosevka-regular.ttf -r 0x20-0x7E --format lvgl \
  --lv-include lvgl.h --lv-font-name iosevka_regular_16 \
  -o firmware/main/fonts/iosevka_regular_16.c --force-fast-kern-format

npx --yes lv_font_conv --no-compress --no-prefilter --bpp 4 --size 20 \
  --font /tmp/opencode/iosevka-regular.ttf -r 0x20-0x7E --format lvgl \
  --lv-include lvgl.h --lv-font-name iosevka_regular_20 \
  -o firmware/main/fonts/iosevka_regular_20.c --force-fast-kern-format

npx --yes lv_font_conv --no-compress --no-prefilter --bpp 4 --size 24 \
  --font /tmp/opencode/iosevka-bold.ttf -r 0x20-0x7E --format lvgl \
  --lv-include lvgl.h --lv-font-name iosevka_bold_24 \
  -o firmware/main/fonts/iosevka_bold_24.c --force-fast-kern-format
```

Only generated `firmware/main/fonts/*.c` files are committed. Downloaded `firmware/assets/PkgTTC-*` source packages are ignored by git.

## Host Sender On Alpine/OpenRC

The `host/` directory contains a supervised Alpine/OpenRC sender for the mini-pc. It writes the current Kismet/Murmur status to the attached display every few seconds and keeps a lightweight Mumble client connected so user count/name changes can update the display quickly.

Build the sender locally using Docker:

```bash
docker run --rm -v "$PWD/host/waveshare-status-send:/src" -w /src golang:1.22-alpine \
  sh -c 'gofmt -w main.go && CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -o waveshare-status-send .'
```

Install on the target host:

```bash
doas install -m 0755 host/waveshare-status-send/waveshare-status-send /usr/local/bin/waveshare-status-send
doas install -m 0755 host/waveshare-status.openrc /etc/init.d/waveshare-status
doas rc-update add waveshare-status default
doas rc-service waveshare-status start
```

Install the Kismet power-button and Wi-Fi mode scripts:

```bash
doas install -m 0755 host/kismet/kismet-power-button /usr/local/sbin/kismet-power-button
doas install -m 0755 host/kismet/kismet-power-button-action /usr/local/sbin/kismet-power-button-action
doas install -m 0755 host/kismet/kismet-wifi-ap /usr/local/sbin/kismet-wifi-ap
doas install -m 0755 host/kismet/kismet-wifi-client /usr/local/sbin/kismet-wifi-client
doas install -m 0755 host/kismet/acpi-PWRF-00000080 /etc/acpi/PWRF/00000080
```

The AP/client scripts signal the running sender with `SIGUSR1` after mode changes so the display updates without starting a second Mumble client.

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
- Mumble users: a minimal TLS Mumble client connected to `127.0.0.1:64738`

Run a single update manually:

```bash
doas /usr/local/bin/waveshare-status-send -once
```

## Current Milestone

The firmware logs a startup line and a periodic heartbeat over the ESP32-S3 USB serial/JTAG console.

## Future Direction

A possible next step is to generalize the serial protocol into a small semantic dashboard API. Instead of only rendering one Murmur-specific status payload, the firmware could accept updates for multiple service cards such as Murmur, SSH, web, or backups.

Another Murmur-specific idea is to show who is currently talking in voice chat. The host sender could detect active voice packets or speaking state from its Mumble protocol connection and include a `speaking` or `active_speaker` field in status updates. The display could then highlight that user name or show a short `Speaking: alice` line.

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
