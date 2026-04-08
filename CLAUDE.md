# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

PlatformIO ESP32-S3 firmware for RTK GNSS positioning using a ZED-F9P module. The device operates in two modes:
- **Stationary** — fixed base station, runs survey-in, generates RTCM corrections and pushes them to one or more NTRIP casters simultaneously, and publishes its position via MQTT.
- **Rover** — mobile unit (excavator), receives NTRIP RTK corrections for cm-level accuracy, supports pluggable display drivers.

Both modes: read ZED-F9P over I2C, connect to WiFi, sync NTP, publish JSON metrics to MQTT, and optionally self-update via OTA.

## Build Commands

```bash
pio run -e stationary             # Build stationary firmware
pio run -e rover                  # Build rover firmware
pio run -e stationary -t upload   # Build + flash stationary
pio run -e rover -t upload        # Build + flash rover
pio device monitor --baud 115200  # Serial monitor
```

## Setup

### Secrets

Secrets are gitignored. Copy examples before first build:
```bash
cp src/secrets.h.example src/secrets.h
cp src/secrets.cpp.example src/secrets.cpp
```
`secrets.h` has extern declarations. `secrets.cpp` has the actual values. This split avoids multiple-definition linker errors.

**Stationary secrets** (`#ifdef MODE_STATIONARY`): WiFi credentials, MQTT broker, `NtripCasterConfig NTRIP_CASTERS[]` array (host, port, mountpoint, user, password — add one entry per caster).

**Rover secrets** (`#ifdef MODE_ROVER`): WiFi credentials, MQTT broker, single NTRIP caster fields (`NTRIP_HOST`, `NTRIP_PORT`, `NTRIP_MOUNTPOINT`, `NTRIP_USER`, `NTRIP_PASSWORD`).

**OTA secrets** (`#ifdef OTA_ENABLED`): `OTA_USER` and `OTA_PASSWORD` for HTTP Basic Auth against the OTA server.

### Config

Config is also gitignored (site-specific values like base station coordinates):
```bash
cp src/config.h.example src/config.h
```

## Architecture

The mode is selected at compile time via `build_flags` in `platformio.ini`: `-D MODE_STATIONARY` or `-D MODE_ROVER`. OTA is enabled with `-D OTA_ENABLED`. Mode-specific and OTA code is guarded with `#ifdef` throughout.

**Design pattern:** Cooperative multitasking inside Arduino's `loop()`. Each module exposes a non-blocking `update()` call. Time-sensitive reconnections use `Ticker` one-shot timers.

**Data flow (both modes):**
`gnss.cpp` reads ZED-F9P → `metrics.cpp` formats JSON → `mqtt_manager.cpp` publishes to MQTT.

**Stationary-only flow:**
`gnss.cpp` outputs RTCM → `ntrip_broadcaster.cpp` reads RTCM bytes and streams them to all configured NTRIP casters simultaneously, each via its own `WiFiClient` state machine.

**Rover-only flow:**
`ntrip_client.cpp` fetches RTCM from caster → pushes bytes to ZED-F9P via `gnssGetHandle().pushRawData()` each loop cycle. `gnss.cpp` caches the last RTK-corrected position (when `carr_soln > 0`); `gnssGetData()` returns the cached position with `corr_age` computed as seconds since that fix. Before the first RTK fix, raw data is returned. `display.h` defines an `IDisplay` abstract interface; the default `NullDisplay` no-ops until a concrete driver is added.

**Logging:**
`logger.h` provides `logMsg()` — a `printf`-style function that writes to both Serial and MQTT (`mqtt/logs/v1`). Controlled by `LOG_SERIAL_ENABLED` / `LOG_MQTT_ENABLED` in `config.h`. Cannot be used in `wifi_manager.cpp` or `mqtt_manager.cpp` (circular dependency). OTA messages after WiFi shutdown must use `Serial` directly.

**OTA flow (when enabled):**
`ota_updater.cpp` polls the manifest URL every 5 min over HTTPS with Basic Auth → compares version → downloads `.bin` → streams to flash via internal SRAM IO buffer → shuts down WiFi → verifies image → reboots. See [docs/OTA.md](docs/OTA.md) for full setup instructions including the OTA server.

**Connectivity:** `wifi_manager.cpp` cycles through multiple AP credentials on disconnect. `mqtt_manager.cpp` auto-reconnects when WiFi is available. Both use `Ticker` one-shot timers for reconnection.

