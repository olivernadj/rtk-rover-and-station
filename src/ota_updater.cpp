#include "ota_updater.h"
#ifdef OTA_ENABLED
#include "config.h"
#include "secrets.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#ifdef MODE_ROVER
#include "ntrip_client.h"
#endif
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>

static uint32_t _lastCheck = 0;

// Fetch manifest JSON, extract md5 and url for our mode.
// Returns true if a new firmware is available (md5 differs from running).
static bool checkManifest(String& outUrl, String& outMd5) {
    WiFiClientSecure client;
    client.setInsecure();  // Skip cert validation; MD5 check guards integrity

    HTTPClient http;
    http.begin(client, String(OTA_MANIFEST_URL));
    http.setUserAgent(String("OTA-ESP32/1.0 (") + WiFi.getHostname() + ")");
    http.setAuthorization(OTA_USER, OTA_PASSWORD);
    http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[OTA] Manifest fetch failed: %d\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    // Minimal JSON parsing — avoid pulling in ArduinoJson.
#ifdef MODE_STATIONARY
    const char* modeKey = "\"stationary\"";
#else
    const char* modeKey = "\"rover\"";
#endif

    int modePos = body.indexOf(modeKey);
    if (modePos < 0) {
        Serial.println("[OTA] Mode not found in manifest");
        return false;
    }

    int md5Key = body.indexOf("\"md5\"", modePos);
    if (md5Key < 0) return false;
    int md5Start = body.indexOf('"', md5Key + 5) + 1;
    int md5End   = body.indexOf('"', md5Start);
    if (md5Start <= 0 || md5End <= md5Start) return false;
    outMd5 = body.substring(md5Start, md5End);

    int urlKey = body.indexOf("\"url\"", modePos);
    if (urlKey < 0) return false;
    int urlStart = body.indexOf('"', urlKey + 5) + 1;
    int urlEnd   = body.indexOf('"', urlStart);
    if (urlStart <= 0 || urlEnd <= urlStart) return false;
    outUrl = body.substring(urlStart, urlEnd);

    if (outMd5.length() != 32) {
        Serial.println("[OTA] Invalid MD5 in manifest");
        return false;
    }

    // Compare version string from manifest against compiled-in FW_VERSION.
    int verKey = body.indexOf("\"version\"", modePos);
    if (verKey >= 0) {
        int verStart = body.indexOf('"', verKey + 9) + 1;
        int verEnd   = body.indexOf('"', verStart);
        if (verStart > 0 && verEnd > verStart) {
            String manifestVer = body.substring(verStart, verEnd);
            if (manifestVer.equals(FW_VERSION)) {
                return false;   // Same version already running
            }
        }
    }

    return true;
}

static bool performOta(const String& url, const String& expectedMd5) {
    String fullUrl = String(OTA_BASE_URL) + url;
    Serial.printf("[OTA] Downloading %s\n", fullUrl.c_str());

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client, fullUrl);
    http.setUserAgent(String("OTA-ESP32/1.0 (") + WiFi.getHostname() + ")");
    http.setAuthorization(OTA_USER, OTA_PASSWORD);
    http.setTimeout(60000);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[OTA] Download failed: %d\n", code);
        http.end();
        return false;
    }

    int contentLen = http.getSize();
    if (contentLen <= 0) {
        Serial.println("[OTA] Invalid content length");
        http.end();
        return false;
    }

    Serial.printf("[OTA] Firmware size: %d bytes\n", contentLen);

    if (!Update.begin(contentLen)) {
        Serial.printf("[OTA] Not enough space: %s\n", Update.errorString());
        http.end();
        return false;
    }

    Update.setMD5(expectedMd5.c_str());

    WiFiClient* stream = http.getStreamPtr();
    size_t written = Update.writeStream(*stream);
    http.end();

    if (written != (size_t)contentLen) {
        Serial.printf("[OTA] Written %u / %d bytes\n", written, contentLen);
        Update.abort();
        return false;
    }

    if (!Update.end()) {
        Serial.printf("[OTA] Update failed: %s\n", Update.errorString());
        return false;
    }

    Serial.println("[OTA] Update successful!");
    return true;
}

void otaInit() {
    _lastCheck = millis();
    Serial.printf("[OTA] Initialised (fw: %s), first check in %lus\n",
                  FW_VERSION, OTA_CHECK_INTERVAL_MS / 1000);
}

void otaUpdate() {
    if (!wifiIsConnected()) return;

    uint32_t now = millis();
    if (now - _lastCheck < OTA_CHECK_INTERVAL_MS) return;
    _lastCheck = now;

    Serial.println("[OTA] Checking for update...");

    String url, md5;
    if (!checkManifest(url, md5)) {
        Serial.println("[OTA] No update available");
        return;
    }

    Serial.printf("[OTA] New firmware available (md5: %s)\n", md5.c_str());

    Serial.println("[OTA] Stopping network services before flash...");
    mqttOnWifiDisconnect();
#ifdef MODE_ROVER
    ntripOnWifiDisconnect();
#endif
    delay(100);  // Let async TCP task settle

    if (performOta(url, md5)) {
        Serial.println("[OTA] Rebooting in 2 seconds...");
        delay(2000);
        ESP.restart();
    }
}

#endif // OTA_ENABLED
