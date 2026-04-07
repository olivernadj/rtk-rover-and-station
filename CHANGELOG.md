# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.7.0...HEAD
[0.7.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/olivernadj/rtk-rover-and-station/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/olivernadj/rtk-rover-and-station/releases/tag/v0.1.0
