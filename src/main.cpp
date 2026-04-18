#include "config.h"
#include "secrets.h"
#include "gnss.h"
#include "metrics.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "logger.h"
#include "status_led.h"
#ifdef OTA_ENABLED
#include "ota_updater.h"
#endif

#ifdef MODE_STATIONARY
#include "ntrip_broadcaster.h"
#endif

#ifdef MODE_ROVER
#include "ntrip_client.h"
#include "display.h"
#ifdef BOARD_CYD
#include "display_cyd.h"
#endif
#endif

#include <Arduino.h>
#include <time.h>

// ── Static globals ───────────────────────────────────────────────────────────
static char     _metricsBuf[METRICS_BUF_LEN];
static bool     _ntpSynced   = false;
static uint32_t _lastPublish = 0;

#ifdef MODE_ROVER
#ifdef BOARD_CYD
static CydDisplay  _cydDisplay;
static IDisplay*   display = &_cydDisplay;
#else
static NullDisplay _nullDisplay;
static IDisplay*   display = &_nullDisplay;
#endif
#endif

// ── Helpers ──────────────────────────────────────────────────────────────────
static bool isNtpSynced() {
    // time() returns seconds since epoch; > 2020-01-01 means NTP has synced.
    return time(nullptr) > 1577836800UL;
}

// ── WiFi callbacks ───────────────────────────────────────────────────────────
static void onWifiConnect() {
    configTime(NTP_GMT_OFFSET_SEC, NTP_DAYLIGHT_OFFSET, NTP_SERVER);
    mqttOnWifiConnect();
}

static void onWifiDisconnect() {
    _ntpSynced = false;
    mqttOnWifiDisconnect();
#ifdef MODE_ROVER
    ntripOnWifiDisconnect();
#endif
}

// ── Arduino setup / loop ─────────────────────────────────────────────────────
void setup() {
    delay(1000);  // let power rails stabilize before initializing peripherals
    Serial.begin(115200);

    ledInit();

    if (!gnssInit()) {
        logMsg("[GNSS] Init failed - check I2C wiring (SDA=%u, SCL=%u)",
               GNSS_SDA_PIN, GNSS_SCL_PIN);
    }

    mqttInit();
    wifiInit(onWifiConnect, onWifiDisconnect);

#ifdef MODE_STATIONARY
    ntripBroadcasterInit();
#endif

#ifdef MODE_ROVER
    ntripInit();
    display->init();
#endif

#ifdef OTA_ENABLED
    otaInit();
#endif

    logMsg("[MAIN] Setup complete");
}

void loop() {
    uint32_t now = millis();

    // 1. Read GNSS (non-blocking: returns immediately if no fresh PVT)
    gnssUpdate();

#ifdef MODE_STATIONARY
    // 2a. Read RTCM from ZED-F9P and push to all configured NTRIP casters
    ntripBroadcasterUpdate();
#endif

#ifdef MODE_ROVER
    // 2b. Fetch RTCM corrections from caster and push to ZED-F9P
    ntripUpdate();
    display->update(gnssGetData());
#endif

    // 3. Check NTP sync (latches true; reset on WiFi disconnect)
    if (!_ntpSynced && wifiIsConnected()) {
        _ntpSynced = isNtpSynced();
    }

#ifdef MODE_STATIONARY
    // Update correction age from broadcaster (seconds since last successful RTCM push)
    gnssSetCorrAge(ntripBroadcasterCorrAgeSec());
#endif

    // 4. Publish metrics at configured interval
    if (now - _lastPublish >= GPS_SAMPLE_INTERVAL_MS) {
        _lastPublish = now;
        const GnssData& data = gnssGetData();
        if (data.valid && mqttIsConnected()) {
            size_t len = metricsFormat(_metricsBuf, sizeof(_metricsBuf), data);
            if (len > 0) {
                mqttPublish(_metricsBuf);
            }
        }
    }

#ifdef OTA_ENABLED
    // 5. Check for OTA firmware update
    otaUpdate();
#endif

    // 6. Update status LED state machine
    bool ntripOk;
#ifdef MODE_STATIONARY
    ntripOk = ntripBroadcasterAnyConnected();
#elif defined(MODE_ROVER)
    ntripOk = ntripIsConnected() && gnssGetData().corr_age < 60;
#else
    ntripOk = true;
#endif

    ledUpdate(
        wifiIsConnected(),
        !gnssHasError(),
        _ntpSynced,
        mqttIsConnected(),
        ntripOk
    );

    // 7. Reboot if status has been non-OK for too long
    if (healthCheckReboot()) {
        Serial.println("[MAIN] Status non-OK for >1 min — rebooting");
        Serial.flush();
        ESP.restart();
    }
}
