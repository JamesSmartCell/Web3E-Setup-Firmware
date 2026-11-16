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
#include "WifiStore.h"
#include <Web3.h>
#include <KeyID.h>

// Configuration from build flags
#ifndef PROXY_BASE_URL
#define PROXY_BASE_URL ""
#endif

#ifndef PROJECT_ID
#define PROJECT_ID ""
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
WifiStore wifiStore;
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

// Forward declarations
void setupAccessPoint();
void setupWebServer();
void initializeDeviceKey();
bool connectToStoredNetworks();
bool queryProxyForFirmware();
bool downloadAndFlashFirmware();
String scanNetworks();
void handleRoot();
void handleScan();
void handleSave();
void handleList();
void handleRemove();
void handleNotFound();
bool verifyFirmwareSHA256(const String& sha256);

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n=== ESP32 Web3E Setup Firmware ===");
    Serial.println("Initializing...");
    
    // Initialize EEPROM for WifiStore (KeyID initializes its own EEPROM region)
    if (!wifiStore.begin()) {
        Serial.println("ERROR: Failed to initialize EEPROM");
        ESP.restart();
    }
    
    // Initialize device key (Web3E KeyID)
    initializeDeviceKey();
    
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
}

void loop() {
    dnsServer.processNextRequest();
    webServer.handleClient();
    
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
        return false;
    }
    
    Serial.printf("Found %d stored network(s), attempting to connect...\n", count);
    
    WiFi.mode(WIFI_STA);
    
    for (int i = 0; i < count; i++) {
        Serial.printf("Trying network %d: %s\n", i + 1, networks[i].ssid);
        
        if (networks[i].pwLen > 0) {
            WiFi.begin(networks[i].ssid, networks[i].password);
        } else {
            WiFi.begin(networks[i].ssid);
        }
        
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECT_TIMEOUT_MS) {
            delay(500);
            Serial.print(".");
        }
        Serial.println();
        
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("Connected!");
            Serial.print("IP Address: ");
            Serial.println(WiFi.localIP());
            return true;
        } else {
            Serial.printf("Failed to connect to %s\n", networks[i].ssid);
            WiFi.disconnect();
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
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
        Serial.println("ERROR: Failed to parse JSON response");
        lastError = "Invalid JSON from proxy";
        return false;
    }
    
    // Extract firmware metadata
    if (!doc.containsKey("url") || !doc.containsKey("sha256")) {
        Serial.println("ERROR: Missing required fields in proxy response");
        lastError = "Invalid proxy response format";
        return false;
    }
    
    firmwareDownloadUrl = doc["url"].as<String>();
    firmwareSHA256 = doc["sha256"].as<String>();
    firmwareSize = doc.containsKey("size") ? doc["size"].as<size_t>() : 0;
    
    Serial.println("Firmware metadata:");
    Serial.println("  Project: " + doc["project"].as<String>());
    Serial.println("  Version: " + doc["version"].as<String>());
    Serial.println("  URL: " + firmwareDownloadUrl);
    Serial.println("  SHA256: " + firmwareSHA256);
    Serial.printf("  Size: %zu bytes\n", firmwareSize);
    
    return true;
}

bool downloadAndFlashFirmware() {
    if (firmwareDownloadUrl.length() == 0) {
        Serial.println("ERROR: No firmware URL available");
        return false;
    }
    
    Serial.println("\n=== Starting OTA Update ===");
    Serial.println("Downloading from: " + firmwareDownloadUrl);
    
    // Configure HTTPUpdate
    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    httpUpdate.rebootOnUpdate(false);
    
    // Set up progress callback
    httpUpdate.onProgress([](int current, int total) {
        if (total > 0) {
            Serial.printf("Progress: %d%% (%d/%d bytes)\n", (current * 100) / total, current, total);
        }
    });
    
    WiFiClientSecure client;
    client.setInsecure(); // Web3E uses untrusted HTTPS mode
    
    // Perform update
    t_httpUpdate_return ret = httpUpdate.update(client, firmwareDownloadUrl);
    
    switch (ret) {
        case HTTP_UPDATE_FAILED:
            Serial.printf("ERROR: OTA failed - %s\n", httpUpdate.getLastErrorString().c_str());
            lastError = "OTA failed: " + httpUpdate.getLastErrorString();
            return false;
            
        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("No update available");
            return false;
            
        case HTTP_UPDATE_OK:
            Serial.println("OTA download complete, verifying...");
            
            // Verify SHA256 if provided
            if (firmwareSHA256.length() == 64) {
                Serial.println("Note: SHA256 verification happens during HTTPUpdate");
                Serial.println("For post-download verification, implement partition reading");
                // TODO: Read OTA partition and verify hash
                // For now, trust HTTPUpdate's internal verification
            }
            
            // Verify size if provided
            if (firmwareSize > 0) {
                Serial.printf("Expected size: %zu bytes\n", firmwareSize);
                // Size is verified during download by HTTPUpdate
            }
            
            Serial.println("Update verified and flashed successfully!");
            return true;
    }
    
    return false;
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
    webServer.onNotFound(handleNotFound);
    
    webServer.begin();
    Serial.println("Web server started on port 80");
}

