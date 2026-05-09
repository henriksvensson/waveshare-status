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

## Project Layout

```text
firmware/   ESP-IDF firmware
host/       Host-side status sender scripts, added later
```

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
