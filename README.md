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
