# RTK Rover and Station

PlatformIO firmware for ESP32-S3 + u-blox ZED-F9P providing centimeter-level GNSS positioning. One codebase, two compile-time modes:

- **Stationary (Base Station)** -- uses a known fixed position (or survey-in) to generate RTCM corrections broadcast to one or more NTRIP casters simultaneously, while publishing position metrics via MQTT.
- **Rover (Mobile Unit)** -- receives NTRIP RTK corrections and feeds them to the ZED-F9P for cm-level accuracy. Supports pluggable display drivers.

Both modes share: ZED-F9P I2C driver, multi-AP WiFi manager, NTP sync, MQTT telemetry, and NeoPixel status LED.

## Hardware

| Component | Detail |
|-----------|--------|
| MCU | ESP32-S3-DevKitC-1 |
| GNSS | u-blox ZED-F9P via I2C (SDA=GPIO 8, SCL=GPIO 9, 400 kHz) |
| Status LED | Onboard NeoPixel on GPIO 48 |

## Quick Start

### 1. Clone and configure secrets

```bash
git clone https://github.com/olivernadj/rtk-rover-and-station.git
cd rtk-rover-and-station
cp src/secrets.h.example src/secrets.h
cp src/secrets.cpp.example src/secrets.cpp
```

Edit `src/secrets.cpp` with your WiFi credentials, MQTT broker, and NTRIP caster details.

### 2. Build and flash

```bash
pio run -e stationary -t upload   # Base station
pio run -e rover -t upload        # Rover
pio device monitor --baud 115200  # Serial monitor
```

## Architecture

Mode is selected at compile time via `build_flags` in `platformio.ini` (`-D MODE_STATIONARY` or `-D MODE_ROVER`). Mode-specific code is guarded with `#ifdef`.

**Design pattern:** Cooperative multitasking in Arduino `loop()`. Each module exposes a non-blocking `update()` call. Reconnections use `Ticker` one-shot timers.

### Data flow

```
                    Both modes
                    ----------
ZED-F9P (I2C) --> gnss.cpp --> metrics.cpp --> mqtt_manager.cpp --> MQTT broker

                    Stationary only
                    ----------------
ZED-F9P (RTCM) --> gnss.cpp (processRTCM callback) --> ntrip_broadcaster.cpp --> NTRIP caster(s)

                    Rover only
                    ----------
NTRIP caster --> ntrip_client.cpp --> ZED-F9P (pushRawData)
```

### Source files

| File | Mode | Purpose |
|------|------|---------|
| `main.cpp` | both | Setup + cooperative loop |
| `config.h` | both | Compile-time constants |
| `secrets.h` / `.cpp` | both | Credentials (gitignored) |
| `gnss.h` / `.cpp` | both | ZED-F9P I2C driver; stationary enables RTCM output via `processRTCM` callback |
| `metrics.h` / `.cpp` | both | JSON payload formatter (stack buffer, no heap) |
| `wifi_manager.h` / `.cpp` | both | Multi-AP WiFi with Ticker reconnect |
| `mqtt_manager.h` / `.cpp` | both | AsyncMqttClient wrapper with Ticker reconnect |
| `status_led.h` / `.cpp` | both | NeoPixel blink state machine |
| `ntrip_broadcaster.h` / `.cpp` | stationary | NTRIP v1 SOURCE protocol, simultaneous multi-caster RTCM broadcast |
| `ntrip_client.h` / `.cpp` | rover | NTRIP correction client |
| `display.h` | rover | `IDisplay` abstract interface + `NullDisplay` default |

## Configuration

All compile-time constants live in `src/config.h`: I2C pins, GPS sample interval, MQTT topic, NTRIP timeouts, LED timing, buffer sizes, and fixed base station coordinates.

### Secrets

`secrets.h` declares extern symbols; `secrets.cpp` defines the values. This split avoids multiple-definition linker errors.

**Stationary secrets:** WiFi credentials, MQTT broker, `NtripCasterConfig NTRIP_CASTERS[]` array (host, port, mountpoint, user, password -- one entry per caster).

**Rover secrets:** WiFi credentials, MQTT broker, single NTRIP caster fields.

### Fixed position vs. survey-in

Set `USE_FIXED_POSITION = true` in `config.h` to skip survey-in and use known coordinates (recommended for permanent installations). Coordinates use ZED-F9P raw units: lat/lon in degrees x 10^-7, altitude in mm above ellipsoid.

Example (Stonehenge):
```cpp
constexpr int32_t  FIXED_LAT     = 511788630;   // 51.1788630°
constexpr int32_t  FIXED_LON     = -18262170;   // -1.8262170°
constexpr int32_t  FIXED_ALT_MM  = 102000;      // 102.000 m
```

## Status LED

Blink burst every 5 seconds, highest priority first:

| Pattern | Meaning |
|---------|---------|
| Red 2x | GNSS I2C error |
| Red 1x | WiFi disconnected |
| Yellow 1x | NTP not synced |
| Yellow 2x | NTRIP down |
| Yellow 3x | MQTT down |
| Blue 1x | All systems OK |

Status is also printed to serial every 5 seconds:
```
[STATUS] OK (wifi=1 gnss=1 ntp=1 mqtt=1 ntrip=1)
```

## MQTT Metrics

Published to `mqtt/metrics/v2` as JSON:

```json
{"lat":511788630,"long":-18262170,"alt":102000,"siv":24,"fix_type":5,"carr_soln":0,"device":"esp32s3-74696F","mode":"stationary"}
```

| Field | Unit | Description |
|-------|------|-------------|
| `lat`, `long` | degrees x 10^-7 | e.g. 511788630 = 51.1788630 degrees |
| `alt` | mm above ellipsoid | e.g. 102000 = 102.000 m |
| `siv` | count | Satellites in view |
| `fix_type` | enum | 0=none, 3=3D, 5=time-only |
| `carr_soln` | enum | 0=none, 1=float RTK, 2=fixed RTK |
| `device` | string | Hostname |
| `mode` | string | "stationary" or "rover" |

## Adding a Display Driver (Rover)

1. Create `src/display_<name>.h` subclassing `IDisplay` from `src/display.h`
2. Implement `void init()` and `void update(const GnssData& data)`
3. In `src/main.cpp`, replace `NullDisplay` with your driver instance

## Dependencies

| Library | Version |
|---------|---------|
| [SparkFun u-blox GNSS v3](https://github.com/sparkfun/SparkFun_u-blox_GNSS_v3) | >= 3.1.13 |
| [AsyncMqttClient](https://github.com/marvinroger/async-mqtt-client) | >= 0.9.0 |
| [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel) | >= 1.12.0 |

## License

MIT
