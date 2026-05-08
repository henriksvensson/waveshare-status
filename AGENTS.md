# Agent Notes

## Project

- Build ESP-IDF firmware for a Waveshare ESP32-S3-LCD-1.69 status display.
- Firmware displays host-provided Murmur/server state received as JSON Lines over USB serial.
- Prefer firmware progress first; host-side sender comes later.

## Hardware

- Board appears as `/dev/ttyACM0` via ESP32-S3 USB serial/JTAG.
- Observed USB identity: `303a:1001 Espressif USB JTAG/serial debug unit`.
- Observed MAC: `a0:85:e3:fb:47:a4`.
- LCD is ST7789/ST7789V2, `240x280`, 4-wire SPI.
- LCD gap is `(0,20)` and color inversion is required.
- Screen has rounded corners; do not place important information in the corners. Keep UI content inside a safe inset.

## LCD Pins

- `DC=GPIO4`
- `CS=GPIO5`
- `SCLK=GPIO6`
- `MOSI=GPIO7`
- `RST=GPIO8`
- `BL=GPIO15`

## Build And Flash

- Use Docker Compose ESP-IDF environment; avoid host toolchain assumptions.
- `idf.py` does not have a true quiet flag in the current image. Use `--no-hints` and redirect noisy output to `/tmp/opencode` when possible.
- Low-noise build pattern:

```bash
docker compose run --rm idf idf.py --no-hints build >/tmp/opencode/waveshare-idf-build.log
```

- Low-noise flash pattern:

```bash
docker compose run --rm idf idf.py --no-hints -p /dev/ttyACM0 flash >/tmp/opencode/waveshare-idf-flash.log
```

- If a quiet command fails, read the saved log instead of rerunning with full output.

## Serial Protocol

- Device accepts one JSON object per line over USB serial.
- Current fields: `service`, `online`, `host`, `ip`, `users`, `max_users`.
- Example:

```json
{"service":"MURMUR","online":true,"host":"homeserver","ip":"192.168.1.42","users":3,"max_users":32}
```

## UI Notes

- Use LVGL for layout and text rendering.
- Design for a small `240x280` screen with rounded unavailable corners.
- Favor large status text and fewer data rows over dense layouts.
- `UNSCII_16` is the preferred readable font on this panel; it fits about 13-14 characters per safe-inset row depending on horizontal inset.
- Current UI is a dark dashboard with service, online/offline state, host, IP, users, and freshness/stale state.

## Current Verification

- Firmware builds in Docker.
- Firmware flashes via `/dev/ttyACM0`.
- A sample JSON status line was received and logged by firmware.
