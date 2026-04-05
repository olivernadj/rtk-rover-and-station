#include "metrics.h"
#include <stdio.h>
#include <time.h>
#include <WiFi.h>

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
        "\"siv\":\"%d\","
        "\"fix_type\":\"%d\","
        "\"carr_soln\":\"%d\""
      "},"
      "\"timestamp\":%ld,"
      "\"client\":\"rtk-%s\","
      "\"labels\":{"
        "\"device\":\"%s\","
        "\"mode\":\"%s\","
        "\"project\":\"GPS\""
      "}"
    "}";

size_t metricsFormat(char* buf, size_t len, const GnssData& data) {
    time_t now;
    time(&now);

    int n = snprintf(buf, len, msgTemplate,
        (long)data.lat,
        (int)data.latHp,
        (long)data.lon,
        (int)data.lonHp,
        (long)data.alt,
        (int)data.siv,
        (int)data.fix_type,
        (int)data.carr_soln,
        (long)now,
        MODE_STR,
        WiFi.getHostname(),
        MODE_STR
    );
    if (n < 0 || (size_t)n >= len) return 0;
    return (size_t)n;
}
