#pragma once
#ifdef MODE_ROVER

void ntripInit();
void ntripUpdate();
bool ntripIsConnected();
void ntripOnWifiDisconnect();

#endif // MODE_ROVER
