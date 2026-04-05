#include "wifi_manager.h"
#include "config.h"
#include "secrets.h"
#include <WiFi.h>
#include <Ticker.h>
#include <esp_mac.h>

static WifiEventCallback _onConnect;
static WifiEventCallback _onDisconnect;
static Ticker            _reconnectTicker;
static int               _apIndex = 0;
static bool              _reconnectScheduled = false;

static void setHostnameFromMac() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[20];
    snprintf(name, sizeof(name), "esp32s3-%02X%02X%02X", mac[3], mac[4], mac[5]);
    WiFi.setHostname(name);
}

static void startConnect() {
    _reconnectScheduled = false;
    Serial.printf("[WIFI] Connecting to AP #%d: \"%s\"\n",
                  _apIndex, WIFI_CREDENTIALS[_apIndex].ssid);
    WiFi.disconnect(true);
    delay(100);
    setHostnameFromMac();
    WiFi.begin(WIFI_CREDENTIALS[_apIndex].ssid,
               WIFI_CREDENTIALS[_apIndex].password);
    _apIndex = (_apIndex + 1) % WIFI_CREDENTIAL_COUNT;
}

static void onWiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case SYSTEM_EVENT_STA_GOT_IP: {
            _reconnectTicker.detach();
            Serial.printf("[WIFI] Connected! IP: %s  GW: %s\n",
                          WiFi.localIP().toString().c_str(),
                          WiFi.gatewayIP().toString().c_str());
            if (_onConnect) _onConnect();
            break;
        }
        case SYSTEM_EVENT_STA_DISCONNECTED:
            Serial.printf("[WIFI] Disconnected\n");
            if (_onDisconnect) _onDisconnect();
            if (!_reconnectScheduled) {
                _reconnectScheduled = true;
                _reconnectTicker.once(5, startConnect);
            }
            break;

        default:
            break;
    }
}

void wifiInit(WifiEventCallback onConnect, WifiEventCallback onDisconnect) {
    _onConnect    = onConnect;
    _onDisconnect = onDisconnect;
    WiFi.onEvent(onWiFiEvent);
    startConnect();
}

bool wifiIsConnected() {
    return WiFi.isConnected();
}
