#pragma once
#include <stdint.h>
#include <stddef.h>

// Forward-declare the SparkFun GNSS class to avoid pulling the full header
// into every translation unit that only needs GnssData.
class SFE_UBLOX_GNSS;

struct GnssData {
    int32_t  lat;       // degrees × 10^-7
    int8_t   latHp;     // additional degrees × 10^-9 (high-precision component)
    int32_t  lon;       // degrees × 10^-7
    int8_t   lonHp;     // additional degrees × 10^-9 (high-precision component)
    int32_t  alt;       // mm above ellipsoid
    uint16_t corr_age;  // correction age in seconds (rover: from ZED-F9P, stationary: since last RTCM push)
    uint8_t  siv;       // satellites in view
    uint8_t  fix_type;  // 0=no fix, 2=2D, 3=3D, 4=GNSS+DR, 5=time only
    uint8_t  carr_soln; // 0=none, 1=float RTK, 2=fixed RTK
    bool     valid;     // true when data has been populated at least once
};

bool              gnssInit();
void              gnssUpdate();
const GnssData&   gnssGetData();
void              gnssSetCorrAge(uint16_t seconds);
bool              gnssHasError();

// Returns the underlying driver object (used by ntrip_client / ntrip_broadcaster).
SFE_UBLOX_GNSS&   gnssGetHandle();

// Notify GNSS module that RTCM corrections were successfully pushed (rover).
#ifdef MODE_ROVER
void              gnssNotifyCorrPush();
#endif

// Reads RTCM bytes produced by ZED-F9P in base-station mode.
// Returns number of bytes actually read (may be 0 if none available).
#ifdef MODE_STATIONARY
size_t gnssReadRtcm(uint8_t* buf, size_t maxLen);
#endif
