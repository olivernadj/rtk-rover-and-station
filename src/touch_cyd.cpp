#if defined(MODE_ROVER) && defined(BOARD_CYD)

#include "touch_cyd.h"
#include <Arduino.h>

// XPT2046 on the CYD lives on its own soft-SPI bus (pins declared in the
// rover-cyd env's build_flags: CLK=25, MOSI=32, MISO=39, CS=33, IRQ=36).
// Max ADC clock is 2 MHz; we bitbang at ~500 kHz which is well inside spec
// and simpler than juggling a third hardware SPI controller on top of the
// display (VSPI) and SD slot (HSPI).

// XPT2046 control-byte layout: 1 S2 S1 S0 MODE SER/DFR PD1 PD0
//   A=0b101 -> Y, A=0b001 -> X, A=0b011 -> Z1 (pressure)
// Mode 0 = 12-bit, SER/DFR = 0 = differential, PD = 00 = power-down between conversions.
static constexpr uint8_t CMD_READ_Y  = 0x90;
static constexpr uint8_t CMD_READ_X  = 0xD0;
static constexpr uint8_t CMD_READ_Z1 = 0xB0;

// Initial calibration for this CYD panel at setRotation(1) (landscape,
// USB-C on the right). Raw ADC values sweep roughly 200..3900 across the
// active area; the touch panel is mounted rotated relative to the LCD
// driver so raw Y maps to screen X and raw X maps to screen Y.
// These constants are a reasonable starting point; serial prints the raw
// values on every tap so they can be refined.
static constexpr int16_t  RAW_X_MIN   =  240;
static constexpr int16_t  RAW_X_MAX   = 3800;
static constexpr int16_t  RAW_Y_MIN   =  240;
static constexpr int16_t  RAW_Y_MAX   = 3800;
static constexpr int16_t  SCREEN_W    =  320;
static constexpr int16_t  SCREEN_H    =  240;

// IRQ considered active-low (XPT2046 drives low while touched).
static constexpr uint16_t Z_PRESS_MIN = 400;   // below = not really pressed; above = good contact

// Gesture thresholds
static constexpr uint32_t TAP_MAX_MS        = 250;   // press up to here -> tap
static constexpr uint32_t LONG_PRESS_MS     = 700;   // still held at/after this -> long-press (fires while pressed)
static constexpr int16_t  MAX_MOVE_PX       = 12;    // if moved beyond this before gesture, cancel

enum class TouchPhase : uint8_t { Idle, Pressed, LongFired };

static TouchPhase _phase         = TouchPhase::Idle;
static uint32_t   _pressStartMs  = 0;
static int16_t    _pressX        = 0;
static int16_t    _pressY        = 0;

// ---- bitbang SPI primitives ------------------------------------------------

static inline void clkLow()  { digitalWrite(CYD_TOUCH_CLK, LOW);  }
static inline void clkHigh() { digitalWrite(CYD_TOUCH_CLK, HIGH); }

static uint16_t xpt2046Read(uint8_t cmd) {
    digitalWrite(CYD_TOUCH_CS, LOW);
    clkLow();

    // Send 8-bit command, MSB first, MOSI sampled on rising edge.
    for (int i = 7; i >= 0; --i) {
        digitalWrite(CYD_TOUCH_MOSI, (cmd >> i) & 1);
        delayMicroseconds(1);
        clkHigh();
        delayMicroseconds(1);
        clkLow();
    }

    // Read 16 bits, keep the middle 12 (XPT2046 returns 12 bits of ADC
    // preceded by a null bit and followed by three trailing zero bits).
    uint16_t raw = 0;
    for (int i = 0; i < 16; ++i) {
        clkHigh();
        delayMicroseconds(1);
        raw = (raw << 1) | (digitalRead(CYD_TOUCH_MISO) ? 1 : 0);
        clkLow();
        delayMicroseconds(1);
    }

    digitalWrite(CYD_TOUCH_CS, HIGH);
    return raw >> 3;   // shift away the trailing zeroes -> 12-bit value
}

static inline bool touchIrqActive() {
    return digitalRead(CYD_TOUCH_IRQ) == LOW;
}

// ---- helpers ---------------------------------------------------------------

static int16_t mapClamped(int16_t v, int16_t inLo, int16_t inHi, int16_t outLo, int16_t outHi) {
    if (inHi == inLo) return outLo;
    int32_t r = (int32_t)(v - inLo) * (outHi - outLo) / (inHi - inLo) + outLo;
    if (r < outLo) r = outLo;
    if (r > outHi) r = outHi;
    return (int16_t)r;
}

