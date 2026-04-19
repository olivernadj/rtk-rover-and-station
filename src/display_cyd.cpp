#if defined(MODE_ROVER) && defined(BOARD_CYD)

#include "display_cyd.h"
#include "display_cyd_palette.h"
#include "ntrip_client.h"
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

// NVS-backed preset persistence. Namespace stays open for the life of the
// process so writes after each savePreset are cheap.
static Preferences _prefs;
static constexpr const char* NVS_NAMESPACE = "rover-pres";

// Haptic flash duration after a tap or long-press. 250 ms matches the
// HUD's redraw interval -- guarantees at least one frame shows the flashed
// state even in the worst-case phase alignment.
static constexpr uint32_t FLASH_MS = 250;

// Rolling position buffer: 15s of samples at the HUD's 4 Hz redraw rate.
// Used to smooth the delta-band "hero" readings and to capture a stable
// averaged position at the moment a user long-presses A/B/C/D.
// 64 slots = 16s headroom at 4 Hz; each slot is 24 B so total ~1.5 KB.
namespace {
struct PositionSample {
    double   lat_deg;
    double   lon_deg;
    float    alt_m;
    uint32_t ms;
};
constexpr size_t   POSBUF_SIZE    = 64;
constexpr uint32_t AVG_WINDOW_MS  = 15000;
PositionSample _posBuf[POSBUF_SIZE];
size_t _posBufHead  = 0;
size_t _posBufCount = 0;

void appendPositionSample(double lat, double lon, float alt) {
    _posBuf[_posBufHead] = {lat, lon, alt, millis()};
    _posBufHead = (_posBufHead + 1) % POSBUF_SIZE;
    if (_posBufCount < POSBUF_SIZE) _posBufCount++;
}

struct AvgPosition { double lat_deg; double lon_deg; float alt_m; bool valid; };

// Average of all samples within the last AVG_WINDOW_MS. `valid=false` if
// the buffer is empty (pre-first-fix), letting callers fall back to the
// instantaneous fix.
AvgPosition computeAvgPosition() {
    uint32_t now = millis();
    double sumLat = 0.0, sumLon = 0.0;
    float  sumAlt = 0.0f;
    size_t n = 0;
    for (size_t i = 0; i < _posBufCount; ++i) {
        const PositionSample& s = _posBuf[(_posBufHead + POSBUF_SIZE - 1 - i) % POSBUF_SIZE];
        if (now - s.ms > AVG_WINDOW_MS) break;
        sumLat += s.lat_deg;
        sumLon += s.lon_deg;
        sumAlt += s.alt_m;
        n++;
    }
    if (n == 0) return {0, 0, 0, false};
    return {sumLat / n, sumLon / n, sumAlt / n, true};
}
} // namespace

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

// Horizontal ground distance in metres between two WGS-84 lat/lon points.
// Equirectangular projection is accurate enough at walking scale (<1 km);
// we avoid the full haversine sqrt/trig for speed.
static float horizDistM(double lat1, double lon1, double lat2, double lon2) {
    constexpr double R = 6371000.0;
    constexpr double DEG2RAD = M_PI / 180.0;
    double meanLat = (lat1 + lat2) * 0.5 * DEG2RAD;
    double x = (lon2 - lon1) * DEG2RAD * cos(meanLat);
    double y = (lat2 - lat1) * DEG2RAD;
    return (float)(sqrt(x * x + y * y) * R);
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
    // SSID truncated to 12 chars so long names don't run into the NTRIP dot
    // at x=166; RSSI shown as a bare negative number, strength indicated by
    // the bar graph to the left.
    char buf[48];
    snprintf(buf, sizeof(buf), "%.12s %d", _state.wifi_ssid, _state.wifi_rssi);
    frame.drawString(buf, 20, 1);

    uint8_t ntripCol = _state.ntrip_ok ? PI_OK_GREEN : PI_BAD_RED;
    frame.fillCircle(169, 9, 3, ntripCol);
    frame.drawString("NTRIP", 176, 1);

    frame.setTextDatum(TR_DATUM);
    frame.drawString(_state.time_utc, W - 6, 1);
}

