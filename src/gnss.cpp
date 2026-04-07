#include "gnss.h"
#include "config.h"
#include <Wire.h>
#include <SparkFun_u-blox_GNSS_v3.h>

// ── RTCM ring buffer for stationary mode ────────────────────────────────────
#ifdef MODE_STATIONARY
static constexpr size_t RTCM_RING_SIZE = 2048;
static uint8_t  _rtcmRing[RTCM_RING_SIZE];
static volatile size_t _rtcmHead = 0;
static volatile size_t _rtcmTail = 0;

// Override the weak processRTCM to capture RTCM bytes into ring buffer
void DevUBLOXGNSS::processRTCM(uint8_t incoming) {
    size_t nextHead = (_rtcmHead + 1) % RTCM_RING_SIZE;
    if (nextHead != _rtcmTail) {
        _rtcmRing[_rtcmHead] = incoming;
        _rtcmHead = nextHead;
    }
}
#endif

static SFE_UBLOX_GNSS _gnss;

static GnssData _data  = {};
static bool     _error = false;

#ifdef MODE_ROVER
static GnssData  _cachedBest       = {};
static bool      _cacheValid       = false;
static uint32_t  _cacheTimestampMs = 0;
static uint32_t  _corrPushMs[2]    = {0, 0};   // last two pushRawData timestamps
static bool      _corrPushInit     = false;     // true after first push
#endif

bool gnssInit() {
    Wire.begin(GNSS_SDA_PIN, GNSS_SCL_PIN);
    Wire.setClock(GNSS_I2C_FREQ);

    if (!_gnss.begin()) {
        _error = true;
        return false;
    }

    // UBX+RTCM3 alone is not valid; must enable all three (per SparkFun docs)
    _gnss.setI2COutput(COM_TYPE_UBX | COM_TYPE_NMEA | COM_TYPE_RTCM3);
    _gnss.setNavigationFrequency(1);        // 1 Hz matches GPS_SAMPLE_INTERVAL_MS
    _gnss.setAutoPVT(true);                 // push PVT automatically; getPVT() is non-blocking
    _gnss.setAutoHPPOSLLH(true);            // enable NAV-HPPOSLLH for high-res lat/lon

#ifdef MODE_STATIONARY
    // Disable NMEA on I2C (we only needed it enabled for the port protocol)
    bool ok = _gnss.newCfgValset(VAL_LAYER_RAM);
    ok &= _gnss.addCfgValset(UBLOX_CFG_MSGOUT_NMEA_ID_GLL_I2C, 0);
    ok &= _gnss.addCfgValset(UBLOX_CFG_MSGOUT_NMEA_ID_GSA_I2C, 0);
    ok &= _gnss.addCfgValset(UBLOX_CFG_MSGOUT_NMEA_ID_GSV_I2C, 0);
    ok &= _gnss.addCfgValset(UBLOX_CFG_MSGOUT_NMEA_ID_GST_I2C, 0);
    ok &= _gnss.addCfgValset(UBLOX_CFG_MSGOUT_NMEA_ID_RMC_I2C, 0);
    ok &= _gnss.addCfgValset(UBLOX_CFG_MSGOUT_NMEA_ID_VTG_I2C, 0);
    ok &= _gnss.addCfgValset(UBLOX_CFG_MSGOUT_NMEA_ID_GGA_I2C, 0);
    ok &= _gnss.addCfgValset(UBLOX_CFG_MSGOUT_NMEA_ID_ZDA_I2C, 0);
    ok &= _gnss.sendCfgValset();
    Serial.printf("[GNSS] NMEA disable: %s\n", ok ? "OK" : "FAILED");

    // Enable RTCM messages in a single batched valset
    ok = _gnss.newCfgValset(VAL_LAYER_RAM);
    ok &= _gnss.addCfgValset(UBLOX_CFG_MSGOUT_RTCM_3X_TYPE1005_I2C, 1);
    ok &= _gnss.addCfgValset(UBLOX_CFG_MSGOUT_RTCM_3X_TYPE1074_I2C, 1);
    ok &= _gnss.addCfgValset(UBLOX_CFG_MSGOUT_RTCM_3X_TYPE1084_I2C, 1);
    ok &= _gnss.addCfgValset(UBLOX_CFG_MSGOUT_RTCM_3X_TYPE1094_I2C, 1);
    ok &= _gnss.addCfgValset(UBLOX_CFG_MSGOUT_RTCM_3X_TYPE1124_I2C, 1);
    ok &= _gnss.addCfgValset(UBLOX_CFG_MSGOUT_RTCM_3X_TYPE1230_I2C, 10);
    ok &= _gnss.sendCfgValset();
    Serial.printf("[GNSS] RTCM enable: %s\n", ok ? "OK" : "FAILED");

    // Use a known fixed position or fall back to survey-in.
    if (USE_FIXED_POSITION) {
        int32_t altCm     = FIXED_ALT_MM / 10;
        int8_t  altHpMm01 = (int8_t)(FIXED_ALT_MM % 10);
        bool posOk = _gnss.setStaticPosition(FIXED_LAT, 0, FIXED_LON, 0,
                                             altCm, altHpMm01, true);
        Serial.printf("[GNSS] setStaticPosition: %s\n", posOk ? "OK" : "FAILED");
        Serial.printf("[GNSS] Fixed position: lat=%ld lon=%ld alt=%ld mm\n",
                      (long)FIXED_LAT, (long)FIXED_LON, (long)FIXED_ALT_MM);
    } else {
        _gnss.enableSurveyMode(SURVEY_IN_MIN_DURATION_S, SURVEY_IN_ACCURACY_M);
        Serial.printf("[GNSS] Survey-in: %u s, %.1f m accuracy\n",
                      SURVEY_IN_MIN_DURATION_S, SURVEY_IN_ACCURACY_M);
    }
#endif

    _error = false;
    return true;
}

