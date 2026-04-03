#ifdef MODE_ROVER

#include "ntrip_client.h"
#include "config.h"
#include "secrets.h"
#include "gnss.h"
#include "wifi_manager.h"
#include <SparkFun_u-blox_GNSS_v3.h>
#include <WiFiClient.h>
#include <Arduino.h>

// ── Base64 encoder ───────────────────────────────────────────────────────────
static String base64Encode(const String& input) {
    static const char* chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String result;
    result.reserve((input.length() + 2) / 3 * 4);
    int i = 0;
    uint8_t c3[3], c4[4];
    int len = (int)input.length();
    const uint8_t* bytes = (const uint8_t*)input.c_str();
    while (len--) {
        c3[i++] = *bytes++;
        if (i == 3) {
            c4[0] = (c3[0] & 0xfc) >> 2;
            c4[1] = ((c3[0] & 0x03) << 4) | ((c3[1] & 0xf0) >> 4);
            c4[2] = ((c3[1] & 0x0f) << 2) | ((c3[2] & 0xc0) >> 6);
            c4[3] = c3[2] & 0x3f;
            for (int j = 0; j < 4; j++) result += chars[c4[j]];
            i = 0;
        }
    }
    if (i) {
        for (int j = i; j < 3; j++) c3[j] = 0;
        c4[0] = (c3[0] & 0xfc) >> 2;
        c4[1] = ((c3[0] & 0x03) << 4) | ((c3[1] & 0xf0) >> 4);
        c4[2] = ((c3[1] & 0x0f) << 2) | ((c3[2] & 0xc0) >> 6);
        c4[3] = c3[2] & 0x3f;
        for (int j = 0; j < i + 1; j++) result += chars[c4[j]];
        while (i++ < 3) result += '=';
    }
    return result;
}

// ── State machine ────────────────────────────────────────────────────────────
enum class NtripState { DISCONNECTED, CONNECTING, HANDSHAKING, STREAMING };

static WiFiClient  _client;
static NtripState  _state          = NtripState::DISCONNECTED;
static uint32_t    _lastDataMs     = 0;
static uint32_t    _reconnectAfter = 0;
static uint8_t     _rtcmBuf[RTCM_BUF_LEN];

static void startConnect() {
    _client.setTimeout(3000);
    if (!_client.connect(NTRIP_HOST, NTRIP_PORT)) {
        _state          = NtripState::DISCONNECTED;
        _reconnectAfter = millis() + NTRIP_RECONNECT_DELAY_MS;
        return;
    }

    String credentials = base64Encode(String(NTRIP_USER) + ":" + NTRIP_PASSWORD);
    String request =
        String("GET /") + NTRIP_MOUNTPOINT + " HTTP/1.1\r\n" +
        "Host: " + NTRIP_HOST + "\r\n" +
        "Ntrip-Version: Ntrip/2.0\r\n" +
        "User-Agent: NTRIP ESP32Client/1.0\r\n" +
        "Authorization: Basic " + credentials + "\r\n" +
        "Connection: close\r\n\r\n";
    _client.print(request);
    _state      = NtripState::HANDSHAKING;
    _lastDataMs = millis();
}

static void processHandshake() {
    if (!_client.available()) {
        if (millis() - _lastDataMs > NTRIP_TIMEOUT_MS) {
            _client.stop();
            _state          = NtripState::DISCONNECTED;
            _reconnectAfter = millis() + NTRIP_RECONNECT_DELAY_MS;
        }
        return;
    }

    _client.setTimeout(200);
    String line = _client.readStringUntil('\n');
    if (line.startsWith("ICY 200") || line.startsWith("HTTP/1.1 200")) {
        // Drain remaining HTTP headers
        while (_client.available()) {
            String h = _client.readStringUntil('\n');
            if (h == "\r" || h.length() == 0) break;
        }
        _state      = NtripState::STREAMING;
        _lastDataMs = millis();
    } else if (line.indexOf("401") >= 0 || line.indexOf("404") >= 0) {
        _client.stop();
        _state          = NtripState::DISCONNECTED;
        _reconnectAfter = millis() + NTRIP_RECONNECT_DELAY_MS;
    }
}

static void processStream() {
    if (!_client.connected()) {
        _client.stop();
        _state          = NtripState::DISCONNECTED;
        _reconnectAfter = millis() + NTRIP_RECONNECT_DELAY_MS;
        return;
    }

    int avail = _client.available();
    if (avail <= 0) {
        if (millis() - _lastDataMs > NTRIP_TIMEOUT_MS) {
            _client.stop();
            _state          = NtripState::DISCONNECTED;
            _reconnectAfter = millis() + NTRIP_RECONNECT_DELAY_MS;
        }
        return;
    }

    size_t toRead = ((size_t)avail < RTCM_BUF_LEN) ? (size_t)avail : RTCM_BUF_LEN;
    size_t nRead  = _client.read(_rtcmBuf, toRead);
    if (nRead > 0) {
        gnssGetHandle().pushRawData(_rtcmBuf, nRead);
        _lastDataMs = millis();
    }
}

// ── Public API ───────────────────────────────────────────────────────────────
void ntripInit() {
    _state          = NtripState::DISCONNECTED;
    _reconnectAfter = 0;
}

void ntripUpdate() {
    if (!wifiIsConnected()) return;

    switch (_state) {
        case NtripState::DISCONNECTED:
            if (millis() >= _reconnectAfter) {
                _state = NtripState::CONNECTING;
                startConnect();
            }
            break;
        case NtripState::CONNECTING:
            break;
        case NtripState::HANDSHAKING:
            processHandshake();
            break;
        case NtripState::STREAMING:
            processStream();
            break;
    }
}

bool ntripIsConnected() {
    return _state == NtripState::STREAMING;
}

#endif // MODE_ROVER
