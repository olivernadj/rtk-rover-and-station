#ifdef MODE_STATIONARY

#include "ntrip_broadcaster.h"
#include "config.h"
#include "secrets.h"
#include "gnss.h"
#include "wifi_manager.h"
#include <WiFiClient.h>
#include <Arduino.h>

// ── Per-caster state machine ─────────────────────────────────────────────────
enum class NtripState { DISCONNECTED, CONNECTING, HANDSHAKING, STREAMING };

struct CasterConnection {
    WiFiClient  client;
    NtripState  state          = NtripState::DISCONNECTED;
    uint32_t    lastDataMs     = 0;
    uint32_t    reconnectAfter = 0;
};

static CasterConnection _casters[8]; // supports up to 8 casters; actual count from secrets
static int              _casterCount = 0;
static uint8_t          _rtcmBuf[RTCM_BUF_LEN];

static void casterConnect(int idx) {
    const NtripCasterConfig& cfg = NTRIP_CASTERS[idx];
    CasterConnection& c = _casters[idx];

    Serial.printf("[NTRIP] Connecting to %s:%d/%s\n", cfg.host, cfg.port, cfg.mountpoint);
    c.client.setTimeout(3000);
    if (!c.client.connect(cfg.host, cfg.port)) {
        Serial.printf("[NTRIP] Connection failed to %s:%d\n", cfg.host, cfg.port);
        c.state          = NtripState::DISCONNECTED;
        c.reconnectAfter = millis() + NTRIP_RECONNECT_DELAY_MS;
        return;
    }

    // NTRIP v1 SOURCE protocol (used by Emlid and most casters)
    String request =
        String("SOURCE ") + cfg.password + " /" + cfg.mountpoint + "\r\n" +
        "Source-Agent: NTRIP ESP32Caster/1.0\r\n" +
        "\r\n";
    c.client.print(request);
    c.state      = NtripState::HANDSHAKING;
    c.lastDataMs = millis();
}

static void casterProcessHandshake(int idx) {
    CasterConnection& c = _casters[idx];

    if (!c.client.available()) {
        if (millis() - c.lastDataMs > NTRIP_TIMEOUT_MS) {
            Serial.printf("[NTRIP] Handshake timeout (no response in %lu ms)\n",
                          NTRIP_TIMEOUT_MS);
            c.client.stop();
            c.state          = NtripState::DISCONNECTED;
            c.reconnectAfter = millis() + NTRIP_RECONNECT_DELAY_MS;
        }
        return;
    }

    c.client.setTimeout(200);
    String line = c.client.readStringUntil('\n');
    Serial.printf("[NTRIP] Response: %s\n", line.c_str());
    if (line.startsWith("ICY 200") || line.startsWith("HTTP/1.1 200")) {
        // Drain remaining HTTP headers
        while (c.client.available()) {
            String h = c.client.readStringUntil('\n');
            if (h == "\r" || h.length() == 0) break;
        }
        c.state      = NtripState::STREAMING;
        c.lastDataMs = millis();
    } else if (line.indexOf("401") >= 0 || line.indexOf("404") >= 0) {
        c.client.stop();
        c.state          = NtripState::DISCONNECTED;
        c.reconnectAfter = millis() + NTRIP_RECONNECT_DELAY_MS;
    }
}

static void casterCheckStreamAlive(int idx) {
    CasterConnection& c = _casters[idx];

    if (!c.client.connected()) {
        Serial.printf("[NTRIP] Connection to caster #%d lost\n", idx);
        c.client.stop();
        c.state          = NtripState::DISCONNECTED;
        c.reconnectAfter = millis() + NTRIP_RECONNECT_DELAY_MS;
    }
    // No receive timeout — broadcasting is one-way (we push RTCM, caster sends nothing back)
}

// ── Public API ───────────────────────────────────────────────────────────────
void ntripBroadcasterInit() {
    _casterCount = (NTRIP_CASTER_COUNT < 8) ? NTRIP_CASTER_COUNT : 8;
    for (int i = 0; i < _casterCount; i++) {
        _casters[i].state          = NtripState::DISCONNECTED;
        _casters[i].reconnectAfter = 0;
    }
}

void ntripBroadcasterUpdate() {
    if (!wifiIsConnected()) return;

    // 1. Advance each caster's state machine
    for (int i = 0; i < _casterCount; i++) {
        CasterConnection& c = _casters[i];
        switch (c.state) {
            case NtripState::DISCONNECTED:
                if (millis() >= c.reconnectAfter) {
                    c.state = NtripState::CONNECTING;
                    casterConnect(i);
                }
                break;
            case NtripState::CONNECTING:
                // handled synchronously in casterConnect()
                break;
            case NtripState::HANDSHAKING:
                casterProcessHandshake(i);
                break;
            case NtripState::STREAMING:
                casterCheckStreamAlive(i);
                break;
        }
    }

    // 2. Read RTCM bytes from ZED-F9P once per loop
    size_t nRead = gnssReadRtcm(_rtcmBuf, RTCM_BUF_LEN);
    if (nRead == 0) return;

    static uint32_t _totalRtcm = 0;
    static uint32_t _lastRtcmLog = 0;
    _totalRtcm += nRead;
    if (millis() - _lastRtcmLog >= 5000) {
        Serial.printf("[NTRIP] RTCM: %lu bytes total, last chunk %u bytes\n",
                      _totalRtcm, nRead);
        _lastRtcmLog = millis();
    }

    // 3. Forward to every STREAMING caster
    for (int i = 0; i < _casterCount; i++) {
        if (_casters[i].state == NtripState::STREAMING) {
            _casters[i].client.write(_rtcmBuf, nRead);
            _casters[i].lastDataMs = millis();
        }
    }
}

bool ntripBroadcasterAnyConnected() {
    for (int i = 0; i < _casterCount; i++) {
        if (_casters[i].state == NtripState::STREAMING) return true;
    }
    return false;
}

#endif // MODE_STATIONARY
