#include "ota_updater.h"
#ifdef OTA_ENABLED
#include "config.h"
#include "secrets.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "logger.h"
#ifdef MODE_ROVER
#include "ntrip_client.h"
#endif
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_heap_caps.h>
#include <MD5Builder.h>

static uint32_t _lastCheck = 0;
static uint8_t  _failCount = 0;
static String   _lastFailedMd5;
static const uint8_t OTA_MAX_RETRIES = 3;

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
        logMsg("[OTA] Manifest fetch failed: %d", code);
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
        logMsg("[OTA] Mode not found in manifest");
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
        logMsg("[OTA] Invalid MD5 in manifest");
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
    logMsg("[OTA] Downloading %s", fullUrl.c_str());

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client, fullUrl);
    http.setUserAgent(String("OTA-ESP32/1.0 (") + WiFi.getHostname() + ")");
    http.setAuthorization(OTA_USER, OTA_PASSWORD);
    http.setTimeout(60000);
    int code = http.GET();
    if (code != 200) {
        logMsg("[OTA] Download failed: %d", code);
        http.end();
        return false;
    }

    int contentLen = http.getSize();
    if (contentLen <= 0) {
        logMsg("[OTA] Invalid content length");
        http.end();
        return false;
    }

    logMsg("[OTA] Firmware size: %d bytes", contentLen);

    if (!Update.begin(contentLen)) {
        logMsg("[OTA] Not enough space: %s", Update.errorString());
        http.end();
        return false;
    }

    Update.setMD5(expectedMd5.c_str());

    // Stream directly to flash — no PSRAM involved.
    // All buffers (IO, TLS internal) stay in internal SRAM, avoiding the
    // PSRAM cache coherency issues that corrupt data on ESP32-S3.
    static constexpr size_t CHUNK = 2048;
    uint8_t* ioBuf = (uint8_t*)heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ioBuf) {
        logMsg("[OTA] Cannot allocate IO buffer");
        http.end();
        Update.abort();
        return false;
    }

    MD5Builder md5;
    md5.begin();

    WiFiClient* stream = http.getStreamPtr();
    size_t written = 0;
    uint32_t lastActivity = millis();

    while (written < (size_t)contentLen) {
        size_t avail = stream->available();
        if (avail == 0) {
            if (millis() - lastActivity > 30000) {
                logMsg("[OTA] Download timeout (30s no data)");
                break;
            }
            delay(10);
            continue;
        }
        size_t toRead = min(avail, CHUNK);
        size_t bytesRead = stream->readBytes(ioBuf, toRead);
        if (bytesRead == 0) break;
        md5.add(ioBuf, bytesRead);
        size_t n = Update.write(ioBuf, bytesRead);
        if (n != bytesRead) {
            logMsg("[OTA] Flash write error at %u bytes", written);
            break;
        }
        written += n;
        lastActivity = millis();
        delay(2);  // Yield to RTOS
        if (written % (100 * 1024) < CHUNK) {
            logMsg("[OTA] Progress: %u / %d bytes (%u%%)",
                   written, contentLen, (written * 100) / contentLen);
        }
    }

    free(ioBuf);

    if (written != (size_t)contentLen) {
        logMsg("[OTA] Incomplete: %u / %d bytes", written, contentLen);
        Update.abort();
        http.end();
        return false;
    }

    // Verify downloaded bytes match manifest MD5.
    md5.calculate();
    String computedMd5 = md5.toString();
    if (!computedMd5.equalsIgnoreCase(expectedMd5)) {
        Serial.printf("[OTA] Download MD5 mismatch! expected=%s computed=%s\n",
                      expectedMd5.c_str(), computedMd5.c_str());
        Update.abort();
        http.end();
        return false;
    }
    Serial.println("[OTA] Download MD5 verified OK");

    // Shut down WiFi before verification — prevents reconnect ticker from
    // calling WiFi.begin() during esp_image_verify() flash read-back.
    wifiStopReconnect();
    http.end();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    Serial.println("[OTA] Download complete, verifying image...");
    if (!Update.end()) {
        Serial.printf("[OTA] Update failed: %s\n", Update.errorString());
        // Restore WiFi so the device can continue operating
        wifiResumeReconnect();
        return false;
    }

    Serial.println("[OTA] Update successful!");
    return true;
}

void otaInit() {
    _lastCheck = millis();
    logMsg("[OTA] Initialised (fw: %s), first check in %lus",
           FW_VERSION, OTA_CHECK_INTERVAL_MS / 1000);
}

void otaUpdate() {
    if (!wifiIsConnected()) return;

    uint32_t now = millis();
    if (now - _lastCheck < OTA_CHECK_INTERVAL_MS) return;
    _lastCheck = now;

    logMsg("[OTA] Checking for update...");

    String url, md5;
    if (!checkManifest(url, md5)) {
        logMsg("[OTA] No update available");
        return;
    }

    logMsg("[OTA] New firmware available (md5: %s)", md5.c_str());

    if (md5 != _lastFailedMd5) {
        _failCount = 0;  // New firmware on server, reset retries
    }

    if (_failCount >= OTA_MAX_RETRIES) {
        logMsg("[OTA] Skipping — %d consecutive failures, waiting for new manifest", _failCount);
        return;
    }

    logMsg("[OTA] Stopping network services before flash...");
    mqttOnWifiDisconnect();
#ifdef MODE_ROVER
    ntripOnWifiDisconnect();
#endif
    delay(100);  // Let async TCP task settle

    if (performOta(url, md5)) {
        Serial.println("[OTA] Rebooting in 2 seconds...");
        delay(2000);
        ESP.restart();
    } else {
        _failCount++;
        _lastFailedMd5 = md5;
        logMsg("[OTA] Failure %d/%d", _failCount, OTA_MAX_RETRIES);
    }
}

#endif // OTA_ENABLED