**Status LED:** `status_led.cpp` drives the onboard NeoPixel (GPIO 48). Blink burst every 5 s, priority = most severe first:
- Red 2× = GNSS I2C error
- Red 1× = no WiFi
- Yellow 1× = NTP not synced
- Yellow 2× = NTRIP issue (rover: client down or corrections stale >60s; stationary: all casters disconnected or corrections stale >30s)
- Yellow 3× = MQTT down
- Blue 1× = all OK

**Key config:** I2C pins (SDA=8, SCL=9), GPS sample interval, MQTT topic, NTRIP timeout, LED timing, buffer sizes, and OTA URLs are in `src/config.h`.

## Versioning

`CHANGELOG.md` is the single source of truth for the firmware version. The PlatformIO build script `read_version.py` extracts the latest version (first `## [x.y.z]` heading, skipping `[Unreleased]`) and injects it as `-D FW_VERSION="x.y.z"` at compile time. This version appears in MQTT metrics as the `fw_version` label and in the OTA manifest.

When making changes: update `CHANGELOG.md` first, then build.

## Source Files

| File | Mode | Purpose |
|------|------|---------|
| `src/config.h` | both | Compile-time constants (gitignored; copy from `.example`) |
| `src/secrets.h` / `.cpp` | both | Credentials (gitignored; copy from `.example` files) |
| `src/gnss.h` / `.cpp` | both | ZED-F9P I2C driver; stationary also enables RTCM output + survey-in |
| `src/metrics.h` / `.cpp` | both | JSON payload formatter (stack buffer, no heap) |
| `src/wifi_manager.h` / `.cpp` | both | Multi-AP WiFi with Ticker reconnect |
| `src/mqtt_manager.h` / `.cpp` | both | AsyncMqttClient wrapper with Ticker reconnect |
| `src/status_led.h` / `.cpp` | both | NeoPixel blink state machine |
| `src/ntrip_broadcaster.h` / `.cpp` | stationary | Simultaneous multi-caster RTCM broadcaster |
| `src/ntrip_client.h` / `.cpp` | rover | NTRIP correction client |
| `src/display.h` | rover | `IDisplay` abstract interface + `NullDisplay` default |
| `src/logger.h` | both | Header-only dual-backend logging (Serial + MQTT) |
| `src/ota_updater.h` / `.cpp` | both (opt-in) | Poll-based OTA firmware updater (requires `-D OTA_ENABLED`) |
| `src/main.cpp` | both | Setup + cooperative loop |
| `read_version.py` | build | PlatformIO pre-script: injects `FW_VERSION` from `CHANGELOG.md` |

## MQTT Metric Format

Published to `mqtt/metrics/v2` as JSON:
```json
{"metric_type":"gauge","samples":{"lat":"461566930","lat_hp":"42","long":"199614664","long_hp":"-15","alt":"133247","corr_age":"0","siv":"24","fix_type":"3","carr_soln":"2","wifi_rssi":"-52"},"timestamp":1775400000,"client":"rtk-stationary","labels":{"device":"esp32s3-314A2C","mode":"stationary","fw_version":"0.6.0","wifi_ssid":"MyNetwork","project":"GPS"}}
```

Key fields:
- `lat`, `long` — degrees × 10⁻⁷; `lat_hp`, `long_hp` — high-precision digits (× 10⁻⁹, int8)
- `alt` — mm above ellipsoid
- `corr_age` — stationary: seconds since last RTCM push to caster; rover: seconds since last RTK-corrected position (carr_soln > 0)
- `siv`, `fix_type`, `carr_soln` — satellite count, fix type (0/3/5), carrier solution (0=none, 1=float, 2=fixed)
- `wifi_rssi` — WiFi signal strength in dBm
- `device` — MAC-derived hostname (e.g. `esp32s3-314A2C`)
- `fw_version` — from `CHANGELOG.md` via `read_version.py`
- `wifi_ssid` — connected WiFi access point name

## SparkFun GNSS v3 API Notes

- Enable RTCM output: use batched `newCfgValset` / `addCfgValset` / `sendCfgValset` for RTCM message types (not individual `setVal8` — batched is more reliable).
- **Reading RTCM (stationary):** Override the weak `DevUBLOXGNSS::processRTCM(uint8_t incoming)` function to capture RTCM bytes into your own ring buffer. Call `checkUblox()` each loop to pull data from I2C. Do NOT use the library's built-in RTCM buffer (`setRTCMBufferSize` / `rtcmBufferAvailable` / `extractRTCMBufferData`) — it requires `_storageRTCM` to be allocated via `setRTCMLoggingMask()`, which is undocumented and fragile.
- I2C output protocol: must set `COM_TYPE_UBX | COM_TYPE_NMEA | COM_TYPE_RTCM3` — UBX+RTCM3 alone is not a valid combination (per SparkFun docs). Disable NMEA messages individually via valset after enabling the protocol.
- `setStaticPosition(lat, latHp, lon, lonHp, alt, altHp, true)` — note the alternating value/high-precision pairs. Getting the argument order wrong causes silent failure (no RTCM output, no error).
- Push corrections to module (rover): `pushRawData(uint8_t*, size_t)`
- Survey-in: `enableSurveyMode(uint16_t observationTimeSecs, float accuracyMeters)`

