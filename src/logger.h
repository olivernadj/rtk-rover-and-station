#pragma once
#include <cstdio>
#include <cstdarg>
#include <Arduino.h>
#include "config.h"

#if LOG_MQTT_ENABLED
#include "mqtt_manager.h"
#endif

constexpr size_t LOG_BUF_LEN = 256;

inline void logMsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

inline void logMsg(const char* fmt, ...) {
    char buf[LOG_BUF_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

#if LOG_SERIAL_ENABLED
    Serial.println(buf);
#endif

#if LOG_MQTT_ENABLED
    mqttLog(buf);
#endif
}
