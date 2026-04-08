# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.12.1] - 2026-04-08

### Changed
- Switch NTRIP caster from caster.emlid.com to self-hosted copmonitoring.com:2101

## [0.12.0] - 2026-04-08

### Added
- Health watchdog: auto-reboot when system status is non-OK for >1 minute (`HEALTH_REBOOT_TIMEOUT_MS`)
- `wifiResumeReconnect()` API to restore WiFi after failed OTA
- OTA download MD5 verification before flash — distinguishes download corruption from flash issues
- Custom board definition `esp32-s3-devkitc-1-n16r8` matching actual hardware (16MB flash, 8MB PSRAM)

### Fixed
- WiFi permanently dead after failed OTA verification — now calls `wifiResumeReconnect()` to restore connectivity

## [0.10.0] - 2026-04-07

### Added
- `corr_count` counter in MQTT metrics — increments each time RTK corrections are sent (stationary) or received (rover); Prometheus-compatible monotonic counter
- 1000ms boot delay for power rail stabilization before peripheral init

### Changed
- Status LED log messages (`[STATUS]`) now only print when status flags change, reducing serial/MQTT log noise
- MQTT log topic now includes version, mode, and hostname: `mqtt/logs/v{FW_VERSION}/{mode}/{hostname}` (was `mqtt/logs/v1`)

### Fixed
- MQTT log topic and client ID used default hostname `esp32s3-000001` because `mqttInit()` ran before `wifiInit()` set the MAC-derived hostname; now resolved lazily after WiFi is up
- Rover position cache rejected valid RTK fixes when correction push was younger than 1s; removed the `>= 1000ms` lower bound and simplified to single-timestamp check

## [0.9.0] - 2026-04-07

### Added
- `logger.h` — header-only dual-backend logging (Serial + MQTT `mqtt/logs/v1`), controlled by `LOG_SERIAL_ENABLED` / `LOG_MQTT_ENABLED` in `config.h`
- `wifiStopReconnect()` API to disable WiFi auto-reconnect (used by OTA before flash write)
- `mqttLog()` function for publishing to `mqtt/logs/v1`
- OTA download progress logging every ~100 KB
- OTA stops after 3 consecutive failures for the same firmware MD5, resets when new firmware appears

### Changed
- Migrate `Serial.printf()` to `logMsg()` across gnss, ntrip_broadcaster, status_led, ota_updater, and main — logs now go to both Serial and MQTT
- OTA: stream directly to flash using internal SRAM IO buffer via `heap_caps_malloc(MALLOC_CAP_INTERNAL)`, shut down WiFi before `Update.end()` verification

### Fixed
- OTA image verification failure on ESP32-S3 — WiFi reconnect ticker and active TLS session caused SPI bus contention during `esp_image_verify()` flash read-back; now calls `wifiStopReconnect()`, tears down HTTP/WiFi, and waits before verify
- OTA: WiFi recovers after failed update (only shuts down WiFi on the success path)

## [0.8.0] - 2026-04-07

### Changed
- Rover GNSS cache requires recent RTCM push (1–5 s window) in addition to `carr_soln > 0`, preventing stale cached positions when corrections stop flowing

### Added
- `GPS_ACCEPTABLE_CORR_AGE_MS` config constant (default 5 s) for max correction push age
- `gnssNotifyCorrPush()` to track last two RTCM push timestamps
- OTA download progress logging every ~100KB

### Fixed
- OTA "Image hash failed" — verify and finalise flash before TLS teardown to avoid SPI bus contention during `esp_image_verify()` read-back; reduce write buffer to 2 KB and increase RTOS yield
- OTA download corruption — replaced unreliable `writeStream()` with chunked reads and 30s inactivity timeout for reliable large transfers over TLS
- OTA retries infinitely on persistent failure — stops after 3 consecutive failures for the same firmware MD5, resets when new firmware is uploaded

## [0.7.0] - 2026-04-07

### Changed
- OTA update check compares manifest `version` field against compiled-in `FW_VERSION` instead of MD5 — `ESP.getSketchMD5()` differs from file-level MD5 due to ESP32 image format
- OTA check interval reduced to 1 minute

### Fixed
- NTRIP client sent `Connection: close` header, causing the caster to drop the RTCM stream and triggering repeated reconnect cycles with RTK loss
- Socket leak in NTRIP client reconnection path causing ESP32 crashes every ~20 minutes
- Missing NTRIP cleanup on WiFi disconnect leaving stale sockets
- OTA flash corruption ("Image hash failed") — disconnect MQTT and NTRIP before OTA download to prevent AsyncMqttClient TCP task from interfering with flash writes
- Duplicate `[WIFI] Disconnected` log spam during AP cycling — only log and act on the first disconnect event per reconnect cycle

## [0.6.0] - 2026-04-06

### Added
- WiFi SSID (`wifi_ssid`) label and signal strength (`wifi_rssi`) sample in MQTT metrics payload
- Device hostname in User-Agent for NTRIP and OTA HTTP clients

### Changed
- Rover caches last RTK-corrected position (carr_soln > 0) and keeps publishing it when corrections stop
- Rover `corr_age` now represents seconds since last RTK-quality fix, not raw ZED-F9P correction age

### Fixed
- Stationary `corr_age` spiking to 65535 on startup before first RTCM push

## [0.5.0] - 2026-04-05

### Added
- Basic authentication on OTA server to protect firmware binaries containing credentials
- OTA client sends auth credentials from secrets.h/secrets.cpp

## [0.4.0] - 2026-04-05

### Added
- Firmware version (`fw_version`) label in MQTT metrics payload, read from CHANGELOG.md at build time
- OTA (Over-The-Air) firmware update support, opt-in via `-D OTA_ENABLED` build flag
- Device polls a manifest URL every 5 minutes, compares MD5, and auto-updates

## [0.3.0] - 2026-04-05

### Added
- Correction age metric (`corr_age`) in MQTT JSON payload
- LED warning: yellow 2x blink when RTCM corrections are stale (>30s for stationary, >60s for rover)

## [0.2.0] - 2026-04-05

### Added
- High-resolution latitude/longitude fields (`lat_hr`, `lon_hr`) in MQTT payload (degrees x10^-9)

### Fixed
- Device hostname now derived from MAC address (`esp32s3-XXYYZZ`) instead of default

## [0.1.0] - 2026-04-03

### Added
- ESP32-S3 firmware for ZED-F9P RTK positioning with two compile-time modes: stationary base station and rover
- Stationary mode: survey-in or fixed position, RTCM output to multiple NTRIP casters simultaneously
- Rover mode: NTRIP correction client, pluggable display interface (`IDisplay`)
- Multi-AP WiFi manager with automatic reconnection
- MQTT metrics publishing (JSON to `mqtt/metrics/v2`)
- NeoPixel status LED with priority-based blink patterns
- NTP time synchronisation

[Unreleased]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.12.0...HEAD
[0.12.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.10.0...v0.12.0
[0.10.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.9.0...v0.10.0
[0.9.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.8.0...v0.9.0
[0.8.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/olivernadj/rtk-rover-and-station/releases/tag/v0.1.0
