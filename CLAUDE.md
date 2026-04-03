# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

PlatformIO ESP32-S3 firmware for RTK GNSS positioning using a ZED-F9P module. The device operates in two modes:
- **Stationary** — fixed base station, runs survey-in, generates RTCM corrections and pushes them to one or more NTRIP casters simultaneously, and publishes its position via MQTT.
- **Rover** — mobile unit (excavator), receives NTRIP RTK corrections for cm-level accuracy, supports pluggable display drivers.

Both modes: read ZED-F9P over I2C, connect to WiFi, sync NTP, publish JSON metrics to MQTT.

## Build Commands

```bash
pio run -e stationary             # Build stationary firmware
pio run -e rover                  # Build rover firmware
pio run -e stationary -t upload   # Build + flash stationary
pio run -e rover -t upload        # Build + flash rover
pio device monitor --baud 115200  # Serial monitor
```

## Secrets Setup

Secrets are gitignored. Copy examples before first build:
```bash
cp src/secrets.h.example src/secrets.h
cp src/secrets.cpp.example src/secrets.cpp
```
`secrets.h` has extern declarations. `secrets.cpp` has the actual values. This split avoids multiple-definition linker errors.

**Stationary secrets** (`#ifdef MODE_STATIONARY`): WiFi credentials, MQTT broker, `NtripCasterConfig NTRIP_CASTERS[]` array (host, port, mountpoint, user, password — add one entry per caster).

**Rover secrets** (`#ifdef MODE_ROVER`): WiFi credentials, MQTT broker, single NTRIP caster fields (`NTRIP_HOST`, `NTRIP_PORT`, `NTRIP_MOUNTPOINT`, `NTRIP_USER`, `NTRIP_PASSWORD`).

## Architecture

The mode is selected at compile time via `build_flags` in `platformio.ini`: `-D MODE_STATIONARY` or `-D MODE_ROVER`. Mode-specific code is guarded with `#ifdef MODE_STATIONARY` / `#ifdef MODE_ROVER` throughout.

**Design pattern:** Cooperative multitasking inside Arduino's `loop()`. Each module exposes a non-blocking `update()` call. Time-sensitive reconnections use `Ticker` one-shot timers.

**Data flow (both modes):**
`gnss.cpp` reads ZED-F9P → `metrics.cpp` formats JSON → `mqtt_manager.cpp` publishes to MQTT.

**Stationary-only flow:**
`gnss.cpp` outputs RTCM → `ntrip_broadcaster.cpp` reads RTCM bytes and streams them to all configured NTRIP casters simultaneously, each via its own `WiFiClient` state machine.

**Rover-only flow:**
`ntrip_client.cpp` fetches RTCM from caster → pushes bytes to ZED-F9P via `gnssGetHandle().pushRawData()` each loop cycle. `display.h` defines an `IDisplay` abstract interface; the default `NullDisplay` no-ops until a concrete driver is added.

**Connectivity:** `wifi_manager.cpp` cycles through multiple AP credentials on disconnect. `mqtt_manager.cpp` auto-reconnects when WiFi is available. Both use `Ticker` one-shot timers for reconnection.

**Status LED:** `status_led.cpp` drives the onboard NeoPixel (GPIO 48). Blink burst every 5 s, priority = most severe first:
- Red 2× = GNSS I2C error
- Red 1× = no WiFi
- Yellow 1× = NTP not synced
- Yellow 2× = NTRIP issue (rover: client down; stationary: all casters disconnected)
- Yellow 3× = MQTT down
- Blue 1× = all OK

**Key config:** I2C pins (SDA=8, SCL=9), GPS sample interval, MQTT topic, NTRIP timeout, LED timing, and buffer sizes are in `src/config.h`.

## Source Files

| File | Mode | Purpose |
|------|------|---------|
| `src/config.h` | both | Compile-time constants |
| `src/secrets.h` / `.cpp` | both | Credentials (gitignored; copy from `.example` files) |
| `src/gnss.h` / `.cpp` | both | ZED-F9P I2C driver; stationary also enables RTCM output + survey-in |
| `src/metrics.h` / `.cpp` | both | JSON payload formatter (stack buffer, no heap) |
| `src/wifi_manager.h` / `.cpp` | both | Multi-AP WiFi with Ticker reconnect |
| `src/mqtt_manager.h` / `.cpp` | both | AsyncMqttClient wrapper with Ticker reconnect |
| `src/status_led.h` / `.cpp` | both | NeoPixel blink state machine |
| `src/ntrip_broadcaster.h` / `.cpp` | stationary | Simultaneous multi-caster RTCM broadcaster |
| `src/ntrip_client.h` / `.cpp` | rover | NTRIP correction client |
| `src/display.h` | rover | `IDisplay` abstract interface + `NullDisplay` default |
| `src/main.cpp` | both | Setup + cooperative loop |

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

## MQTT Metric Format

Published to `mqtt/metrics/v2` as JSON:
```json
{"lat":-337654321,"long":1512345678,"alt":123456,"siv":12,"fix_type":3,"carr_soln":2,"device":"rtk-device-1","mode":"stationary"}
```
Fields: `lat`, `long`, `alt` (ZED-F9P raw units: degrees×10⁻⁷, mm above ellipsoid), `siv`, `fix_type`, `carr_soln` (0=none, 1=float RTK, 2=fixed RTK), `device` (hostname), `mode` (stationary/rover).

## Hardware

- Board: ESP32-S3-DevKitC-1
- GNSS: u-blox ZED-F9P connected via I2C (SDA=GPIO 8, SCL=GPIO 9, 400 kHz)
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
