#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>
#include <esp_ota_ops.h>
#include <Web3.h>
#include <KeyID.h>
#include "SetupWifiStore.h"

// Configuration from build flags
#ifndef PROXY_BASE_URL
#define PROXY_BASE_URL ""
#endif

#ifndef PROJECT_ID
#define PROJECT_ID ""
#endif

#ifndef APP_NAME
#define APP_NAME ""
#endif

#ifndef FIRMWARE_URL
#define FIRMWARE_URL ""
#endif

#ifndef FIRMWARE_SHA256
#define FIRMWARE_SHA256 ""
#endif

#ifndef AP_SSID_PREFIX
#define AP_SSID_PREFIX "ESP32-Setup"
#endif

#ifndef AP_PASSWORD
#define AP_PASSWORD ""
#endif

// Constants
#define DNS_PORT 53
#define WEB_SERVER_PORT 80
#define MAX_WIFI_CONNECT_ATTEMPTS 3
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define OTA_RETRY_MAX 3
#define OTA_RETRY_DELAY_MS 5000

// Global objects
SetupWifiStore wifiStore;
DNSServer dnsServer;
WebServer webServer(WEB_SERVER_PORT);
KeyID* deviceKey = nullptr;
Web3* web3 = nullptr;

// State
bool wifiConnected = false;
String lastError = "";
String firmwareDownloadUrl = "";
String firmwareSHA256 = "";
size_t firmwareSize = 0;
unsigned long lastScanTime = 0;
bool scanInProgress = false;
int lastScanCount = 0;
bool otaInProgress = false;
String otaStatus = "";
String lastFailedSSID = "";
String lastFailReason = "";

// Forward declarations
void setupAccessPoint();
void setupWebServer();
void initializeDeviceKey();
bool connectToStoredNetworks();
bool queryProxyForFirmware();
bool downloadAndFlashFirmware();
void startWifiScan();
void checkScanProgress();
String scanNetworks();
void handleRoot();
void handleScan();
void handleSave();
void handleList();
void handleRemove();
void handleDone();
void handleStatus();
void handleNotFound();
bool verifyFirmwareSHA256(const String& sha256);
bool downloadAndVerifyFirmware(const String& url, const String& expectedSHA256);

#ifdef TEST_PROVISION
void runTestProvisioning();
#endif

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n=== ESP32 Web3E Setup Firmware ===");
    
#ifdef TEST_PROVISION
    Serial.println("Mode: TEST_PROVISION (Hardware Test Harness)");
    Serial.println("Initializing...");
    
    // CRITICAL ORDER: Initialize KeyID first, THEN WifiStore
    // Web3E's KeyID calls EEPROM.begin(33), which in arduino-esp32 3.x
    // TRUNCATES the NVS blob if called after a larger EEPROM.begin().
    // WifiStore.begin() must be called AFTER KeyID to set the final size.
    // DO NOT reorder without understanding this truncation hazard.
    
    // Run test provisioning (creates KeyID internally, then initializes WifiStore)
    runTestProvisioning();
    // Never returns to normal flow
    return;
#else
    Serial.println("Mode: Production Setup");
    Serial.println("Initializing...");
    
    // CRITICAL ORDER: Initialize KeyID first, THEN WifiStore
    // Web3E's KeyID calls EEPROM.begin(33), which in arduino-esp32 3.x
    // TRUNCATES the NVS blob if called after a larger EEPROM.begin().
    // WifiStore.begin() must be called AFTER KeyID to set the final size.
    // DO NOT reorder without understanding this truncation hazard.
    
    // Initialize device key (Web3E KeyID) - calls EEPROM.begin(33)
    initializeDeviceKey();
    
    // Initialize WifiStore - must come AFTER KeyID to expand EEPROM size
    if (!wifiStore.begin()) {
        Serial.println("ERROR: Failed to initialize EEPROM for WifiStore");
        ESP.restart();
    }
    
    // Try to connect to stored Wi-Fi networks
    if (connectToStoredNetworks()) {
        Serial.println("Connected to Wi-Fi!");
        wifiConnected = true;
        
        // Query proxy or use direct URL
        if (queryProxyForFirmware()) {
            Serial.println("Firmware metadata retrieved, starting OTA update...");
            if (downloadAndFlashFirmware()) {
                Serial.println("OTA update successful! Rebooting...");
                delay(2000);
                ESP.restart();
            } else {
                Serial.println("OTA update failed, starting captive portal for recovery");
                wifiConnected = false;
            }
        } else {
            Serial.println("Failed to get firmware metadata, starting captive portal");
            wifiConnected = false;
        }
    }
    
    // If we reach here, either no networks stored or connection failed
    Serial.println("Starting captive portal for Wi-Fi configuration...");
    setupAccessPoint();
    setupWebServer();
    
    // Start initial Wi-Fi scan
    Serial.println("Starting initial Wi-Fi scan...");
    startWifiScan();
#endif
}

void loop() {
    dnsServer.processNextRequest();
    webServer.handleClient();
    
    // Check scan progress and restart periodically
    checkScanProgress();
    
    // Check if Wi-Fi was configured and we should retry OTA
    if (wifiConnected && firmwareDownloadUrl.length() > 0) {
        delay(1000);
        if (downloadAndFlashFirmware()) {
            Serial.println("OTA update successful! Rebooting...");
            delay(2000);
            ESP.restart();
        } else {
            Serial.println("OTA update failed, waiting for retry...");
            delay(OTA_RETRY_DELAY_MS);
        }
    }
}

