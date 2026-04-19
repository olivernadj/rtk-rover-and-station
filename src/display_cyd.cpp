#if defined(MODE_ROVER) && defined(BOARD_CYD)

#include "display_cyd.h"
#include "display_cyd_palette.h"
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

// ---- 4-bit sprite with custom palette --------------------------------------
//
// ESP32-WROOM-32 max contiguous DRAM is ~110 KB, so a 320x240x16bpp sprite
// (153 KB) won't fit. Instead we use a 4-bit sprite with a 16-entry custom
// palette: 320*240/2 = 38 KB, fits comfortably and gives us one atomic SPI
// burst per frame. pushSprite expands the 4-bit indices through the palette
// to RGB565 as it transfers to the panel.
//
// All drawing calls on `frame` take palette INDICES (0..15) as the color
// argument, not RGB565 values.

static TFT_eSPI    tft;
static TFT_eSprite frame(&tft);
static bool        _spriteReady = false;

static constexpr int W = 320;
static constexpr int H = 240;

// Palette indices -- order doesn't matter, but must match `palette16[]` below.
enum : uint8_t {
    PI_BG         = 0,
    PI_SURFACE    = 1,
    PI_BORDER     = 2,
    PI_TEXT       = 3,
    PI_TEXT_DIM   = 4,
    PI_ACCENT     = 5,
    PI_OK_GREEN   = 6,
    PI_WARN_AMBER = 7,
    PI_BAD_RED    = 8,
    PI_IDLE_GRAY  = 9,
    PI_BLACK      = 10,
};

static uint16_t palette16[16] = {
    palette::BG,         // 0
    palette::SURFACE,    // 1
    palette::BORDER,     // 2
    palette::TEXT,       // 3
    palette::TEXT_DIM,   // 4
    palette::ACCENT,     // 5
    palette::OK_GREEN,   // 6
    palette::WARN_AMBER, // 7
    palette::BAD_RED,    // 8
    palette::IDLE_GRAY,  // 9
    TFT_BLACK,           // 10 (pill letter on colored pill)
    0, 0, 0, 0, 0        // unused
};

static uint8_t ageColorIdx(float age_s) {
    if (age_s < 5.0f)  return PI_OK_GREEN;
    if (age_s < 15.0f) return PI_WARN_AMBER;
    return PI_BAD_RED;
}

// ---- formatting helpers ----------------------------------------------------

static void fmtDist(char* out, size_t n, float m) {
    float mag = fabsf(m);
    if (mag < 100.0f)        snprintf(out, n, "%.2f m", m);
    else if (mag < 1000.0f)  snprintf(out, n, "%.1f m", m);
    else                     snprintf(out, n, "%.2f km", m / 1000.0f);
}

static void fmtLat(char* out, size_t n, double lat) {
    snprintf(out, n, "%.6f %c", fabs(lat), lat >= 0 ? 'N' : 'S');
}

static void fmtLon(char* out, size_t n, double lon) {
    snprintf(out, n, "%.6f %c", fabs(lon), lon >= 0 ? 'E' : 'W');
}

static void fmtAlt(char* out, size_t n, float alt) {
    snprintf(out, n, "%.2f m", alt);
}

static int wifiBars(int rssi) {
    int bars = (rssi + 95) / 10;
    if (bars < 1) bars = 1;
    if (bars > 4) bars = 4;
    return bars;
}

static void drawArrowHoriz(int cx, int cy, int w, int h, uint8_t colorIdx) {
    int left   = cx - w / 2;
    int right  = cx + w / 2;
    int headW  = h / 2;
    int shaftL = left  + headW;
    int shaftR = right - headW;
    frame.drawFastHLine(shaftL, cy - 1, shaftR - shaftL, colorIdx);
    frame.drawFastHLine(shaftL, cy,     shaftR - shaftL, colorIdx);
    frame.drawFastHLine(shaftL, cy + 1, shaftR - shaftL, colorIdx);
    frame.fillTriangle(left,  cy, shaftL, cy - headW, shaftL, cy + headW, colorIdx);
    frame.fillTriangle(right, cy, shaftR, cy - headW, shaftR, cy + headW, colorIdx);
}

