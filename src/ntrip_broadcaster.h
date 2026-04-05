#pragma once
#ifdef MODE_STATIONARY
#include <stdint.h>

void     ntripBroadcasterInit();
void     ntripBroadcasterUpdate();
bool     ntripBroadcasterAnyConnected();
uint16_t ntripBroadcasterCorrAgeSec();

#endif // MODE_STATIONARY
