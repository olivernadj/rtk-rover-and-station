#include "ota_updater.h"
#ifdef OTA_ENABLED
#include "config.h"
#include "wifi_manager.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>

static uint32_t _lastCheck = 0;
static char     _runningMd5[33] = "";   // MD5 of currently running firmware

// Fetch manifest JSON, extract md5 and url for our mode.
// Returns true if a new firmware is available (md5 differs from running).
static bool checkManifest(String& outUrl, String& outMd5) {
    WiFiClientSecure client;
    client.setInsecure();  // Skip cert validation; MD5 check guards integrity

    HTTPClient http;
    http.begin(client, String(OTA_MANIFEST_URL));
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

    if (outMd5.equalsIgnoreCase(_runningMd5)) {
        return false;  // Same version
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
    _runningMd5[0] = '\0';
    _lastCheck = millis();
    Serial.println("[OTA] Initialised, first check in " +
                   String(OTA_CHECK_INTERVAL_MS / 1000) + "s");
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

    if (performOta(url, md5)) {
        strncpy(_runningMd5, md5.c_str(), sizeof(_runningMd5) - 1);
        Serial.println("[OTA] Rebooting in 2 seconds...");
        delay(2000);
        ESP.restart();
    }
}

#endif // OTA_ENABLED