void initializeDeviceKey() {
    Serial.println("\n--- Initializing Device Key (Web3E KeyID) ---");
    
    // Web3E KeyID handles EEPROM initialization and key generation/recovery
    // It uses bytes 0-32 of EEPROM (1 marker byte + 32-byte private key)
    web3 = new Web3(1); // Chain ID doesn't matter for key generation
    deviceKey = new KeyID(web3);
    
    if (deviceKey->hasRecoveredKey()) {
        Serial.println("Device key recovered from EEPROM");
    } else {
        Serial.println("New device key generated");
    }
    
    Serial.print("Device Address: ");
    Serial.println(deviceKey->getAddress().c_str());
    Serial.println("--- Device Key Ready ---\n");
}

bool connectToStoredNetworks() {
    WifiCredential networks[MAX_WIFI_NETWORKS];
    int count = wifiStore.readNetworks(networks, MAX_WIFI_NETWORKS);
    
    if (count == 0) {
        Serial.println("No stored Wi-Fi networks found");
        lastFailReason = "No networks saved";
        return false;
    }
    
    Serial.printf("Found %d stored network(s), attempting to connect...\n", count);
    
    // Stop any in-progress scan before connecting
    if (scanInProgress) {
        int scanResult = WiFi.scanComplete();
        if (scanResult == WIFI_SCAN_RUNNING) {
            Serial.println("Stopping in-progress scan before connecting...");
            WiFi.scanDelete();
        }
        scanInProgress = false;
    }
    
    // Stay in AP+STA mode if AP is active (for status updates)
    if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
        WiFi.mode(WIFI_AP_STA);
        Serial.println("Connecting in AP+STA mode (AP stays active for status)");
    } else {
        WiFi.mode(WIFI_STA);
    }
    
    for (int i = 0; i < count; i++) {
        Serial.printf("\nTrying network %d: '%s' (SSID length=%d, password length=%d)\n", 
                      i + 1, networks[i].ssid, networks[i].ssidLen, networks[i].pwLen);
        
        // Disconnect from previous attempt
        WiFi.disconnect(true);
        delay(100);
        
        if (networks[i].pwLen > 0) {
            WiFi.begin(networks[i].ssid, networks[i].password);
        } else {
            WiFi.begin(networks[i].ssid);
        }
        
        unsigned long startTime = millis();
        unsigned long timeout = 20000; // 20 second timeout per network
        wl_status_t lastStatus = WL_IDLE_STATUS;
        
        while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < timeout) {
            wl_status_t currentStatus = WiFi.status();
            if (currentStatus != lastStatus) {
                Serial.printf(" [status: %d] ", currentStatus);
                lastStatus = currentStatus;
            }
            delay(500);
            Serial.print(".");
        }
        Serial.println();
        
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("Connected!");
            Serial.print("IP Address: ");
            Serial.println(WiFi.localIP());
            lastFailedSSID = "";
            lastFailReason = "";
            return true;
        } else {
            wl_status_t finalStatus = WiFi.status();
            String statusStr = "Unknown";
            switch (finalStatus) {
                case WL_NO_SSID_AVAIL: statusStr = "Network not found"; break;
                case WL_CONNECT_FAILED: statusStr = "Connection failed (wrong password?)"; break;
                case WL_CONNECTION_LOST: statusStr = "Connection lost"; break;
                case WL_DISCONNECTED: statusStr = "Disconnected"; break;
                case WL_IDLE_STATUS: statusStr = "Idle/timeout"; break;
                default: statusStr = "Status " + String(finalStatus); break;
            }
            
            Serial.printf("Failed to connect to '%s': %s (WiFi.status=%d)\n", 
                          networks[i].ssid, statusStr.c_str(), finalStatus);
            
            lastFailedSSID = String(networks[i].ssid);
            lastFailReason = statusStr;
            otaStatus = "Failed to connect to '" + lastFailedSSID + "': " + lastFailReason;
            
            WiFi.disconnect(true);
            delay(100);
        }
    }
    
    return false;
}