## Adding a Display Driver (Rover)

1. Create `src/display_<name>.h` subclassing `IDisplay` from `src/display.h`
2. Implement `void init()` and `void update(const GnssData& data)`
3. In `src/main.cpp`, replace `static NullDisplay _nullDisplay; IDisplay* display = &_nullDisplay;` with an instance of your driver

## Hardware

- Board: ESP32-S3-DevKitC-1 (WROOM1 N16R8)
- GNSS: u-blox ZED-F9P connected via I2C (SDA=GPIO 8, SCL=GPIO 9, 400 kHz)
- Antenna: Multi-band GNSS (L1/L2/L5, GPS/Galileo/GLONASS/BeiDou)
- RGB LED: onboard NeoPixel on GPIO 48 (status indicator, brightness 32/255)
- Libraries: SparkFun u-blox GNSS v3 (≥3.1.13), AsyncMqttClient (≥0.9.0), Adafruit NeoPixel (≥1.12.0)

## Troubleshooting (Lessons Learned)

### WiFi won't connect / cycles through all APs

- A `delay(100)` after `WiFi.disconnect(true)` is required before calling `WiFi.begin()`. Without it, the radio hasn't finished teardown and the new connection fails silently.
- Use a `reconnectScheduled` guard flag to prevent multiple disconnect events from stacking reconnect timers.
- Use `SYSTEM_EVENT_STA_GOT_IP` / `SYSTEM_EVENT_STA_DISCONNECTED` event names and `WiFi.isConnected()` — these are the proven-working variants on ESP32 Arduino.
- NTRIP and MQTT must guard their `update()` with `if (!wifiIsConnected()) return;` — otherwise they attempt DNS lookups and connections before WiFi is up, producing misleading error spam.

### NTRIP broadcaster connects but caster drops the connection

**Wrong protocol:** Emlid (and most casters) expect NTRIP v1 `SOURCE <password> /<mountpoint>` — NOT HTTP POST with Basic Auth (NTRIP v2). The caster silently accepts the TCP connection but never responds to a POST request.

**Wrong password field:** For NTRIP v1 SOURCE, the password is the **server/mount password** from the caster dashboard, not a user:password pair. The `NtripCasterConfig.user` field is unused for SOURCE protocol.

**Test from laptop:** `printf "SOURCE <password> /<mountpoint>\r\nSource-Agent: NTRIP Test\r\n\r\n" | curl -v --max-time 10 telnet://caster.emlid.com:2101` — should get `ICY 200 OK`.

**No RTCM data being sent:** The caster will drop the connection after ~15 seconds if no RTCM data arrives. See the RTCM section below.

### ZED-F9P not producing RTCM data (stationary mode)

This was the hardest issue. Multiple things must be correct simultaneously:

1. **I2C output protocol:** `setI2COutput(COM_TYPE_UBX | COM_TYPE_NMEA | COM_TYPE_RTCM3)` — all three are required. UBX+RTCM3 without NMEA is silently invalid. Disable individual NMEA messages via valset afterwards.

2. **RTCM capture method:** The library's built-in RTCM buffer (`setRTCMBufferSize` / `rtcmBufferAvailable`) does NOT work without also calling `setRTCMLoggingMask()`, which allocates the internal `_storageRTCM` struct. Without it, RTCM frames are silently discarded. **Use the `processRTCM` callback instead** — override the weak `DevUBLOXGNSS::processRTCM(uint8_t)` function and store bytes in your own ring buffer. This is the approach used by SparkFun's own NTRIP server example.

3. **`checkUblox()` must be called:** Call `_gnss.checkUblox()` each loop iteration to read data from I2C. Without it, RTCM data stays in the module and `processRTCM` is never invoked. `getPVT()` alone is not sufficient.

4. **`setStaticPosition` argument order:** The signature is `(lat, latHp, lon, lonHp, alt, altHp, lla)` — alternating value and high-precision pairs. Passing `(lat, lon, alt, 0, 0, altHp, true)` compiles without error but overflows `latHp` (int8_t) and the module silently fails to enter TIME mode, producing zero RTCM.

