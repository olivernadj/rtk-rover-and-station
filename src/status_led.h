#pragma once
#include <stdbool.h>

void ledInit();

// Call every loop() iteration.
// ntripOk meaning:
//   rover mode      → NTRIP client is connected and streaming
//   stationary mode → at least one broadcaster caster is connected
void ledUpdate(bool wifiOk, bool gnssOk, bool ntpOk, bool mqttOk, bool ntripOk);