bool queryProxyForFirmware() {
    String proxyUrl = String(PROXY_BASE_URL);
    String projectId = String(PROJECT_ID);
    String directUrl = String(FIRMWARE_URL);
    String directSHA = String(FIRMWARE_SHA256);
    
    // Check if direct URL override is configured
    if (directUrl.length() > 0) {
        Serial.println("Using direct firmware URL (override mode)");
        firmwareDownloadUrl = directUrl;
        firmwareSHA256 = directSHA;
        firmwareSize = 0; // Unknown
        return true;
    }
    
    // Check if proxy is configured
    if (proxyUrl.length() == 0 || projectId.length() == 0) {
        Serial.println("ERROR: No PROXY_BASE_URL/PROJECT_ID or FIRMWARE_URL configured");
        lastError = "No firmware source configured in build flags";
        return false;
    }
    
    // Query SmartLayerProxy
    String apiUrl = proxyUrl + "/api/firmware/" + projectId + "/latest";
    Serial.println("Querying SmartLayerProxy: " + apiUrl);
    
    HTTPClient http;
    http.begin(apiUrl);
    http.setTimeout(10000);
    
    int httpCode = http.GET();
    
    if (httpCode != 200) {
        Serial.printf("ERROR: Proxy returned HTTP %d\n", httpCode);
        lastError = "Proxy API error: HTTP " + String(httpCode);
        http.end();
        return false;
    }
    
    String payload = http.getString();
    http.end();
    
    Serial.println("Proxy response received, parsing JSON...");
    
    // Parse JSON response
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
        Serial.println("ERROR: Failed to parse JSON response");
        lastError = "Invalid JSON from proxy";
        return false;
    }
    
    // Extract firmware metadata
    if (!doc["url"].is<String>() || !doc["sha256"].is<String>()) {
        Serial.println("ERROR: Missing required fields in proxy response");
        lastError = "Invalid proxy response format";
        return false;
    }
    
    firmwareDownloadUrl = doc["url"].as<String>();
    firmwareSHA256 = doc["sha256"].as<String>();
    firmwareSize = doc["size"].is<size_t>() ? doc["size"].as<size_t>() : 0;
    
    Serial.println("Firmware metadata:");
    Serial.println("  Project: " + doc["project"].as<String>());
    Serial.println("  Version: " + doc["version"].as<String>());
    Serial.println("  URL: " + firmwareDownloadUrl);
    Serial.println("  SHA256: " + firmwareSHA256);
    Serial.printf("  Size: %zu bytes\n", firmwareSize);
    
    return true;
}

bool downloadAndVerifyFirmware(const String& url, const String& expectedSHA256) {
    Serial.println("\n=== Starting OTA Download with SHA-256 Verification ===");
    Serial.println("Downloading from: " + url);
    if (expectedSHA256.length() == 64) {
        Serial.println("Expected SHA-256: " + expectedSHA256);
    } else {
        Serial.println("WARNING: No SHA-256 provided, skipping verification");
    }
    
    WiFiClientSecure client;
    client.setInsecure();
    
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(30000);
    
    int httpCode = http.GET();
    if (httpCode != 200) {
        Serial.printf("ERROR: HTTP GET failed with code %d\n", httpCode);
        lastError = "Download failed: HTTP " + String(httpCode);
        http.end();
        return false;
    }
    
    int contentLength = http.getSize();
    if (contentLength <= 0) {
        Serial.println("ERROR: Invalid content length");
        lastError = "Invalid content length";
        http.end();
        return false;
    }
    
    Serial.printf("Content-Length: %d bytes\n", contentLength);
    
    if (firmwareSize > 0 && contentLength != (int)firmwareSize) {
        Serial.printf("WARNING: Size mismatch. Expected: %zu, Got: %d\n", firmwareSize, contentLength);
    }
    
    // Check available OTA partition size BEFORE attempting update
    size_t updateSize = Update.size();
    if (updateSize == 0) {
        // Get the next OTA partition info
        const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
        if (partition != NULL) {
            updateSize = partition->size;
        }
    }
    
    if (updateSize > 0 && contentLength > (int)updateSize) {
        Serial.println("\n=== OTA PARTITION SIZE MISMATCH ===");
        Serial.printf("ERROR: Firmware too large for OTA partition!\n");
        Serial.printf("  Target firmware size: %d bytes (%.2f MB)\n", contentLength, contentLength / 1048576.0);
        Serial.printf("  Available OTA slot:   %zu bytes (%.2f MB)\n", updateSize, updateSize / 1048576.0);
        Serial.println("\nCAUSE: Setup firmware partition table does not match target project firmware.");
        Serial.println("FIX: The setup firmware must be built with the SAME partition table");
        Serial.println("     as the target project firmware (e.g., min_spiffs.csv).");
        Serial.println("     Edit platformio.ini -> board_build.partitions, rebuild, and");
        Serial.println("     re-flash setup firmware via USB to write the correct partition table.");
        Serial.println("=====================================\n");
        lastError = "Firmware too large: partition table mismatch";
        http.end();
        return false;
    }
    
    // Initialize Update
    if (!Update.begin(contentLength)) {
        Serial.printf("ERROR: Update.begin() failed - %s\n", Update.errorString());
        lastError = "Update.begin() failed";
        http.end();
        return false;
    }
    
    // Initialize SHA-256 context
    mbedtls_sha256_context sha256_ctx;
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0);
    
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buffer[512];
    int written = 0;
    int lastProgress = -1;
    
    Serial.println("Downloading and computing SHA-256...");
    
    while (http.connected() && written < contentLength) {
        size_t available = stream->available();
        if (available) {
            int bytesToRead = (available > sizeof(buffer)) ? sizeof(buffer) : available;
            int bytesRead = stream->readBytes(buffer, bytesToRead);
            
            if (bytesRead > 0) {
                // Write to OTA partition
                if (Update.write(buffer, bytesRead) != bytesRead) {
                    Serial.println("ERROR: Update.write() failed");
                    lastError = "Write failed";
                    Update.abort();
                    mbedtls_sha256_free(&sha256_ctx);
                    http.end();
                    return false;
                }
                
                // Update SHA-256 hash
                mbedtls_sha256_update(&sha256_ctx, buffer, bytesRead);
                
                written += bytesRead;
                
                int progress = (written * 100) / contentLength;
                if (progress != lastProgress && progress % 10 == 0) {
                    Serial.printf("Progress: %d%% (%d/%d bytes)\n", progress, written, contentLength);
                    otaStatus = "Downloading firmware: " + String(progress) + "%";
                    lastProgress = progress;
                }
            }
        } else {
            delay(10);
        }
    }
    
    http.end();
    
    if (written != contentLength) {
        Serial.printf("ERROR: Incomplete download. Expected: %d, Got: %d\n", contentLength, written);
        lastError = "Incomplete download";
        Update.abort();
        mbedtls_sha256_free(&sha256_ctx);
        return false;
    }
    
    // Finalize SHA-256
    uint8_t computedHash[32];
    mbedtls_sha256_finish(&sha256_ctx, computedHash);
    mbedtls_sha256_free(&sha256_ctx);
    
    // Convert to hex string
    char computedHashHex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(&computedHashHex[i * 2], "%02x", computedHash[i]);
    }
    computedHashHex[64] = '\0';
    
    Serial.println("Download complete!");
    Serial.print("Computed SHA-256: ");
    Serial.println(computedHashHex);
    
    otaStatus = "Verifying firmware...";
    
    // Verify SHA-256 if expected hash provided
    if (expectedSHA256.length() == 64) {
        String computedStr = String(computedHashHex);
        computedStr.toLowerCase();
        String expectedStr = expectedSHA256;
        expectedStr.toLowerCase();
        
        if (computedStr != expectedStr) {
            Serial.println("ERROR: SHA-256 MISMATCH!");
            Serial.println("Expected: " + expectedStr);
            Serial.println("Computed: " + computedStr);
            lastError = "SHA-256 verification failed";
            Update.abort();
            return false;
        }
        
        Serial.println("SHA-256 verification: PASSED");
        otaStatus = "Verification passed! Flashing firmware...";
    } else {
        Serial.println("WARNING: SHA-256 verification skipped (no expected hash)");
        otaStatus = "Flashing firmware...";
    }
    
    // Commit the update
    if (!Update.end(true)) {
        Serial.println("ERROR: Update.end() failed");
        lastError = "Update commit failed";
        otaStatus = "Failed: Update commit failed";
        otaInProgress = false;
        return false;
    }
    
    Serial.println("Update verified and flashed successfully!");
    otaStatus = "Update complete! Rebooting...";
    otaInProgress = false;
    return true;
}

