#include "status_led.h"
#include "config.h"
#include "logger.h"
#ifndef BOARD_CYD
#include <Adafruit_NeoPixel.h>
static Adafruit_NeoPixel _pixel(1, LED_PIN, NEO_GRB + NEO_KHZ800);
#else
#include <Arduino.h>
#endif

static constexpr uint32_t COLOR_OFF    = 0x000000;
static constexpr uint32_t COLOR_RED    = 0xFF0000;
static constexpr uint32_t COLOR_YELLOW = 0xFFAA00;
static constexpr uint32_t COLOR_BLUE   = 0x0000FF;

struct BlinkPattern {
    uint32_t color;
    uint8_t  count;
};

enum class LedPhase { CYCLE_GAP, BLINK_ON, BLINK_OFF, POST_BLINK_GAP };

static LedPhase    _phase      = LedPhase::CYCLE_GAP;
static uint8_t     _blinksLeft = 0;
static uint32_t    _phaseStart = 0;
static BlinkPattern _current   = {COLOR_BLUE, 1};

// Previous status flags for change detection (initialised to impossible values to force first log)
static int8_t _prevWifi = -1, _prevGnss = -1, _prevNtp = -1, _prevMqtt = -1, _prevNtrip = -1;

// Tracks the last time status was "OK" — used for reboot watchdog
static uint32_t _lastOkMs = 0;

static void setPixel(uint32_t color) {
#ifdef BOARD_CYD
    // Common-anode RGB LED, active LOW. Threshold each channel on/off;
    // the palette only uses pure-channel mixes so binary output is sufficient.
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >>  8) & 0xFF;
    uint8_t b = (color      ) & 0xFF;
    digitalWrite(LED_R_PIN, r ? LOW : HIGH);
    digitalWrite(LED_G_PIN, g ? LOW : HIGH);
    digitalWrite(LED_B_PIN, b ? LOW : HIGH);
#else
    _pixel.setPixelColor(0, color);
    _pixel.show();
#endif
}

// Priority: most severe first.
static BlinkPattern selectPattern(bool wifiOk, bool gnssOk, bool ntpOk,
                                   bool mqttOk, bool ntripOk) {
    if (!gnssOk)  return {COLOR_RED,    2};  // GNSS I2C error
    if (!wifiOk)  return {COLOR_RED,    1};  // no WiFi
    if (!ntpOk)   return {COLOR_YELLOW, 1};  // NTP not synced
    if (!ntripOk) return {COLOR_YELLOW, 2};  // NTRIP down (rover client / stationary broadcaster)
    if (!mqttOk)  return {COLOR_YELLOW, 3};  // MQTT down
    return              {COLOR_BLUE,   1};   // all OK
}

void ledInit() {
#ifdef BOARD_CYD
    pinMode(LED_R_PIN, OUTPUT);
    pinMode(LED_G_PIN, OUTPUT);
    pinMode(LED_B_PIN, OUTPUT);
#else
    _pixel.begin();
    _pixel.setBrightness(32); // ~12% — avoids blinding and keeps GPIO current safe
#endif
    setPixel(COLOR_OFF);
    _phase      = LedPhase::CYCLE_GAP;
    _phaseStart = millis();
    _lastOkMs   = millis();
}

void ledUpdate(bool wifiOk, bool gnssOk, bool ntpOk, bool mqttOk, bool ntripOk) {
    uint32_t now = millis();

    switch (_phase) {
        case LedPhase::CYCLE_GAP:
            if (now - _phaseStart >= LED_CYCLE_MS) {
                _current    = selectPattern(wifiOk, gnssOk, ntpOk, mqttOk, ntripOk);
                _blinksLeft = _current.count;

                if (_current.color == COLOR_BLUE) {
                    _lastOkMs = now;
                }

                if (wifiOk != _prevWifi || gnssOk != _prevGnss || ntpOk != _prevNtp ||
                    mqttOk != _prevMqtt || ntripOk != _prevNtrip) {
                    const char* status = "OK";
                    if      (!gnssOk)  status = "GNSS error";
                    else if (!wifiOk)  status = "WiFi down";
                    else if (!ntpOk)   status = "NTP not synced";
                    else if (!ntripOk) status = "NTRIP down";
                    else if (!mqttOk)  status = "MQTT down";
                    logMsg("[STATUS] %s (wifi=%d gnss=%d ntp=%d mqtt=%d ntrip=%d)",
                           status, wifiOk, gnssOk, ntpOk, mqttOk, ntripOk);
                    _prevWifi = wifiOk; _prevGnss = gnssOk; _prevNtp = ntpOk;
                    _prevMqtt = mqttOk; _prevNtrip = ntripOk;
                }

                setPixel(_current.color);
                _phase      = LedPhase::BLINK_ON;
                _phaseStart = now;
            }
            break;

        case LedPhase::BLINK_ON:
            if (now - _phaseStart >= LED_BLINK_ON_MS) {
                setPixel(COLOR_OFF);
                _blinksLeft--;
                _phase      = (_blinksLeft > 0) ? LedPhase::BLINK_OFF
                                                 : LedPhase::POST_BLINK_GAP;
                _phaseStart = now;
            }
            break;

        case LedPhase::BLINK_OFF:
            if (now - _phaseStart >= LED_BLINK_OFF_MS) {
                setPixel(_current.color);
                _phase      = LedPhase::BLINK_ON;
                _phaseStart = now;
            }
            break;

        case LedPhase::POST_BLINK_GAP:
            // Stay off until the 5-second cycle gap elapses.
            if (now - _phaseStart >= LED_BLINK_OFF_MS * 2) {
                _phase      = LedPhase::CYCLE_GAP;
                _phaseStart = now;
            }
            break;
    }
}

bool healthCheckReboot() {
    return (millis() - _lastOkMs) >= HEALTH_REBOOT_TIMEOUT_MS;
}
