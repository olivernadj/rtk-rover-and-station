#include "mqtt_manager.h"
#include "config.h"
#include "secrets.h"
#include "wifi_manager.h"
#include <AsyncMqttClient.h>
#include <Ticker.h>
#include <WiFi.h>

static AsyncMqttClient _mqttClient;
static Ticker          _reconnectTicker;

static void connectToMqtt() {
    if (wifiIsConnected()) {
        _mqttClient.connect();
    }
}

static void onMqttConnect(bool /*sessionPresent*/) {
    _reconnectTicker.detach();
}

static void onMqttDisconnect(AsyncMqttClientDisconnectReason /*reason*/) {
    if (wifiIsConnected()) {
        _reconnectTicker.once_ms(MQTT_RECONNECT_DELAY_MS, connectToMqtt);
    }
    // If WiFi is also down, mqttOnWifiConnect() will re-trigger the connection
    // once WiFi comes back up.
}

void mqttInit() {
    _mqttClient.setServer(MQTT_HOST, (uint16_t)MQTT_PORT);
    _mqttClient.setKeepAlive(MQTT_KEEPALIVE);
    _mqttClient.setClientId(WiFi.getHostname());
    _mqttClient.onConnect(onMqttConnect);
    _mqttClient.onDisconnect(onMqttDisconnect);
}

void mqttOnWifiConnect() {
    // Defer by one Ticker tick so the network stack is fully up before
    // attempting a TCP connection.
    _reconnectTicker.once_ms(100, connectToMqtt);
}

void mqttOnWifiDisconnect() {
    _reconnectTicker.detach();
    _mqttClient.disconnect();
}

bool mqttPublish(const char* payload) {
    if (!_mqttClient.connected()) return false;
    uint16_t id = _mqttClient.publish(MQTT_TOPIC, 0, false, payload);
    return id != 0;
}

bool mqttLog(const char* message) {
    if (!_mqttClient.connected()) return false;
    uint16_t id = _mqttClient.publish("mqtt/logs/v1", 0, false, message);
    return id != 0;
}

bool mqttIsConnected() {
    return _mqttClient.connected();
}