bool downloadAndFlashFirmware() {
    if (firmwareDownloadUrl.length() == 0) {
        Serial.println("ERROR: No firmware URL available");
        return false;
    }
    
    return downloadAndVerifyFirmware(firmwareDownloadUrl, firmwareSHA256);
}

void setupAccessPoint() {
    WiFi.mode(WIFI_AP);
    
    String apSSID = String(AP_SSID_PREFIX) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX).substring(0, 4);
    apSSID.toUpperCase();
    
    String apPassword = String(AP_PASSWORD);
    
    if (apPassword.length() > 0) {
        WiFi.softAP(apSSID.c_str(), apPassword.c_str());
        Serial.println("Access Point started: " + apSSID);
        Serial.println("Password: " + apPassword);
    } else {
        WiFi.softAP(apSSID.c_str());
        Serial.println("Access Point started (OPEN): " + apSSID);
    }
    
    IPAddress apIP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(apIP, gateway, subnet);
    
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());
    
    // Start DNS server for captive portal
    dnsServer.start(DNS_PORT, "*", apIP);
    Serial.println("DNS server started for captive portal");
}

void setupWebServer() {
    webServer.on("/", handleRoot);
    webServer.on("/scan", handleScan);
    webServer.on("/save", HTTP_POST, handleSave);
    webServer.on("/list", handleList);
    webServer.on("/remove", HTTP_POST, handleRemove);
    webServer.on("/done", HTTP_POST, handleDone);
    webServer.on("/status", handleStatus);
    webServer.onNotFound(handleNotFound);
    
    webServer.begin();
    Serial.println("Web server started on port 80");
}

void startWifiScan() {
    if (!scanInProgress) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.scanNetworks(true);
        scanInProgress = true;
        lastScanTime = millis();
        Serial.println("Wi-Fi scan started (async)");
    }
}

void checkScanProgress() {
    if (scanInProgress) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            lastScanCount = n;
            scanInProgress = false;
            Serial.printf("Wi-Fi scan complete: %d networks found\n", n);
        } else if (n == WIFI_SCAN_FAILED) {
            scanInProgress = false;
            Serial.println("Wi-Fi scan failed");
        }
    }
    
    if (!scanInProgress && (millis() - lastScanTime > 12000)) {
        startWifiScan();
    }
}

String scanNetworks() {
    int n = WiFi.scanComplete();
    
    if (n == WIFI_SCAN_RUNNING) {
        return "{\"scanning\":true,\"networks\":[]}";
    } else if (n == WIFI_SCAN_FAILED) {
        return "{\"scanning\":false,\"networks\":[]}";
    } else if (n >= 0) {
        String json = "{\"scanning\":false,\"networks\":[";
        
        bool seen[n];
        for (int i = 0; i < n; i++) seen[i] = false;
        
        for (int i = 0; i < n; i++) {
            if (seen[i]) continue;
            
            String ssid = WiFi.SSID(i);
            int bestRSSI = WiFi.RSSI(i);
            int bestIdx = i;
            bool encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            
            for (int j = i + 1; j < n; j++) {
                if (!seen[j] && WiFi.SSID(j) == ssid) {
                    seen[j] = true;
                    if (WiFi.RSSI(j) > bestRSSI) {
                        bestRSSI = WiFi.RSSI(j);
                        bestIdx = j;
                    }
                }
            }
            
            seen[bestIdx] = true;
            
            if (i > 0) json += ",";
            json += "{\"ssid\":\"" + ssid + "\",";
            json += "\"rssi\":" + String(bestRSSI) + ",";
            json += "\"encrypted\":" + String(encrypted ? "true" : "false") + "}";
        }
        
        json += "]}";
        return json;
    }
    
    return "{\"scanning\":false,\"networks\":[]}";
}

