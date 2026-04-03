#pragma once
#include <functional>

using WifiEventCallback = std::function<void()>;

void wifiInit(WifiEventCallback onConnect, WifiEventCallback onDisconnect);
bool wifiIsConnected();
