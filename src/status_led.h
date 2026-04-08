#pragma once
#include <stdbool.h>

void ledInit();

// Call every loop() iteration.
// ntripOk meaning:
//   rover mode      → NTRIP client is connected and streaming
//   stationary mode → at least one broadcaster caster is connected
void ledUpdate(bool wifiOk, bool gnssOk, bool ntpOk, bool mqttOk, bool ntripOk);

// Returns true if system health has been non-OK continuously for longer than
// the configured timeout (HEALTH_REBOOT_TIMEOUT_MS).  Caller should reboot.
bool healthCheckReboot();