5. **Satellite visibility:** Even with a correct fixed position, the module needs actual satellite signals to generate RTCM corrections. Indoor operation works if the antenna has partial sky view (24 satellites were visible indoors during testing).

### OTA "Could Not Activate The Firmware" / `invalid segment length` on ESP32-S3 N16R8

**Root cause: DIO SPI flash read bug with Macronix 16MB flash.** The hardware is ESP32-S3-WROOM-1-N16R8 (16MB Macronix flash, 8MB OPI PSRAM), but the Arduino ESP32 v2.x framework (ESP-IDF 4.4) has a bug in the DIO (Dual I/O) SPI flash driver for user-mode reads on this chip. Flash reads via `esp_partition_read()` / `spi_flash_read()` silently wrap at 32-byte boundaries — the first 32 bytes of a read are correct, then they repeat. Flash **writes** are correct (verified via `esptool.py read_flash` raw readback). Normal firmware operation is unaffected because code execution goes through the cache/MMU auto-mode reads, which use a different SPI read path that works correctly.

**Impact on OTA:** `Update.end()` → `esp_ota_end()` → `esp_image_verify()` reads back the firmware from flash using the broken user-mode SPI path, sees corrupted data, and fails with `invalid segment length 0x3e000000`. The bootloader's image verification uses the same broken DIO reads, so even bypassing app-level verification and directly writing otadata doesn't work — the bootloader also fails to verify the new partition and rolls back.

**What was tried and ruled out:**
- `board_build.flash_mode = qio` — the `dio_qspi` pre-compiled SDK ignores this; the image header stays DIO
- `board_build.arduino.memory_type = qio_qspi` or `qio_opi` — changes the SDK to QIO-compiled libraries, but crashes the WiFi driver (`ieee80211_send_setup` dereferences an invalid pointer during power management). Both QIO variants crash identically.
- `Cache_Invalidate_DCache_All()` / `Cache_Invalidate_ICache_All()` before reads — no effect (the issue is in the SPI read command itself, not cache staleness)
- Direct otadata write to bypass `esp_image_verify()` — the write succeeds and the device reboots, but the bootloader's own DIO reads also have the 32-byte wrap, so it fails verification and rolls back to the previous partition

**Current status (2026-04-08):** OTA is blocked on the DIO SPI flash read bug. The most promising fix is upgrading from Arduino ESP32 v2.x (ESP-IDF 4.4, `espressif32 @ 6.13.0`) to v3.x (ESP-IDF 5.x), which has an updated SPI flash driver. This is a major framework upgrade that may require API changes. USB flashing works perfectly and is the current deployment method.

**Board configuration:** A custom board definition `boards/esp32-s3-devkitc-1-n16r8.json` is used with `memory_type: dio_qspi` and `flash_size: 16MB`. This matches the actual hardware better than the stock `esp32-s3-devkitc-1` (which assumes 8MB/no PSRAM), though it does not fix the DIO read bug.

**Previous (incorrect) diagnosis:** Earlier troubleshooting attributed OTA failures to PSRAM cache coherency (WiFi DMA competing with TLS buffers in PSRAM). While removing `-DBOARD_HAS_PSRAM` changed symptoms, the actual root cause was the DIO SPI flash read wrap bug. The PSRAM/WiFi DMA theory was a misdiagnosis.

**What still applies from the original investigation:**

1. **Stream directly to flash — do not buffer the entire firmware.** Without PSRAM, there isn't enough internal heap (~270 KB) to hold a ~1 MB firmware. Stream each chunk directly: `readBytes(ioBuf, n)` → `esp_ota_write(handle, ioBuf, n)`.

2. **Allocate the IO buffer from internal SRAM heap, not the stack.** The ESP32 Arduino loop task has only 8 KB of stack. Use `heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`.

3. **Stop the WiFi reconnect ticker before shutting down WiFi.** `WiFi.disconnect(true)` fires a disconnect event that schedules a reconnect. **Fix:** Call `wifiStopReconnect()` before `WiFi.disconnect()`.

4. **Restore WiFi after a failed OTA.** If `Update.end()` or verification fails after WiFi was shut down, call `wifiResumeReconnect()` to restore connectivity. Without this, WiFi stays permanently dead until reboot.

**Diagnostic approach:** The OTA code computes MD5 of the downloaded byte stream (via `MD5Builder`) and compares against the manifest before any verification. This proves download integrity. A flash write self-test (`esp_partition_write` 64 bytes → `esp_partition_read` → compare) at the start of OTA confirms whether the DIO read wrap bug is present — if the readback shows 32-byte repetition, OTA cannot succeed on this framework version.