static void drawArrowVert(int cx, int cy, int w, int h, uint8_t colorIdx) {
    int top    = cy - h / 2;
    int bottom = cy + h / 2;
    int headH  = w / 2;
    int shaftT = top    + headH;
    int shaftB = bottom - headH;
    frame.drawFastVLine(cx - 1, shaftT, shaftB - shaftT, colorIdx);
    frame.drawFastVLine(cx,     shaftT, shaftB - shaftT, colorIdx);
    frame.drawFastVLine(cx + 1, shaftT, shaftB - shaftT, colorIdx);
    frame.fillTriangle(cx, top,    cx - headH, shaftT, cx + headH, shaftT, colorIdx);
    frame.fillTriangle(cx, bottom, cx - headH, shaftB, cx + headH, shaftB, colorIdx);
}

// ---- section draws (operate on the 4-bit sprite; color args = palette idx) -

void CydDisplay::drawHeader() {
    frame.fillRect(0, 0, W, 18, PI_SURFACE);
    frame.drawFastHLine(0, 18, W, PI_BORDER);

    int bars = wifiBars(_state.wifi_rssi);
    for (int i = 0; i < 4; ++i) {
        int barH = 2 + i * 2;
        int x    = 5 + i * 3;
        frame.fillRect(x, 14 - barH, 2, barH, (i < bars) ? PI_ACCENT : PI_IDLE_GRAY);
    }

    frame.setTextFont(2);   // built-in 16px bitmap; more compact than 9pt GFX
    frame.setTextDatum(TL_DATUM);
    frame.setTextColor(PI_TEXT_DIM, PI_SURFACE);
    char buf[48];
    snprintf(buf, sizeof(buf), "%s  %d dBm", _state.wifi_ssid, _state.wifi_rssi);
    frame.drawString(buf, 20, 1);

    uint8_t ntripCol = _state.ntrip_ok ? PI_OK_GREEN : PI_BAD_RED;
    frame.fillCircle(169, 9, 3, ntripCol);
    frame.drawString("NTRIP", 176, 1);

    frame.setTextDatum(TR_DATUM);
    frame.drawString(_state.time_utc, W - 6, 1);
}

void CydDisplay::drawPresetPill() {
    uint8_t colorIdx = ageColorIdx(_state.corr_age);
    constexpr int x = 6, y = 26, w = 68, h = 48, r = 6;
    frame.fillRoundRect(x, y, w, h, r, colorIdx);
    frame.fillRect(x + w - r, y, r, h, colorIdx);

    frame.setFreeFont(&FreeSansBold18pt7b);
    frame.setTextDatum(MC_DATUM);
    frame.setTextColor(PI_BLACK, colorIdx);
    char letter[2] = {(char)('A' + _state.selected), 0};
    frame.drawString(letter, x + w / 2, y + h / 2 + 2);
}

void CydDisplay::drawCoords() {
    constexpr int lx = 74,  ly = 26, lw = 118, lh = 48;
    constexpr int rx = 196, ry = 26, rw = 118, rh = 48;

    frame.fillRect(lx, ly, lw, lh, PI_SURFACE);
    frame.drawRect(lx, ly, lw, lh, PI_BORDER);
    frame.fillRect(rx, ry, rw, rh, PI_SURFACE);
    frame.drawRect(rx, ry, rw, rh, PI_BORDER);

    frame.setTextFont(2);   // built-in 16px bitmap; tighter per-char than GFX FreeMono9pt
    frame.setTextDatum(TR_DATUM);
    frame.setTextColor(PI_TEXT, PI_SURFACE);

    char latBuf[24], lonBuf[24], altBuf[24];
    if (_state.preset_saved[_state.selected]) {
        fmtLat(latBuf, sizeof(latBuf), _state.preset_lat[_state.selected]);
        fmtLon(lonBuf, sizeof(lonBuf), _state.preset_lon[_state.selected]);
        fmtAlt(altBuf, sizeof(altBuf), _state.preset_alt[_state.selected]);
    } else {
        strcpy(latBuf, "-"); strcpy(lonBuf, "-"); strcpy(altBuf, "-");
    }
    frame.drawString(latBuf, lx + lw - 6, ly + 3);
    frame.drawString(lonBuf, lx + lw - 6, ly + 3 + 14);
    frame.drawString(altBuf, lx + lw - 6, ly + 3 + 28);

    fmtLat(latBuf, sizeof(latBuf), _state.lat_deg);
    fmtLon(lonBuf, sizeof(lonBuf), _state.lon_deg);
    fmtAlt(altBuf, sizeof(altBuf), _state.alt_m);
    frame.drawString(latBuf, rx + rw - 6, ry + 3);
    frame.drawString(lonBuf, rx + rw - 6, ry + 3 + 14);
    frame.drawString(altBuf, rx + rw - 6, ry + 3 + 28);
}

