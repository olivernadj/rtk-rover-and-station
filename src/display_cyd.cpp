#if defined(MODE_ROVER) && defined(BOARD_CYD)

#include "display_cyd.h"
#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <cstdarg>

static TFT_eSPI tft;

static const char* fixName(uint8_t f) {
    switch (f) {
        case 0: return "NO FIX";
        case 2: return "2D";
        case 3: return "3D";
        case 4: return "GNSS+DR";
        case 5: return "TIME";
        default: return "?";
    }
}

static const char* carrName(uint8_t c) {
    switch (c) {
        case 0: return "NONE";
        case 1: return "FLOAT";
        case 2: return "FIXED";
        default: return "?";
    }
}

void CydDisplay::init() {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    tft.init();
    tft.invertDisplay(true);  // this panel powers up INVOFF; flip to show correct colors
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Booting...", 10, 10);
}

// Format `fmt` into `out` and right-pad with spaces to exactly `width` chars.
// Required because drawString's per-glyph bg fill only covers glyphs it draws --
// if a new string is shorter than the previous one at the same position,
// trailing pixels leak through. Padding makes every redraw cover the same area.
static void fmtPadded(char* out, size_t cap, size_t width, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0) { out[0] = '\0'; return; }
    size_t len = (size_t)n;
    while (len < width && len < cap - 1) out[len++] = ' ';
    out[len] = '\0';
}

void CydDisplay::update(const GnssData& data) {
    // Rover loop() runs continuously; pace redraws to 4 Hz. Drawing via
    // setTextColor(fg, bg) + padded strings means each call overwrites in place
    // without a black-flash step.
    static uint32_t _lastDrawMs  = 0;
    static bool     _firstDraw   = true;
    uint32_t now = millis();
    if (now - _lastDrawMs < 250) return;
    _lastDrawMs = now;

    if (_firstDraw) {
        tft.fillScreen(TFT_BLACK);
        _firstDraw = false;
    }

    const bool wifiOk = WiFi.isConnected();
    const bool pvtOk  = data.valid && data.fix_type >= 2;
    char buf[64];

    tft.setTextDatum(TL_DATUM);

    // WiFi label (fixed-width "WiFi OK" / "WiFi ...") + detail (padded to 28)
    tft.setTextFont(4);
    tft.setTextColor(wifiOk ? TFT_GREEN : TFT_RED, TFT_BLACK);
    tft.drawString(wifiOk ? "WiFi OK " : "WiFi ...", 10, 6);

    tft.setTextFont(2);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    if (wifiOk) {
        fmtPadded(buf, sizeof(buf), 28, "%s  %d dBm",
                  WiFi.SSID().c_str(), WiFi.RSSI());
    } else {
        fmtPadded(buf, sizeof(buf), 28, "connecting...");
    }
    tft.drawString(buf, 120, 14);

    // GNSS label
    tft.setTextFont(4);
    tft.setTextColor(pvtOk ? TFT_GREEN : TFT_RED, TFT_BLACK);
    tft.drawString(pvtOk ? "GNSS OK " : "GNSS ...", 10, 40);

    // Fix summary (padded to 44; longest realistic content is ~42 chars)
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    fmtPadded(buf, sizeof(buf), 44, "fix=%s  carr=%s  siv=%u  age=%us",
              fixName(data.fix_type), carrName(data.carr_soln),
              data.siv, data.corr_age);
    tft.drawString(buf, 10, 78);

    // Coord rows (padded to 20 each)
    fmtPadded(buf, sizeof(buf), 20, "lat %.7f", data.lat / 1e7);
    tft.drawString(buf, 10, 102);
    fmtPadded(buf, sizeof(buf), 20, "lon %.7f", data.lon / 1e7);
    tft.drawString(buf, 10, 122);
    fmtPadded(buf, sizeof(buf), 20, "alt %.3f m", data.alt / 1000.0);
    tft.drawString(buf, 10, 142);
}

#endif