void handleRoot() {
    String deviceAddr = deviceKey->getAddress().c_str();
    String appName = String(APP_NAME);
    if (appName.length() == 0) {
        appName = String(PROJECT_ID);
        if (appName.length() == 0) {
            appName = "Device";
        }
    }
    
    String html = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>" + appName + " Setup</title><style>"
        "body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5}"
        ".container{max-width:600px;margin:0 auto;background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}"
        "h1{color:#333;margin-top:0;margin-bottom:5px}"
        ".subtitle{color:#666;font-size:14px;margin-top:0;margin-bottom:20px}"
        "h2{color:#666;font-size:18px;margin-top:30px;border-bottom:2px solid #ddd;padding-bottom:5px}"
        "button{background:#007bff;color:white;border:none;padding:10px 20px;border-radius:4px;cursor:pointer;font-size:14px;margin:5px}"
        "button:hover{background:#0056b3}"
        "button.danger{background:#dc3545}"
        "button.danger:hover{background:#c82333}"
        "button.success{background:#28a745}"
        "button.success:hover{background:#218838}"
        "input,select{width:100%;padding:10px;margin:5px 0;border:1px solid #ddd;border-radius:4px;box-sizing:border-box}"
        ".pwd-wrapper{position:relative;display:block;margin-bottom:10px}"
        ".pwd-wrapper input{width:100%;padding-right:45px}"
        ".pwd-toggle{position:absolute;right:5px;top:50%;transform:translateY(-50%);background:#6c757d;color:white;border:none;padding:5px 10px;border-radius:3px;cursor:pointer;font-size:12px;font-weight:bold}"
        ".pwd-toggle:hover{background:#5a6268}"
        "label{display:block;margin-top:10px;color:#555;font-weight:bold}"
        ".network-list{margin-top:10px}"
        ".spinner{display:inline-block;width:14px;height:14px;border:2px solid #fff;border-top-color:transparent;border-radius:50%;animation:spin 0.8s linear infinite;margin-right:8px}"
        "@keyframes spin{to{transform:rotate(360deg)}}"
        ".saved-item{padding:10px;margin:5px 0;background:#fff3cd;border-radius:4px;display:flex;justify-content:space-between;align-items:center}"
        ".status{padding:10px;margin:10px 0;border-radius:4px;display:none}"
        ".status.success{background:#d4edda;color:#155724;border:1px solid #c3e6cb;display:block}"
        ".status.error{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb;display:block}"
        ".status.info{background:#d1ecf1;color:#0c5460;border:1px solid #bee5eb;display:block}"
        ".hidden{display:none}"
        ".device-info{background:#e7f3ff;padding:10px;border-radius:4px;margin-bottom:20px;font-size:12px;color:#004085}"
        ".scanning{color:#6c757d;font-style:italic;font-size:14px}"
        "#network-pwd-section{margin-top:15px}"
        "</style></head><body><div class=\"container\">"
        "<h1>" + appName + " Setup</h1>"
        "<p class=\"subtitle\">Web3E Setup</p>"
        "<div class=\"device-info\"><strong>Device Address:</strong> " + deviceAddr + "</div>"
        "<div id=\"status\" class=\"status\"></div>"
        "<h2>Select Network</h2>"
        "<label>Available Networks <span id=\"scan-status\" class=\"scanning\">Scanning...</span></label>"
        "<select id=\"network-select\" onchange=\"onNetworkSelected()\">"
        "<option value=\"\">-- Select a network --</option>"
        "</select>"
        "<div id=\"network-pwd-section\" class=\"hidden\">"
        "<label>Password</label>"
        "<div class=\"pwd-wrapper\"><input type=\"password\" id=\"network-password\" placeholder=\"Enter password\">"
        "<button class=\"pwd-toggle\" onclick=\"togglePassword('network-password')\">👁</button></div>"
        "<button onclick=\"addSelectedNetwork()\">Connect & Save</button>"
        "</div>"
        "<h2>Add Hidden Network</h2><label>SSID:</label>"
        "<input type=\"text\" id=\"hidden-ssid\" placeholder=\"Network name\">"
        "<label>Password:</label>"
        "<div class=\"pwd-wrapper\"><input type=\"password\" id=\"hidden-password\" placeholder=\"Leave empty for open networks\">"
        "<button class=\"pwd-toggle\" onclick=\"togglePassword('hidden-password')\">👁</button></div>"
        "<button onclick=\"addHiddenNetwork()\">Add to List</button>"
        "<h2>Saved Networks</h2>"
        "<div id=\"saved-networks\" class=\"network-list\"></div>"
        "<button id=\"done-btn\" class=\"success\" onclick=\"commitAndConnect()\" style=\"width:100%;margin-top:20px;font-size:16px\">Done - Connect & Update Firmware</button>"
        "</div><script>"
        "let networksData=[];"
        "let networksBySsid={};"
        "let statusPollInterval=null;"
        "let selectFocused=false;"
        "let userHasSelected=false;"
        "function showStatus(msg,type){"
        "const s=document.getElementById('status');s.textContent=msg;"
        "s.className='status '+(type||'success');"
        "setTimeout(()=>{s.className='status'},5000);}"
        "function togglePassword(id){"
        "const inp=document.getElementById(id);"
        "inp.type=inp.type==='password'?'text':'password';}"
        "function pollNetworks(){"
        "if(selectFocused){return;}"
        "fetch('/scan').then(r=>r.json()).then(data=>{"
        "const status=document.getElementById('scan-status');"
        "const sel=document.getElementById('network-select');"
        "if(data.scanning){status.textContent='Scanning...';return;}"
        "status.textContent='';"
        "if(!data.networks||data.networks.length===0){return;}"
        "const currentSSID=sel.value;"
        "networksData=data.networks;"
        "networksBySsid={};"
        "data.networks.forEach(net=>{networksBySsid[net.ssid]=net;});"
        "sel.innerHTML='<option value=\"\">-- Select a network --</option>';"
        "data.networks.forEach(net=>{"
        "const signal=net.rssi>-50?'▂▄▆█':(net.rssi>-70?'▂▄▆_':'▂▄__');"
        "const opt=document.createElement('option');"
        "opt.value=net.ssid;"
        "opt.textContent=net.ssid+' '+signal+(net.encrypted?' 🔒':'');"
        "opt.dataset.encrypted=net.encrypted;"
        "sel.appendChild(opt);});"
        "if(currentSSID&&networksBySsid[currentSSID]){sel.value=currentSSID;}"
        "}).catch(e=>console.log(e));}"
        "document.getElementById('network-select').addEventListener('focus',()=>{selectFocused=true;});"
        "document.getElementById('network-select').addEventListener('blur',()=>{selectFocused=false;});"
        "function onNetworkSelected(){"
        "const sel=document.getElementById('network-select');"
        "const pwdSection=document.getElementById('network-pwd-section');"
        "if(!sel.value){pwdSection.classList.add('hidden');userHasSelected=false;return;}"
        "userHasSelected=true;"
        "const net=networksBySsid[sel.value];"
        "if(!net){pwdSection.classList.add('hidden');return;}"
        "if(net.encrypted){pwdSection.classList.remove('hidden');"
        "document.getElementById('network-password').value='';"
        "}else{pwdSection.classList.add('hidden');}}"
        "function addSelectedNetwork(){"
        "const sel=document.getElementById('network-select');"
        "const ssid=sel.value;"
        "if(!ssid){showStatus('Please select a network','error');return;}"
        "const net=networksBySsid[ssid];"
        "if(!net){showStatus('Network not found','error');return;}"
        "const pwd=net.encrypted?document.getElementById('network-password').value:'';"
        "if(net.encrypted&&!pwd){showStatus('Please enter password','error');return;}"
        "addNetwork(ssid,pwd);}"
        "function addHiddenNetwork(){"
        "const ssid=document.getElementById('hidden-ssid').value.trim();"
        "const pwd=document.getElementById('hidden-password').value;"
        "if(!ssid){showStatus('Please enter SSID','error');return;}"
        "addNetwork(ssid,pwd);"
        "document.getElementById('hidden-ssid').value='';"
        "document.getElementById('hidden-password').value='';}"
        "function addNetwork(ssid,pwd){"
        "const formData=new FormData();formData.append('ssid',ssid);formData.append('password',pwd);"
        "fetch('/save',{method:'POST',body:formData})"
        ".then(r=>r.text()).then(msg=>{showStatus(msg,'success');loadSavedNetworks();})"
        ".catch(e=>showStatus('Failed to add network','error'));}"
        "function loadSavedNetworks(){"
        "fetch('/list').then(r=>r.json()).then(data=>{"
        "const div=document.getElementById('saved-networks');"
        "if(data.length===0){div.innerHTML='<p>No networks saved yet. Add networks above, then click Done.</p>';return;}"
        "div.innerHTML='';"
        "data.forEach((net,idx)=>{"
        "const item=document.createElement('div');item.className='saved-item';"
        "item.innerHTML='<span><strong>'+net.ssid+'</strong> '+(net.pwLen>0?'🔒':'')+'</span>'"
        "+'<button class=\"danger\" onclick=\"removeNetwork('+idx+')\">Remove</button>';"
        "div.appendChild(item);});}).catch(e=>console.log(e));}"
        "function removeNetwork(idx){if(!confirm('Remove this network?'))return;"
        "const formData=new FormData();formData.append('index',idx);"
        "fetch('/remove',{method:'POST',body:formData})"
        ".then(r=>r.text()).then(msg=>{showStatus(msg,'success');loadSavedNetworks();})"
        ".catch(e=>showStatus('Failed to remove','error'));}"
        "function commitAndConnect(){"
        "if(!confirm('Commit saved networks and start firmware update?'))return;"
        "const btn=document.getElementById('done-btn');"
        "btn.disabled=true;"
        "btn.innerHTML='<span class=\"spinner\"></span>Firmware update in progress...';"
        "showStatus('Connecting to Wi-Fi and downloading firmware...','info');"
        "fetch('/done',{method:'POST'})"
        ".then(r=>r.text()).then(msg=>{"
        "showStatus(msg,'success');"
        "startStatusPolling();"
        "}).catch(e=>{"
        "showStatus('Failed to start update','error');"
        "btn.disabled=false;"
        "btn.innerHTML='Done - Connect & Update Firmware';});}"
        "function startStatusPolling(){"
        "if(statusPollInterval)clearInterval(statusPollInterval);"
        "statusPollInterval=setInterval(()=>{"
        "fetch('/status').then(r=>r.json()).then(data=>{"
        "if(data.inProgress){"
        "showStatus(data.status,'info');"
        "}else if(data.status){"
        "showStatus(data.status,data.error?'error':'success');"
        "clearInterval(statusPollInterval);"
        "}}).catch(e=>{});},2000);}"
        "pollNetworks();"
        "setInterval(pollNetworks,3000);"
        "loadSavedNetworks();"
        "</script></body></html>";
    
    webServer.send(200, "text/html", html);
}

