#pragma once
#if defined(MODE_ROVER) && defined(BOARD_CYD)

#include <stdint.h>

// RGB888 -> RGB565 at compile time.
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

// Palette mirrors ui/mockup.py in the companion ESP32-2432S028-USBC project.
// Dark UI chosen so a color-change on the pill is the main RTK-quality signal.
namespace palette {
constexpr uint16_t BG         = RGB565( 13,  17,  23);
constexpr uint16_t SURFACE    = RGB565( 22,  27,  34);
constexpr uint16_t BORDER     = RGB565( 48,  54,  61);
constexpr uint16_t TEXT       = RGB565(230, 237, 243);
constexpr uint16_t TEXT_DIM   = RGB565(139, 148, 158);
constexpr uint16_t ACCENT     = RGB565( 88, 166, 255);
constexpr uint16_t OK_GREEN   = RGB565( 63, 185,  80);
constexpr uint16_t WARN_AMBER = RGB565(210, 153,  34);
constexpr uint16_t BAD_RED    = RGB565(248,  81,  73);
constexpr uint16_t IDLE_GRAY  = RGB565(110, 118, 129);
}

// RTCM correction-age thresholds drive both the pill and AGE color.
inline uint16_t ageColor(float age_s) {
    if (age_s < 5.0f)  return palette::OK_GREEN;
    if (age_s < 15.0f) return palette::WARN_AMBER;
    return palette::BAD_RED;
}

#endif