// Average a few ADC reads to suppress jitter.
static bool readTouch(int16_t& sx, int16_t& sy) {
    if (!touchIrqActive()) return false;

    // Pressure check: Z1 low means good contact, high means barely touching.
    // XPT2046 Z1 semantics: small Z1 ADC value with large Z2 ADC value -> low resistance -> touched.
    // Simpler: require raw X+Y to be in the usable range as a sanity check.
    int32_t rx = 0, ry = 0;
    constexpr int N = 3;
    for (int i = 0; i < N; ++i) {
        rx += xpt2046Read(CMD_READ_X);
        ry += xpt2046Read(CMD_READ_Y);
    }
    rx /= N; ry /= N;

    if (rx < 100 || ry < 100 || rx > 4000 || ry > 4000) return false;
    if (!touchIrqActive()) return false;   // finger lifted during our reads

    // Map raw ADC -> screen coords. Swap raw axes to match setRotation(1).
    sx = mapClamped((int16_t)ry, RAW_Y_MIN, RAW_Y_MAX, 0, SCREEN_W - 1);
    sy = mapClamped((int16_t)rx, RAW_X_MIN, RAW_X_MAX, 0, SCREEN_H - 1);
    return true;
}

// Hit-test against the A/B/C/D button row. Geometry mirrors
// display_cyd.cpp::drawButtons. The hit-area is padded 5 px in y so
// slightly inaccurate calibration still triggers.
static int8_t buttonAt(int16_t x, int16_t y) {
    constexpr int pad = 4, y0 = 200, h = 34, btnW = 75;
    if (y < y0 - 5 || y > y0 + h + 5) return -1;
    for (int i = 0; i < 4; ++i) {
        int x0 = pad + i * (btnW + pad);
        if (x >= x0 && x < x0 + btnW) return (int8_t)i;
    }
    return -1;
}

// ---- public API ------------------------------------------------------------

void touchInit() {
    pinMode(CYD_TOUCH_CS,   OUTPUT);
    pinMode(CYD_TOUCH_CLK,  OUTPUT);
    pinMode(CYD_TOUCH_MOSI, OUTPUT);
    pinMode(CYD_TOUCH_MISO, INPUT);
    pinMode(CYD_TOUCH_IRQ,  INPUT_PULLUP);
    digitalWrite(CYD_TOUCH_CS,  HIGH);
    digitalWrite(CYD_TOUCH_CLK, LOW);
    digitalWrite(CYD_TOUCH_MOSI, LOW);
    _phase = TouchPhase::Idle;
}

TouchEvent touchPoll() {
    TouchEvent ev{TouchGesture::None, -1, -1, -1};
    uint32_t now = millis();
    bool irq = touchIrqActive();

    switch (_phase) {
        case TouchPhase::Idle: {
            if (irq) {
                int16_t sx, sy;
                if (readTouch(sx, sy)) {
                    _pressStartMs = now;
                    _pressX = sx;
                    _pressY = sy;
                    _phase  = TouchPhase::Pressed;
                    Serial.printf("[TOUCH] down screen=(%d,%d)\n", sx, sy);
                }
            }
            break;
        }

        case TouchPhase::Pressed: {
            if (!irq) {
                // Release: decide tap vs nothing.
                uint32_t dur = now - _pressStartMs;
                if (dur <= TAP_MAX_MS) {
                    int8_t btn = buttonAt(_pressX, _pressY);
                    ev.gesture = TouchGesture::Tap;
                    ev.x = _pressX; ev.y = _pressY; ev.button = btn;
                    Serial.printf("[TOUCH] tap (%d,%d) btn=%d\n", _pressX, _pressY, btn);
                }
                _phase = TouchPhase::Idle;
                break;
            }
            // Still held -- fire long-press once at the threshold.
            if (now - _pressStartMs >= LONG_PRESS_MS) {
                int16_t sx, sy;
                if (readTouch(sx, sy)) {
                    // Cancel if finger wandered too far from the press origin.
                    if (abs(sx - _pressX) <= MAX_MOVE_PX &&
                        abs(sy - _pressY) <= MAX_MOVE_PX) {
                        int8_t btn = buttonAt(_pressX, _pressY);
                        ev.gesture = TouchGesture::LongPress;
                        ev.x = _pressX; ev.y = _pressY; ev.button = btn;
                        Serial.printf("[TOUCH] long  (%d,%d) btn=%d\n", _pressX, _pressY, btn);
                    }
                }
                _phase = TouchPhase::LongFired;
            }
            break;
        }

        case TouchPhase::LongFired: {
            // Wait for release without firing anything else.
            if (!irq) _phase = TouchPhase::Idle;
            break;
        }
    }

    return ev;
}

#endif