void CydDisplay::drawPresetPill() {
    // Haptic: pill overrides to accent-blue for FLASH_MS after selectPreset().
    uint8_t colorIdx = (millis() < _pillFlashUntilMs) ? PI_ACCENT
                                                      : ageColorIdx(_state.corr_age);
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

    uint32_t now = millis();
    for (int i = 0; i < 4; ++i) {
        int x0 = pad + i * (btnW + pad);
        bool selected = (i == _state.selected);
        bool saved    = _state.preset_saved[i];
        bool flash    = (now < _buttonFlashUntilMs[i]);   // save-confirmation flash

        uint8_t fill, stroke, textCol;
        bool thick;
        if (flash)           { fill = PI_ACCENT;  stroke = PI_ACCENT; textCol = PI_BLACK;    thick = true;  }
        else if (selected)   { fill = PI_SURFACE; stroke = PI_ACCENT; textCol = PI_ACCENT;   thick = true;  }
        else if (saved)      { fill = PI_SURFACE; stroke = PI_BORDER; textCol = PI_TEXT;     thick = false; }
        else                 { fill = PI_BG;      stroke = PI_BORDER; textCol = PI_TEXT_DIM; thick = false; }

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

// ---- preset NVS persistence + state mutators ------------------------------

void CydDisplay::loadPresetsFromNvs() {
    char key[8];
    for (int i = 0; i < 4; ++i) {
        snprintf(key, sizeof(key), "s%d", i);
        if (!_prefs.getBool(key, false)) continue;
        _state.preset_saved[i] = true;
        snprintf(key, sizeof(key), "lat%d", i);
        _state.preset_lat[i] = _prefs.getDouble(key, 0.0);
        snprintf(key, sizeof(key), "lon%d", i);
        _state.preset_lon[i] = _prefs.getDouble(key, 0.0);
        snprintf(key, sizeof(key), "alt%d", i);
        _state.preset_alt[i] = _prefs.getFloat(key, 0.0f);
        if (i == 0) _autoSavedA = true;   // don't overwrite a user-saved A with the first-fix auto-save
    }
}

void CydDisplay::writePresetToNvs(uint8_t i) {
    if (i >= 4) return;
    char key[8];
    snprintf(key, sizeof(key), "s%d", i);
    _prefs.putBool(key, _state.preset_saved[i]);
    if (!_state.preset_saved[i]) return;
    snprintf(key, sizeof(key), "lat%d", i);
    _prefs.putDouble(key, _state.preset_lat[i]);
    snprintf(key, sizeof(key), "lon%d", i);
    _prefs.putDouble(key, _state.preset_lon[i]);
    snprintf(key, sizeof(key), "alt%d", i);
    _prefs.putFloat(key, _state.preset_alt[i]);
}

void CydDisplay::selectPreset(uint8_t i) {
    if (i >= 4) return;
    _state.selected = i;
    _pillFlashUntilMs = millis() + FLASH_MS;
    Serial.printf("[HUD] select preset %c\n", (char)('A' + i));
}

void CydDisplay::savePreset(uint8_t i) {
    if (i >= 4) return;
    if (!_hasLiveFix) {
        Serial.printf("[HUD] savePreset %c skipped (no 3D fix yet)\n", (char)('A' + i));
        return;
    }
    // Capture the averaged position at the moment of the long-press so the
    // saved point isn't biased by a single noisy sample. Fall back to the
    // instantaneous fix if the rolling window hasn't filled yet.
    AvgPosition avg = computeAvgPosition();
    double lat = avg.valid ? avg.lat_deg : _state.lat_deg;
    double lon = avg.valid ? avg.lon_deg : _state.lon_deg;
    float  alt = avg.valid ? avg.alt_m   : _state.alt_m;

    _state.preset_saved[i] = true;
    _state.preset_lat[i]   = lat;
    _state.preset_lon[i]   = lon;
    _state.preset_alt[i]   = alt;
    writePresetToNvs(i);
    _buttonFlashUntilMs[i] = millis() + FLASH_MS;
    Serial.printf("[HUD] saved preset %c at %.6f,%.6f,%.2fm (avg of %s)\n",
                  (char)('A' + i), lat, lon, alt,
                  avg.valid ? "last 15 s" : "single sample -- buffer cold");
}

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

    _state = {};
    _state.selected = 0;
    strncpy(_state.wifi_ssid, "--", sizeof(_state.wifi_ssid));
    _state.wifi_rssi = -100;
    strncpy(_state.time_utc, "--:--:--", sizeof(_state.time_utc));

    _prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
    loadPresetsFromNvs();

#ifdef CYD_FAKE_STATE
    // Seed fake state so the rotator starts in a sensible place.
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
    // ---- map live GNSS + WiFi + NTRIP + time into _state ------------------

    if (data.valid) {
        _state.lat_deg  = data.lat / 1e7;
        _state.lon_deg  = data.lon / 1e7;
        _state.alt_m    = data.alt / 1000.0f;
        _state.siv      = data.siv;
        _state.corr_age = data.corr_age;
        _state.hdop     = data.pdop / 100.0f;
        if (data.fix_type >= 3) {
            _hasLiveFix = true;
            appendPositionSample(_state.lat_deg, _state.lon_deg, _state.alt_m);
        }

        // Auto-populate preset A on the first 3D fix so the delta band has
        // something to display on fresh boot. Skipped when NVS already holds
        // a saved slot A (user has their own preset from a prior session).
        if (!_autoSavedA && _hasLiveFix) {
            _state.preset_saved[0] = true;
            _state.preset_lat[0]   = _state.lat_deg;
            _state.preset_lon[0]   = _state.lon_deg;
            _state.preset_alt[0]   = _state.alt_m;
            writePresetToNvs(0);
            _autoSavedA = true;
        }

        // Delta band uses the 15-second rolling average of the rover's own
        // position (smoother hero readings). The current cell keeps the
        // instantaneous fix. Fall back to instant values until the buffer
        // warms up.
        AvgPosition avg = computeAvgPosition();
        double refLat = avg.valid ? avg.lat_deg : _state.lat_deg;
        double refLon = avg.valid ? avg.lon_deg : _state.lon_deg;
        float  refAlt = avg.valid ? avg.alt_m   : _state.alt_m;
        for (int i = 0; i < 4; ++i) {
            if (_state.preset_saved[i]) {
                _state.preset_dh[i] = horizDistM(refLat, refLon,
                                                 _state.preset_lat[i], _state.preset_lon[i]);
                _state.preset_dv[i] = _state.preset_alt[i] - refAlt;
            }
        }
    }

    // WiFi row: live SSID + RSSI when connected, "--" otherwise.
    // WiFi.SSID() returns a temporary String; copy directly so the temporary
    // stays alive across the strncpy. (Saving .c_str() to a local ptr first
    // would be UB -- the String dies at the semicolon.)
    if (WiFi.isConnected()) {
        strncpy(_state.wifi_ssid, WiFi.SSID().c_str(), sizeof(_state.wifi_ssid) - 1);
        _state.wifi_ssid[sizeof(_state.wifi_ssid) - 1] = '\0';
        _state.wifi_rssi = WiFi.RSSI();
    } else {
        strncpy(_state.wifi_ssid, "--", sizeof(_state.wifi_ssid));
        _state.wifi_rssi = -100;
    }

    // NTRIP dot: green means corrections are actively flowing (same rule as
    // the status LED in status_led.cpp)
    _state.ntrip_ok = ntripIsConnected() && data.corr_age < 60;

    // UTC clock from NTP-synced system time
    time_t t = time(nullptr);
    if (t > 1577836800UL) {   // > 2020-01-01 means NTP has synced
        struct tm tm_utc;
        gmtime_r(&t, &tm_utc);
        strftime(_state.time_utc, sizeof(_state.time_utc), "%H:%M:%S", &tm_utc);
    } else {
        strncpy(_state.time_utc, "--:--:--", sizeof(_state.time_utc));
    }
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