void gnssUpdate() {
    if (_error) return;

    if (_gnss.getPVT()) {
        _data.lat       = _gnss.getHighResLatitude();
        _data.latHp     = _gnss.getHighResLatitudeHp();
        _data.lon       = _gnss.getHighResLongitude();
        _data.lonHp     = _gnss.getHighResLongitudeHp();
        _data.alt       = _gnss.getAltitude();
        _data.siv       = _gnss.getSIV();
        _data.fix_type  = _gnss.getFixType();
        _data.carr_soln = _gnss.getCarrierSolutionType();

#ifdef MODE_ROVER
        // Convert lastCorrectionAge enum (4-bit) to upper-bound seconds
        static const uint16_t ageToSec[] = {
            0, 1, 2, 5, 10, 15, 20, 30, 45, 60, 90, 120, 0xFFFF
        };
        uint8_t ageIdx = _gnss.packetUBXNAVPVT->data.flags3.bits.lastCorrectionAge;
        _data.corr_age = (ageIdx < 13) ? ageToSec[ageIdx] : 0xFFFF;

        // Cache position only when RTK-corrected AND corrections were pushed
        // recently enough (between 1 s and GPS_ACCEPTABLE_CORR_AGE_MS ago).
        // Check both the most recent and one-prior push timestamps so that a
        // brand-new push (< 1 s) doesn't disqualify us while the previous one
        // is still in the valid window.
        if (_data.carr_soln > 0 && _corrPushInit) {
            uint32_t now = millis();
            bool pushOk = false;
            for (int i = 0; i < 2; i++) {
                uint32_t age = now - _corrPushMs[i];
                if (age >= 1000 && age <= GPS_ACCEPTABLE_CORR_AGE_MS) {
                    pushOk = true;
                    break;
                }
            }
            if (pushOk) {
                _cachedBest       = _data;
                _cacheValid       = true;
                _cacheTimestampMs = millis();
            }
        }
#endif

        _data.valid     = true;
    }
}

const GnssData& gnssGetData() {
#ifdef MODE_ROVER
    if (_cacheValid) {
        uint32_t elapsed = (millis() - _cacheTimestampMs) / 1000;
        _cachedBest.corr_age = (elapsed < 0xFFFF) ? (uint16_t)elapsed : 0xFFFE;
        return _cachedBest;
    }
#endif
    return _data;
}
#ifdef MODE_ROVER
void gnssNotifyCorrPush() {
    _corrPushMs[1] = _corrPushMs[0];
    _corrPushMs[0] = millis();
    _corrPushInit  = true;
}
#endif
void            gnssSetCorrAge(uint16_t seconds) { _data.corr_age = seconds; }
bool            gnssHasError()    { return _error; }
SFE_UBLOX_GNSS& gnssGetHandle() { return _gnss; }

#ifdef MODE_STATIONARY
size_t gnssReadRtcm(uint8_t* buf, size_t maxLen) {
    if (_error || maxLen == 0) return 0;
    _gnss.checkUblox(); // Pull data from I2C; triggers processRTCM callback
    size_t count = 0;
    while (count < maxLen && _rtcmTail != _rtcmHead) {
        buf[count++] = _rtcmRing[_rtcmTail];
        _rtcmTail = (_rtcmTail + 1) % RTCM_RING_SIZE;
    }
    return count;
}
#endif
