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

- Enable RTCM output: `setVal8(UBLOX_CFG_MSGOUT_RTCM_3X_TYPE1005_I2C, rate)` (not v2's `enableRTCMmessage`)
- Read RTCM buffer: `setRTCMBufferSize(512)` during init, then `rtcmBufferAvailable()` / `extractRTCMBufferData(buf, n)`
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
