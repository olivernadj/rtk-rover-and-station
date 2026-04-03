#pragma once
#include <stdbool.h>

void mqttInit();
bool mqttPublish(const char* payload);
bool mqttIsConnected();

// Called by main.cpp from WiFi event callbacks.
void mqttOnWifiConnect();
void mqttOnWifiDisconnect();