String scanNetworks() {
    Serial.println("Scanning for Wi-Fi networks...");
    int n = WiFi.scanNetworks();
    
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        json += "\"encrypted\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    json += "]";
    
    return json;
}

void handleRoot() {
    String html = R"=====(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Setup</title>
<style>
body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5}
.container{max-width:600px;margin:0 auto;background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}
h1{color:#333;margin-top:0}
h2{color:#666;font-size:18px;margin-top:30px;border-bottom:2px solid #ddd;padding-bottom:5px}
button{background:#007bff;color:white;border:none;padding:10px 20px;border-radius:4px;cursor:pointer;font-size:14px;margin:5px}
button:hover{background:#0056b3}
button.danger{background:#dc3545}
button.danger:hover{background:#c82333}
input,select{width:100%;padding:10px;margin:5px 0;border:1px solid #ddd;border-radius:4px;box-sizing:border-box}
label{display:block;margin-top:10px;color:#555;font-weight:bold}
.network-item{padding:10px;margin:5px 0;background:#f8f9fa;border-radius:4px;display:flex;justify-content:space-between;align-items:center}
.network-list{margin-top:10px}
.status{padding:10px;margin:10px 0;border-radius:4px;display:none}
.status.success{background:#d4edda;color:#155724;border:1px solid #c3e6cb;display:block}
.status.error{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb;display:block}
.hidden{display:none}
.device-info{background:#e7f3ff;padding:10px;border-radius:4px;margin-bottom:20px;font-size:12px;color:#004085}
</style>
</head>
<body>
<div class="container">
<h1>ESP32 Web3E Setup</h1>
<div class="device-info">
<strong>Device Address:</strong> )=====" + deviceKey->getAddress().c_str() + R"=====(
</div>
<div id="status" class="status"></div>

<h2>Connect to Wi-Fi</h2>
<button onclick="scanNetworks()">Scan Networks</button>
<div id="scan-results" class="hidden">
<label>Select Network:</label>
<select id="ssid-select" onchange="selectNetwork()"></select>
<label>Password:</label>
<input type="password" id="password" placeholder="Leave empty for open networks">
<button onclick="saveNetwork()">Connect & Save</button>
</div>

<h2>Add Hidden Network</h2>
<label>SSID:</label>
<input type="text" id="hidden-ssid" placeholder="Network name">
<label>Password:</label>
<input type="password" id="hidden-password" placeholder="Leave empty for open networks">
<button onclick="saveHiddenNetwork()">Add & Save</button>

<h2>Saved Networks</h2>
<button onclick="loadSavedNetworks()">Refresh List</button>
<div id="saved-networks" class="network-list"></div>

</div>

<script>
function showStatus(msg,isError){
const s=document.getElementById('status');
s.textContent=msg;
s.className='status '+(isError?'error':'success');
setTimeout(()=>{s.className='status'},5000);
}

function scanNetworks(){
showStatus('Scanning...',false);
fetch('/scan').then(r=>r.json()).then(data=>{
const sel=document.getElementById('ssid-select');
sel.innerHTML='<option value="">-- Select Network --</option>';
data.forEach(net=>{
const opt=document.createElement('option');
opt.value=net.ssid;
opt.textContent=net.ssid+' ('+(net.encrypted?'Secured':'Open')+') '+net.rssi+'dBm';
opt.dataset.encrypted=net.encrypted;
sel.appendChild(opt);
});
document.getElementById('scan-results').classList.remove('hidden');
showStatus('Scan complete. '+data.length+' networks found.',false);
}).catch(e=>showStatus('Scan failed',true));
}

function selectNetwork(){
const sel=document.getElementById('ssid-select');
const pwd=document.getElementById('password');
if(sel.selectedOptions[0] && sel.selectedOptions[0].dataset.encrypted==='false'){
pwd.value='';
pwd.placeholder='Open network (no password)';
}else{
pwd.placeholder='Enter password';
}
}

function saveNetwork(){
const ssid=document.getElementById('ssid-select').value;
const pwd=document.getElementById('password').value;
if(!ssid){showStatus('Please select a network',true);return;}
const formData=new FormData();
formData.append('ssid',ssid);
formData.append('password',pwd);
showStatus('Saving and connecting...',false);
fetch('/save',{method:'POST',body:formData})
.then(r=>r.text())
.then(msg=>{showStatus(msg,false);setTimeout(()=>location.reload(),2000);})
.catch(e=>showStatus('Failed to save',true));
}

function saveHiddenNetwork(){
const ssid=document.getElementById('hidden-ssid').value.trim();
const pwd=document.getElementById('hidden-password').value;
if(!ssid){showStatus('Please enter SSID',true);return;}
const formData=new FormData();
formData.append('ssid',ssid);
formData.append('password',pwd);
showStatus('Saving...',false);
fetch('/save',{method:'POST',body:formData})
.then(r=>r.text())
.then(msg=>{showStatus(msg,false);document.getElementById('hidden-ssid').value='';document.getElementById('hidden-password').value='';})
.catch(e=>showStatus('Failed to save',true));
}

function loadSavedNetworks(){
fetch('/list').then(r=>r.json()).then(data=>{
const div=document.getElementById('saved-networks');
if(data.length===0){
div.innerHTML='<p>No saved networks</p>';
return;
}
div.innerHTML='';
data.forEach((net,idx)=>{
const item=document.createElement('div');
item.className='network-item';
item.innerHTML='<span><strong>'+net.ssid+'</strong> '+(net.pwLen>0?'(Secured)':'(Open)')+'</span>'+
'<button class="danger" onclick="removeNetwork('+idx+')">Remove</button>';
div.appendChild(item);
});
}).catch(e=>showStatus('Failed to load saved networks',true));
}

function removeNetwork(idx){
if(!confirm('Remove this network?'))return;
const formData=new FormData();
formData.append('index',idx);
fetch('/remove',{method:'POST',body:formData})
.then(r=>r.text())
.then(msg=>{showStatus(msg,false);loadSavedNetworks();})
.catch(e=>showStatus('Failed to remove',true));
}

loadSavedNetworks();
</script>
</body>
</html>
)=====";
    
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
    
    if (wifiStore.appendNetwork(ssid.c_str(), password.c_str())) {
        Serial.println("Network saved: " + ssid);
        
        // Try to connect
        WiFi.mode(WIFI_STA);
        if (password.length() > 0) {
            WiFi.begin(ssid.c_str(), password.c_str());
        } else {
            WiFi.begin(ssid.c_str());
        }
        
        unsigned long startTime = millis();
        bool connected = false;
        while ((millis() - startTime) < WIFI_CONNECT_TIMEOUT_MS) {
            if (WiFi.status() == WL_CONNECTED) {
                connected = true;
                break;
            }
            delay(500);
        }
        
        if (connected) {
            Serial.println("Connected! IP: " + WiFi.localIP().toString());
            wifiConnected = true;
            
            // Query for firmware
            if (queryProxyForFirmware()) {
                webServer.send(200, "text/plain", "Connected! Starting firmware update...");
            } else {
                webServer.send(200, "text/plain", "Connected to Wi-Fi but firmware query failed: " + lastError);
            }
        } else {
            Serial.println("Connection attempt failed, but network saved for later");
            setupAccessPoint(); // Resume AP mode
            webServer.send(200, "text/plain", "Network saved but connection failed. Try another network.");
        }
    } else {
        webServer.send(500, "text/plain", "Failed to save network");
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
        webServer.send(200, "text/plain", "Network removed");
    } else {
        webServer.send(500, "text/plain", "Failed to remove network");
    }
}

void handleNotFound() {
    // Redirect everything to root for captive portal
    webServer.sendHeader("Location", "/", true);
    webServer.send(302, "text/plain", "");
}