void handleScan() {
    webServer.send(200, "application/json", scanNetworks());
}

void handleSave() {
    if (!webServer.hasArg("ssid")) {
        webServer.send(400, "text/plain", "Missing SSID");
        return;
    }
    
    String ssid = webServer.arg("ssid");
    String password = webServer.hasArg("password") ? webServer.arg("password") : "";
    
    // Log lengths for debugging (never log actual credentials)
    Serial.printf("Adding network: SSID='%s' (length=%d), password length=%d\n", 
                  ssid.c_str(), ssid.length(), password.length());
    
    // Verify no URL encoding artifacts
    if (ssid.indexOf('+') >= 0 || ssid.indexOf('%') >= 0) {
        Serial.println("WARNING: SSID may contain URL-encoded characters");
    }
    
    if (wifiStore.appendNetwork(ssid.c_str(), password.c_str())) {
        Serial.printf("Network added to list: '%s' (length %d)\n", ssid.c_str(), ssid.length());
        String response = "Saved " + ssid;
        webServer.send(200, "text/plain", response);
    } else {
        Serial.println("Failed to add network (storage full or error)");
        webServer.send(500, "text/plain", "Failed to add network (storage may be full)");
    }
}

void handleList() {
    WifiCredential networks[MAX_WIFI_NETWORKS];
    int count = wifiStore.readNetworks(networks, MAX_WIFI_NETWORKS);
    
    String json = "[";
    for (int i = 0; i < count; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + String(networks[i].ssid) + "\",";
        json += "\"pwLen\":" + String(networks[i].pwLen) + "}";
    }
    json += "]";
    
    webServer.send(200, "application/json", json);
}

