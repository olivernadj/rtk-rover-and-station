#include "metrics.h"
#include <stdio.h>
#include <time.h>
#include <WiFi.h>

#ifdef MODE_STATIONARY
#include "ntrip_broadcaster.h"
#else
#include "ntrip_client.h"
#endif

#ifdef MODE_STATIONARY
static constexpr char MODE_STR[] = "stationary";
#else
static constexpr char MODE_STR[] = "rover";
#endif

static const char* msgTemplate =
    "{"
      "\"metric_type\":\"gauge\","
      "\"samples\":{"
        "\"lat\":\"%ld\","
        "\"lat_hp\":\"%d\","
        "\"long\":\"%ld\","
        "\"long_hp\":\"%d\","
        "\"alt\":\"%ld\","
        "\"corr_age\":\"%u\","
        "\"siv\":\"%d\","
        "\"fix_type\":\"%d\","
        "\"carr_soln\":\"%d\","
        "\"wifi_rssi\":\"%d\","
        "\"corr_count\":\"%u\""
      "},"
      "\"timestamp\":%ld,"
      "\"client\":\"rtk-%s\","
      "\"labels\":{"
        "\"device\":\"%s\","
        "\"mode\":\"%s\","
        "\"fw_version\":\"%s\","
        "\"wifi_ssid\":\"%s\","
        "\"project\":\"GPS\""
      "}"
    "}";

size_t metricsFormat(char* buf, size_t len, const GnssData& data) {
    time_t now;
    time(&now);

    String ssid = WiFi.SSID();

    int n = snprintf(buf, len, msgTemplate,
        (long)data.lat,
        (int)data.latHp,
        (long)data.lon,
        (int)data.lonHp,
        (long)data.alt,
        (unsigned)data.corr_age,
        (int)data.siv,
        (int)data.fix_type,
        (int)data.carr_soln,
        (int)WiFi.RSSI(),
        (unsigned)ntripGetCorrCount(),
        (long)now,
        MODE_STR,
        WiFi.getHostname(),
        MODE_STR,
        FW_VERSION,
        ssid.c_str()
    );
    if (n < 0 || (size_t)n >= len) return 0;
    return (size_t)n;
}
