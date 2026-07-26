# InkSight for Waveshare ESP32-S3-RLCD-4.2

This is a pure ESP-IDF 5.5 port for the Waveshare
ESP32-S3-RLCD-4.2. It does not use Arduino or PlatformIO.

The first version includes:

- native ST7305 SPI initialization and 400×300 landscape rendering;
- open provisioning AP and a setup page at `http://192.168.4.1`;
- NVS-backed Wi-Fi, backend URL, device token, and refresh interval;
- `https://web.inksight.site` prefilled as the default backend while
  retaining support for self-hosted URLs;
- InkSight `/api/device/{mac}/token` and `/api/render` integration;
- 1-, 8-, 24-, and 32-bit uncompressed BMP decoding;
- battery measurement, KEY wake/next mode, and timer deep sleep;
- ST7305 low-power scan mode while the ESP32-S3 sleeps.

Audio, RTC, SHTC3, microSD, OTA, and live voice features are not part
of this first hardware-validation version.

## Automated release

Changes to this ESP-IDF project on `main` automatically update the
[latest ESP32-S3-RLCD4.2 prerelease](https://github.com/wickenzh/inksight/releases/tag/esp32-s3-rlcd4.2-latest).
The release provides a merged image for flashing at address `0x0`, a
package of individual images, and SHA-256 checksums. See
[`RELEASE_FLASHING.md`](RELEASE_FLASHING.md) for flashing instructions.

## Build and flash

Install and activate ESP-IDF 5.5, then run from the repository root:

```bash
idf.py -C firmware/esp-idf/ESP32-S3-RLCD4.2 set-target esp32s3
idf.py -C firmware/esp-idf/ESP32-S3-RLCD4.2 build
idf.py -C firmware/esp-idf/ESP32-S3-RLCD4.2 -p /dev/cu.usbmodemXXXX flash monitor
```

On first boot, connect to the `InkSight-XXXXXX` Wi-Fi network and open
`http://192.168.4.1`. On a normal boot, holding KEY for about half a
second opens setup. A short KEY press while sleeping wakes the board and
requests the next InkSight mode; keeping it held for about 1.5 seconds
opens setup instead.

Pin mappings are centralized in `main/board_config.h`.

## Focus listening / alert feature

This fork adds **focus listening mode**, which keeps the device awake and
periodically polls for alerts pushed via the InkSight API.

### How it works

1. On boot, after rendering content, the device fetches
   `GET /api/config/{mac}` and checks the `is_focus_listening` flag.
2. If focus listening is **enabled**, the device enters a main loop instead
   of deep sleep:
   - Polls `GET /api/device/{mac}/alert-bmp` every **10 seconds**
   - If an alert exists (HTTP 200), saves the current frame, displays the
     alert for **30 seconds**, then restores the previous content
   - The physical button on the device can be pressed to dismiss the alert
     early
   - Re-checks the `is_focus_listening` flag every **60 seconds**
3. If focus listening is **disabled**, the device deep sleeps as normal.

### Alert display

The server renders the alert as a 400×300 1-bit BMP with Chinese text
("紧急告警" / "FOCUS ALERT"), including the sender name and message.
The BMP is decoded by the existing `backend_decode_bmp()` and displayed
on the ST7305 via `st7305_display()`.

### Files changed

| File | Change |
|------|--------|
| `main/backend.h` | Added `backend_fetch_focus_config()` and
  `backend_fetch_alert_bmp()` declarations |
| `main/backend.c` | Implemented the two functions above; JSON parsing with
  cJSON for the config endpoint; BMP decoding for the alert endpoint |
| `main/app_main.c` | Replaced flat execution with a focus loop; added
  `force` parameter to `enter_deep_sleep()` |

### Triggering an alert from an external agent

Push an alert via the InkSight API:

```bash
curl -X POST "https://www.inksight.site/api/device/{mac}/alert" \
  -H "Content-Type: application/json" \
  -H "X-Agent-Token: <your_agent_token>" \
  -d '{"sender":"pi","message":"测试消息","level":"critical"}'
```

Or use the pi extension included in this repo's `~/.pi/agent/extensions/
inksight-alert/`:

```
/inksight set https://www.inksight.site <mac> <token>
/inksight-alert 测试成功
```

### Limitations

- The alert is **visual only** — the RLCD board has no speaker/buzzer.
- Focus listening prevents deep sleep, increasing power consumption.
  The ST7305 low-power scan mode remains active during the loop.
- The `always_active` config flag also keeps the device awake but does
  **not** enable alert polling (only `focus_listening` does).