void handleRemove() {
    if (!webServer.hasArg("index")) {
        webServer.send(400, "text/plain", "Missing index");
        return;
    }
    
    int index = webServer.arg("index").toInt();
    
    if (wifiStore.removeNetwork(index)) {
        Serial.printf("Network at index %d removed\n", index);
        webServer.send(200, "text/plain", "Network removed");
    } else {
        webServer.send(500, "text/plain", "Failed to remove network");
    }
}

void handleDone() {
    Serial.println("\n=== User clicked Done - Attempting connection ===");
    
    int count = wifiStore.getNetworkCount();
    if (count == 0) {
        Serial.println("No networks saved");
        webServer.send(400, "text/plain", "No networks saved. Add at least one network first.");
        return;
    }
    
    Serial.printf("Attempting to connect to %d saved network(s)...\n", count);
    
    otaInProgress = true;
    otaStatus = "Connecting to Wi-Fi...";
    lastFailedSSID = "";
    lastFailReason = "";
    
    webServer.send(200, "text/plain", "Connecting to saved networks...");
    
    // Keep AP active during connection attempts for status updates
    // connectToStoredNetworks will handle mode switching
    
    if (connectToStoredNetworks()) {
        Serial.println("Connected to Wi-Fi!");
        wifiConnected = true;
        otaStatus = "Connected! Querying for firmware...";
        
        if (queryProxyForFirmware()) {
            Serial.println("Firmware metadata retrieved, starting OTA update...");
            otaStatus = "Downloading firmware...";
        } else {
            Serial.println("Failed to query firmware metadata: " + lastError);
            otaStatus = "Failed: " + lastError;
            otaInProgress = false;
        }
    } else {
        Serial.println("Failed to connect to any saved network");
        Serial.printf("Last failed SSID: '%s', Reason: %s\n", lastFailedSSID.c_str(), lastFailReason.c_str());
        // otaStatus already set in connectToStoredNetworks with details
        otaInProgress = false;
        // Don't call setupAccessPoint() - AP is still running in AP+STA mode
    }
}

void handleStatus() {
    String json = "{\"inProgress\":" + String(otaInProgress ? "true" : "false");
    json += ",\"status\":\"" + otaStatus + "\"";
    json += ",\"error\":" + String((otaStatus.indexOf("Failed") >= 0 || otaStatus.indexOf("failed") >= 0) ? "true" : "false");
    if (lastFailedSSID.length() > 0) {
        json += ",\"failedSSID\":\"" + lastFailedSSID + "\"";
        json += ",\"failReason\":\"" + lastFailReason + "\"";
    }
    json += "}";
    webServer.send(200, "application/json", json);
}

void handleNotFound() {
    webServer.sendHeader("Location", "/", true);
    webServer.send(302, "text/plain", "");
}

