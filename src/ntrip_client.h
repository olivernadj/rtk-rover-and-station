#pragma once
#ifdef MODE_ROVER
#include <stdint.h>

void     ntripInit();
void     ntripUpdate();
bool     ntripIsConnected();
void     ntripOnWifiDisconnect();
uint32_t ntripGetCorrCount();

#endif // MODE_ROVER
