#pragma once
#ifdef MODE_STATIONARY
#include <stdint.h>

void     ntripBroadcasterInit();
void     ntripBroadcasterUpdate();
bool     ntripBroadcasterAnyConnected();
uint16_t ntripBroadcasterCorrAgeSec();
uint32_t ntripGetCorrCount();

#endif // MODE_STATIONARY