#ifdef TEST_PROVISION
void runTestProvisioning() {
    Serial.println("\n=== Hardware Test Provisioning ===");
    
    // Check that test credentials are defined
    #ifndef TEST_WIFI_SSID
    #error "TEST_WIFI_SSID not defined - test_secrets.ini missing?"
    #endif
    #ifndef TEST_WIFI_PASS
    #error "TEST_WIFI_PASS not defined - test_secrets.ini missing?"
    #endif
    
    // Step 1: Ensure device key exists (KeyID must be created first)
    Serial.println("Step 1: Initialize device key...");
    web3 = new Web3(1);
    deviceKey = new KeyID(web3);  // Calls EEPROM.begin(33)
    
    if (!deviceKey->hasRecoveredKey()) {
        Serial.println("ERROR: Failed to initialize device key");
        Serial.println("TEST:PROVISION_FAIL reason=key_init");
        while(1) delay(1000);
    }
    
    String deviceAddress = deviceKey->getAddress().c_str();
    Serial.println("TEST:KEY_ADDR=" + deviceAddress);
    
    // Step 1b: Initialize WifiStore AFTER KeyID to expand EEPROM size
    // KeyID called EEPROM.begin(33), now we expand to full size
    Serial.println("Step 1b: Initialize WifiStore EEPROM...");
    if (!wifiStore.begin()) {
        Serial.println("ERROR: Failed to initialize EEPROM for WifiStore");
        Serial.println("TEST:PROVISION_FAIL reason=eeprom_init");
        while(1) delay(1000);
    }
    
    // Step 2: Write test Wi-Fi credentials to WifiStore
    Serial.println("Step 2: Write test Wi-Fi credentials to EEPROM...");
    
    const char* testSSID = TEST_WIFI_SSID;
    const char* testPass = TEST_WIFI_PASS;
    
    // Clear existing networks and write test network
    if (!wifiStore.clearNetworks()) {
        Serial.println("ERROR: Failed to clear existing networks");
        Serial.println("TEST:WIFI_STORED=FAIL reason=clear_failed");
        Serial.println("TEST:PROVISION_FAIL");
        while(1) delay(1000);
    }
    
    if (!wifiStore.appendNetwork(testSSID, testPass)) {
        Serial.println("ERROR: Failed to write test network");
        Serial.println("TEST:WIFI_STORED=FAIL reason=write_failed");
        Serial.println("TEST:PROVISION_FAIL");
        while(1) delay(1000);
    }
    
    // Step 3: Verify by reading back
    Serial.println("Step 3: Verify Wi-Fi credentials...");
    
    if (!wifiStore.isValid()) {
        Serial.println("ERROR: WifiStore validation failed (CRC mismatch)");
        Serial.println("TEST:WIFI_STORED=FAIL reason=crc_fail");
        Serial.println("TEST:PROVISION_FAIL");
        while(1) delay(1000);
    }
    
    WifiCredential readBack[1];
    int count = wifiStore.readNetworks(readBack, 1);
    
    if (count != 1) {
        Serial.println("ERROR: Expected 1 network, got " + String(count));
        Serial.println("TEST:WIFI_STORED=FAIL reason=count_mismatch");
        Serial.println("TEST:PROVISION_FAIL");
        while(1) delay(1000);
    }
    
    if (strcmp(readBack[0].ssid, testSSID) != 0) {
        Serial.println("ERROR: SSID mismatch after readback");
        Serial.println("TEST:WIFI_STORED=FAIL reason=ssid_mismatch");
        Serial.println("TEST:PROVISION_FAIL");
        while(1) delay(1000);
    }
    
    if (strcmp(readBack[0].password, testPass) != 0) {
        Serial.println("ERROR: Password mismatch after readback");
        Serial.println("TEST:WIFI_STORED=FAIL reason=password_mismatch");
        Serial.println("TEST:PROVISION_FAIL");
        while(1) delay(1000);
    }
    
    Serial.println("TEST:WIFI_STORED=OK");
    
    // Step 4: Connect to Wi-Fi to prove credentials work
    Serial.println("Step 4: Connect to test Wi-Fi network...");
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(testSSID, testPass);
    
    unsigned long connectStart = millis();
    unsigned long timeout = 30000; // 30 second timeout for test
    
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - connectStart > timeout) {
            Serial.println("ERROR: Wi-Fi connection timeout");
            Serial.println("TEST:WIFI_CONNECT=FAIL reason=timeout");
            Serial.println("TEST:PROVISION_FAIL");
            while(1) delay(1000);
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    
    unsigned long connectTime = millis() - connectStart;
    String ipAddress = WiFi.localIP().toString();
    
    Serial.println("Connected to Wi-Fi!");
    Serial.println("TEST:WIFI_CONNECT=OK ip=" + ipAddress + " ms=" + String(connectTime));
    
    // Step 5: Report success
    Serial.println("\n=== Test Provisioning Complete ===");
    Serial.println("Device Address: " + deviceAddress);
    Serial.println("Wi-Fi SSID: " + String(testSSID));
    Serial.println("Wi-Fi IP: " + ipAddress);
    Serial.println("Connection Time: " + String(connectTime) + " ms");
    Serial.println("\nTEST:PROVISION_DONE");
    Serial.println("\nDevice is now provisioned and ready for production firmware flash.");
    Serial.println("Idling... (no OTA in test mode)");
    
    // Step 6: Idle forever
    while(1) {
        delay(1000);
    }
}
#endif
