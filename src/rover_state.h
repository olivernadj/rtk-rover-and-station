#pragma once
#if defined(MODE_ROVER) && defined(BOARD_CYD)

#include <stdint.h>

// Firmware-side mirror of RoverState in ESP32-2432S028-USBC/ui/mockup.py.
// All presets indexed 0..3 for A/B/C/D. `selected` picks the active register.
//
// Delta values (preset_dh, preset_dv) are computed distances from the live fix
// to each saved preset -- the firmware populates these each time new PVT lands;
// Commit 1 keeps them fake, Commit 2 wires real haversine + alt diff.
struct RoverState {
    // Live fix
    double  lat_deg;
    double  lon_deg;
    float   alt_m;
    uint8_t siv;         // total satellites in view (total SIV, no per-constellation breakdown)
    float   corr_age;    // seconds since last RTCM correction (drives pill + AGE color)
    float   hdop;

    // Presets A..D
    bool    preset_saved[4];
    double  preset_lat[4];
    double  preset_lon[4];
    float   preset_alt[4];
    float   preset_dh[4];     // horizontal distance live -> preset, metres (signed: +away, -past)
    float   preset_dv[4];     // vertical   distance live -> preset, metres (signed: +above, -below)

    // Which preset is currently the target. 0..3 mapped to A..D.
    uint8_t selected;

    // Header telemetry
    char    wifi_ssid[32];
    int     wifi_rssi;
    bool    ntrip_ok;
    char    time_utc[9];      // "HH:MM:SS\0"
};

#endif
