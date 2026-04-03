#include "status_led.h"
#include "config.h"
#include <Adafruit_NeoPixel.h>

static Adafruit_NeoPixel _pixel(1, LED_PIN, NEO_GRB + NEO_KHZ800);

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

static void setPixel(uint32_t color) {
    _pixel.setPixelColor(0, color);
    _pixel.show();
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
    _pixel.begin();
    _pixel.setBrightness(32); // ~12% — avoids blinding and keeps GPIO current safe
    setPixel(COLOR_OFF);
    _phase      = LedPhase::CYCLE_GAP;
    _phaseStart = millis();
}

void ledUpdate(bool wifiOk, bool gnssOk, bool ntpOk, bool mqttOk, bool ntripOk) {
    uint32_t now = millis();

    switch (_phase) {
        case LedPhase::CYCLE_GAP:
            if (now - _phaseStart >= LED_CYCLE_MS) {
                _current    = selectPattern(wifiOk, gnssOk, ntpOk, mqttOk, ntripOk);
                _blinksLeft = _current.count;

                const char* status = "OK";
                if      (!gnssOk)  status = "GNSS error";
                else if (!wifiOk)  status = "WiFi down";
                else if (!ntpOk)   status = "NTP not synced";
                else if (!ntripOk) status = "NTRIP down";
                else if (!mqttOk)  status = "MQTT down";
                Serial.printf("[STATUS] %s (wifi=%d gnss=%d ntp=%d mqtt=%d ntrip=%d)\n",
                              status, wifiOk, gnssOk, ntpOk, mqttOk, ntripOk);

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