void CydDisplay::drawDelta() {
    bool saved = _state.preset_saved[_state.selected];

    if (!saved) {
        char msg[32];
        snprintf(msg, sizeof(msg), "HOLD  %c  TO SAVE", (char)('A' + _state.selected));
        frame.setFreeFont(&FreeSansBold9pt7b);
        frame.setTextDatum(MC_DATUM);
        frame.setTextColor(PI_ACCENT, PI_BG);
        frame.drawString(msg, W / 2, 112);

        frame.setTextFont(2);
        frame.setTextColor(PI_TEXT_DIM, PI_BG);
        frame.drawString("the current position here", W / 2, 136);
        return;
    }

    drawArrowHoriz(24, 102, 28, 14, PI_TEXT_DIM);
    drawArrowVert (24, 142, 14, 28, PI_TEXT_DIM);

    frame.setTextFont(4);   // 26px bitmap -- approx 14pt, sits between 12/18pt GFX
    frame.setTextDatum(TR_DATUM);
    frame.setTextColor(PI_TEXT, PI_BG);

    char buf[20];
    fmtDist(buf, sizeof(buf), _state.preset_dh[_state.selected]);
    frame.drawString(buf, W - 10, 89);
    fmtDist(buf, sizeof(buf), _state.preset_dv[_state.selected]);
    frame.drawString(buf, W - 10, 129);
}

void CydDisplay::drawStats() {
    constexpr int y = 179;

    uint8_t ageCol = ageColorIdx(_state.corr_age);

    frame.setTextFont(2);   // 16px bitmap; tighter than 9pt GFX, fits the row

    char satsVal[8], ageVal[16], hdopVal[8];
    snprintf(satsVal, sizeof(satsVal), "%u", _state.siv);
    snprintf(ageVal,  sizeof(ageVal),  "%4.1f s", _state.corr_age);
    snprintf(hdopVal, sizeof(hdopVal), "%.1f", _state.hdop);

    frame.setTextDatum(TL_DATUM);
    frame.setTextColor(PI_TEXT_DIM, PI_BG);
    frame.drawString("SATS", 64,  y);
    frame.drawString("AGE",  144, y);
    frame.drawString("HDOP", 226, y);

    frame.setTextDatum(TR_DATUM);
    frame.setTextColor(PI_TEXT, PI_BG);
    frame.drawString(satsVal, 120, y);
    frame.setTextColor(ageCol, PI_BG);
    frame.drawString(ageVal,  216, y);
    frame.setTextColor(PI_TEXT, PI_BG);
    frame.drawString(hdopVal, 294, y);
}

void CydDisplay::drawButtons() {
    frame.drawFastHLine(0, 195, W, PI_BORDER);

    constexpr int pad = 4, y0 = 200, h = 34;
    constexpr int btnW = (W - pad * 5) / 4;  // 75

    frame.setFreeFont(&FreeSansBold12pt7b);
    frame.setTextDatum(MC_DATUM);

    for (int i = 0; i < 4; ++i) {
        int x0 = pad + i * (btnW + pad);
        bool selected = (i == _state.selected);
        bool saved    = _state.preset_saved[i];

        uint8_t fill, stroke, textCol;
        bool thick;
        if (selected)   { fill = PI_SURFACE; stroke = PI_ACCENT; textCol = PI_ACCENT;   thick = true;  }
        else if (saved) { fill = PI_SURFACE; stroke = PI_BORDER; textCol = PI_TEXT;     thick = false; }
        else            { fill = PI_BG;      stroke = PI_BORDER; textCol = PI_TEXT_DIM; thick = false; }

        frame.fillRoundRect(x0, y0, btnW, h, 5, fill);
        frame.drawRoundRect(x0, y0, btnW, h, 5, stroke);
        if (thick) frame.drawRoundRect(x0 + 1, y0 + 1, btnW - 2, h - 2, 4, stroke);

        char letter[2] = {(char)('A' + i), 0};
        frame.setTextColor(textCol, fill);
        frame.drawString(letter, x0 + btnW / 2, y0 + h / 2 + 1);
    }
}

// ---- fake-state rotator ----------------------------------------------------

#ifdef CYD_FAKE_STATE
void CydDisplay::rotateFakeState() {
    uint32_t now = millis();

    _state.selected = (now / 4000) % 4;
    _state.corr_age = fmodf(now / 1000.0f, 20.0f);

    float w = sinf(now / 1500.0f) * 0.000005f;
    _state.lat_deg = 47.498232 + w;
    _state.lon_deg = 19.023914 + w;
    _state.alt_m   = 137.40f + w * 100.0f;

    float breathe = sinf(now / 800.0f);
    if (_state.selected == 0) {
        _state.preset_dh[0] = 0.12f + breathe * 0.03f;
        _state.preset_dv[0] = 0.01f + breathe * 0.01f;
    } else if (_state.selected == 1) {
        _state.preset_dh[1] = 12.40f + breathe * 0.5f;
        _state.preset_dv[1] = -1.80f;
    } else if (_state.selected == 2) {
        _state.preset_dh[2] = 187.0f + breathe * 2.0f;
        _state.preset_dv[2] = 6.20f;
    }
}
#endif

// ---- init + update ---------------------------------------------------------

void CydDisplay::init() {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    tft.init();
    tft.invertDisplay(true);
    tft.setRotation(1);
    // pushSprite sends 16-bit RGB565 pixels (expanded from the 4-bit sprite
    // via the palette); ILI9341 wants high-byte first.
    tft.setSwapBytes(true);
    tft.fillScreen(palette::BG);

    frame.setColorDepth(4);
    uint16_t* buf = (uint16_t*)frame.createSprite(W, H);
    Serial.printf("[CYD] sprite4=%p free=%u largest=%u\n",
                  buf, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    _spriteReady = (buf != nullptr);
    if (_spriteReady) {
        frame.createPalette(palette16);
        frame.fillSprite(PI_BG);
    } else {
        Serial.println("[CYD] 4-bit sprite alloc FAILED");
    }

#ifdef CYD_FAKE_STATE
    _state = {};
    _state.lat_deg = 47.498232;
    _state.lon_deg = 19.023914;
    _state.alt_m   = 137.40f;
    _state.siv     = 24;
    _state.corr_age = 1.2f;
    _state.hdop    = 0.6f;
    _state.preset_saved[0] = true;
    _state.preset_lat[0]   = 47.498234;
    _state.preset_lon[0]   = 19.023915;
    _state.preset_alt[0]   = 137.42f;
    _state.preset_dh[0]    = 0.12f;
    _state.preset_dv[0]    = 0.01f;
    _state.preset_saved[1] = true;
    _state.preset_lat[1]   = 47.498112;
    _state.preset_lon[1]   = 19.023820;
    _state.preset_alt[1]   = 136.80f;
    _state.preset_dh[1]    = 12.40f;
    _state.preset_dv[1]    = -1.80f;
    _state.preset_saved[2] = true;
    _state.preset_lat[2]   = 47.500010;
    _state.preset_lon[2]   = 19.025000;
    _state.preset_alt[2]   = 143.20f;
    _state.preset_dh[2]    = 187.0f;
    _state.preset_dv[2]    = 6.20f;
    _state.preset_saved[3] = false;
    strncpy(_state.wifi_ssid, "ZTE_9F2A", sizeof(_state.wifi_ssid));
    _state.wifi_rssi = -58;
    _state.ntrip_ok  = true;
    strncpy(_state.time_utc, "10:34:52", sizeof(_state.time_utc));
    _state.selected = 0;
#endif
}

void CydDisplay::update(const GnssData& data) {
    if (!_spriteReady) return;

    static uint32_t _lastDrawMs = 0;
    uint32_t now = millis();
    if (now - _lastDrawMs < 250) return;
    _lastDrawMs = now;

#ifdef CYD_FAKE_STATE
    (void)data;
    rotateFakeState();
#else
    (void)data;
#endif

    frame.fillSprite(PI_BG);
    drawHeader();
    drawPresetPill();
    drawCoords();
    drawDelta();
    drawStats();
    drawButtons();

    frame.pushSprite(0, 0);
}

#endif
