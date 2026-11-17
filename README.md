# ESP32 Web3E Setup Firmware

A PlatformIO-based provisioning firmware for ESP32 Web3E projects. This firmware handles Wi-Fi configuration and OTA installation of project-specific firmware from Cloudflare R2, coordinated through SmartLayerProxy.

**Web3E Library:** https://github.com/AlphaWallet/Web3E

## Purpose

Project designers build **one instance of this setup firmware per project**. When flashed to a device, it:

1. Generates or recovers a unique device private key (Web3E KeyID) stored in EEPROM
2. Provides a captive portal for Wi-Fi provisioning
3. Stores Wi-Fi credentials securely in EEPROM (never in firmware or repo)
4. Queries SmartLayerProxy for the project's latest firmware
5. Downloads and OTA-flashes the production firmware from Cloudflare R2
6. Reboots into the production firmware

## Architecture

### SmartLayerProxy Coordinator (Primary Method)

The setup firmware queries a central proxy for firmware metadata:

```
GET <PROXY_BASE_URL>/api/firmware/<PROJECT_ID>/latest
```

**Response JSON:**
```json
{
  "project": "my-project",
  "version": "1.0.0",
  "url": "https://example.r2.dev/my-project/firmware.bin",
  "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
  "size": 1234567
}
```

The firmware downloads from `url`, verifies `sha256` and `size`, then flashes the OTA partition and reboots.

### Direct URL Override (Testing)

For testing without the proxy, configure `FIRMWARE_URL` and optionally `FIRMWARE_SHA256` in `platformio.ini`. When `FIRMWARE_URL` is set, the proxy is skipped.

## Per-Project Workflow

1. **Configure the build:**
   - Edit `platformio.ini` → `build_flags`
   - Set `PROXY_BASE_URL` (e.g., `https://proxy.example.com`)
   - Set `PROJECT_ID` (unique identifier for this project)
   - Optionally customize `AP_SSID_PREFIX`, `AP_PASSWORD`, `MAX_WIFI_NETWORKS`

2. **Build the setup firmware:**
   ```bash
   pio run -e esp32dev
   ```

3. **Upload project firmware to Cloudflare R2:**
   - Build your production firmware (e.g., https://github.com/JamesSmartCell/NFT-Gated-ESP-Control)
   - Generate SHA-256 hash: `sha256sum firmware.bin`
   - Upload `firmware.bin` to R2 bucket
   - Configure SmartLayerProxy to serve this firmware for `PROJECT_ID`

4. **Flash setup firmware to device:**
   ```bash
   pio run -e esp32dev -t upload
   ```

5. **Provision Wi-Fi:**
   - Device starts access point: `ESP32-Setup-XXXX` (where XXXX = last 4 hex digits of MAC)
   - Connect phone/laptop to AP
   - Captive portal auto-opens (or navigate to `192.168.4.1`)
   - Scan networks, select one, enter password, click "Connect & Save"

6. **Automatic OTA:**
   - Device connects to Wi-Fi
   - Queries SmartLayerProxy → receives firmware metadata
   - Downloads firmware from R2 URL
   - Verifies SHA-256 hash and size
   - Flashes OTA partition
   - Reboots into production firmware

## EEPROM Layout

The ESP32 has 512 bytes of emulated EEPROM in flash. This firmware shares EEPROM with Web3E KeyID and production firmwares.

```
┌─────────────────────────────────────────────────────────────┐
│ Offset │ Size  │ Description                                │
├─────────────────────────────────────────────────────────────┤
│ 0      │ 1     │ KeyID Marker (0x40 = 64 when key present) │
│ 1-32   │ 32    │ Device Private Key (ECDSA secp256k1)      │
├─────────────────────────────────────────────────────────────┤
│ 33     │ 4     │ Wi-Fi Magic: 'W' 'I' 'F' 'I' (0x57...)   │
│ 37     │ 1     │ Wi-Fi Version (currently 1)               │
│ 38+    │ var   │ Network Records (see below)               │
│ ...    │ 2     │ Terminator: 0x00 0x00                     │
│ end-4  │ 4     │ CRC32 (little-endian, covers magic→term)  │
└─────────────────────────────────────────────────────────────┘
```

### KeyID Region (Bytes 0-32)

- **Managed by Web3E's `KeyID` class** (see https://github.com/AlphaWallet/Web3E/blob/master/src/KeyID.cpp)
- Byte 0: Marker value `64` (0x40) indicates a key is stored
- Bytes 1-32: 32-byte ECDSA private key (secp256k1 curve)
- **This setup firmware MUST NEVER overwrite an existing key**
- KeyID automatically generates a new key if marker ≠ 64

### Wi-Fi Region (Bytes 33+)

- **Offset:** `33` (immediately after KeyID region)
- **Format:** Custom binary structure with CRC32 integrity check

**Network Record Format:**
```
[ssidLen:1 byte][ssid: ssidLen bytes][pwLen:1 byte][password: pwLen bytes]
```

- **SSID length:** 1-32 bytes (0 is invalid, used for terminator)
- **Password length:** 0-64 bytes (0 = open network)
- **Terminator:** Two consecutive `0x00` bytes (empty SSID + empty password)
- **CRC32:** Last 4 bytes, computed over `magic || version || records || terminator`

**Constraints:**
- Maximum networks: `MAX_WIFI_NETWORKS` (default 8, configurable in `platformio.ini`)
- Invalid magic or CRC failure → no networks are read
- Records are read/written atomically (all networks updated together)

### Total EEPROM Usage

- KeyID region: 33 bytes
- Wi-Fi header: 5 bytes (magic + version)
- Max per network: 1 + 32 + 1 + 64 = 98 bytes
- Max 8 networks: 8 × 98 = 784 bytes
- Terminator: 2 bytes
- CRC: 4 bytes
- **Total: ~828 bytes** (well under 512-byte EEPROM limit with default config)

## Using WifiStore in Production Firmware

Production firmwares can read Wi-Fi credentials from EEPROM using the `WifiStore` library:

```cpp
#include <WifiStore.h>

WifiStore wifiStore;
wifiStore.begin();

WifiCredential networks[MAX_WIFI_NETWORKS];
int count = wifiStore.readNetworks(networks, MAX_WIFI_NETWORKS);

for (int i = 0; i < count; i++) {
    WiFi.begin(networks[i].ssid, networks[i].password);
    // ... attempt connection
}
```

Copy `lib/WifiStore/` into your project's `lib/` directory or install as a PlatformIO library.

**API Reference:**

- `bool begin()` — Initialize EEPROM
- `bool isValid()` — Check if stored data has valid magic/CRC
- `int readNetworks(WifiCredential* creds, int maxCount)` — Read all networks
- `bool writeNetworks(const WifiCredential* creds, int count)` — Overwrite all networks
- `bool appendNetwork(const char* ssid, const char* password)` — Add one network
- `bool removeNetwork(int index)` — Remove by index
- `bool clearNetworks()` — Erase all networks (keeps valid structure)
- `int getNetworkCount()` — Count stored networks

## Build & Flash Commands

### Build for ESP32
```bash
pio run -e esp32dev
```

### Upload via USB
```bash
pio run -e esp32dev -t upload
```

### Monitor serial output
```bash
pio device monitor -b 115200
```

### Run native unit tests (WifiStore library)
```bash
pio test -e native
```

### Clean build
```bash
pio run -t clean
```

## Configuration Reference

Edit `platformio.ini` under `[env:esp32dev]` → `build_flags`:

| Flag | Required | Default | Description |
|------|----------|---------|-------------|
| `PROXY_BASE_URL` | Yes* | - | SmartLayerProxy base URL (e.g., `https://proxy.example.com`) |
| `PROJECT_ID` | Yes* | - | Unique project identifier for proxy API |
| `FIRMWARE_URL` | No | - | Direct firmware URL (overrides proxy, for testing) |
| `FIRMWARE_SHA256` | No | - | Direct firmware SHA-256 (64 hex chars, only with `FIRMWARE_URL`) |
| `AP_SSID_PREFIX` | No | `ESP32-Setup` | Access point SSID prefix (suffixed with `-XXXX` from MAC) |
| `AP_PASSWORD` | No | (empty) | Access point password (empty = open network) |
| `MAX_WIFI_NETWORKS` | No | 8 | Maximum networks to store in EEPROM |

*Either `PROXY_BASE_URL` + `PROJECT_ID` OR `FIRMWARE_URL` must be configured.

## OTA Partition Scheme

This firmware uses `default_ota.csv` (built into ESP-IDF):

```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x5000
otadata,  data, ota,     0xe000,  0x2000
app0,     app,  ota_0,   0x10000, 0x140000
app1,     app,  ota_1,   0x150000,0x140000
spiffs,   data, spiffs,  0x290000,0x170000
```

- **Two OTA slots:** `app0` and `app1` (1.25 MB each)
- **No factory partition:** Recovery requires re-flashing setup firmware via USB
- **NVS:** Stores Wi-Fi credentials (note: this firmware uses EEPROM abstraction, not NVS directly)
- **SPIFFS:** Available for production firmware use (1.4 MB)

When OTA updates are applied, the ESP32 bootloader alternates between `app0` and `app1`. The inactive slot is overwritten with the new firmware.

## Security Notes

### Wi-Fi Credentials
- **Stored only in EEPROM** (emulated in flash at a fixed address)
- **Never compiled into firmware images or committed to git**
- Survives firmware updates (OTA writes to different partition)
- Persists across setup firmware re-flash (unless chip is fully erased)

### Device Private Key
- **Generated once per device** by Web3E KeyID on first boot
- **Stored in EEPROM bytes 0-32** (managed by KeyID.cpp)
- **Never exposed over network or serial** (except device address, which is public)
- Survives all firmware updates and EEPROM writes to Wi-Fi region
- **Recovery:** If EEPROM is erased, a new key is generated (device gets new address)

### HTTPS & Certificate Validation
- **Web3E uses `client.setInsecure()`** (TLS without certificate validation)
- **Trade-off:** Simpler deployment, but vulnerable to MITM attacks
- **Recommendation:** Use Cloudflare R2 URLs (TLS with opportunistic security)
- **SHA-256 verification:** Firmware integrity is verified post-download

### Captive Portal
- **Open or WPA2-protected AP** (configure `AP_PASSWORD`)
- **No authentication on web interface** (physical proximity assumed)
- **Recommendation:** Set `AP_PASSWORD` for sensitive deployments

### Firmware Verification
- **SHA-256 hash:** Provided by SmartLayerProxy, verified before flashing
- **Size check:** Expected size validated during download
- **HTTPUpdate library:** Performs integrity checks during OTA process

## Recovery Procedure

If Wi-Fi credentials are lost or the device cannot connect:

1. **Re-flash setup firmware via USB:**
   ```bash
   pio run -e esp32dev -t upload
   ```

2. **Device key and saved networks survive** (stored in EEPROM, not firmware partition)

3. **Join captive portal** and reconfigure Wi-Fi

If the device key must be reset (generates new address):

1. **Fully erase flash:**
   ```bash
   esptool.py --port /dev/ttyUSB0 erase_flash
   ```

2. **Re-flash setup firmware** (device generates new key on next boot)

## Testing

### Native Unit Tests (WifiStore)
```bash
pio test -e native -v
```

Validates:
- Encode/decode of network records
- CRC32 integrity checks
- Terminator detection
- Bounds checking (SSID/password length limits)
- Max networks limit
- Append/remove/clear operations

### Manual Integration Test
1. Flash firmware to ESP32
2. Verify device key is generated (check Serial output for address)
3. Connect to `ESP32-Setup-XXXX` AP
4. Access captive portal (should auto-open)
5. Add a Wi-Fi network
6. Verify OTA download starts (monitor Serial)
7. Check firmware hash verification
8. Confirm reboot into production firmware

## Troubleshooting

### "Failed to initialize EEPROM"
- **Cause:** EEPROM initialization failed (rare)
- **Fix:** Check flash integrity, try full erase + re-flash

### "No stored Wi-Fi networks found"
- **Cause:** First boot or EEPROM was erased
- **Expected:** Device starts captive portal automatically

### "Failed to get firmware metadata"
- **Cause:** Proxy API error or network issue
- **Check:**
  - `PROXY_BASE_URL` and `PROJECT_ID` are correct
  - Device has internet access
  - Proxy API is reachable
  - Serial output shows HTTP error code

### "OTA failed: HTTP error"
- **Cause:** Firmware download failed
- **Check:**
  - `url` from proxy response is accessible
  - Firmware file exists on R2
  - Network connection is stable

### "OTA failed: Not enough space"
- **Cause:** Firmware binary exceeds OTA partition size (1.25 MB)
- **Fix:** Reduce production firmware size or customize partition table

### Captive portal doesn't open automatically
- **On iOS:** Should auto-open when joining AP
- **On Android:** Tap "Sign in to network" notification
- **Manual:** Navigate to `http://192.168.4.1`

### Wi-Fi connects but firmware doesn't update
- **Check Serial output** for proxy query results
- **Verify** SmartLayerProxy returns valid JSON
- **Test** direct URL with `FIRMWARE_URL` override

## Project Structure

```
.
├── platformio.ini          # PlatformIO configuration (edit build_flags here)
├── src/
│   └── main.cpp            # Setup firmware logic
├── lib/
│   └── WifiStore/          # Reusable Wi-Fi credential storage library
│       ├── WifiStore.h
│       └── WifiStore.cpp
├── test/
│   └── test_wifistore.cpp  # Native unit tests for WifiStore
├── .gitignore              # Excludes .pio/, .vscode/, etc.
├── LICENSE                 # Project license
└── README.md               # This file
```

## Dependencies

- **PlatformIO:** ESP32 framework (Arduino)
- **Web3E:** https://github.com/AlphaWallet/Web3E (device key management)
- **ArduinoJson:** JSON parsing for proxy responses
- **ESP32 Arduino Core:** WiFi, HTTPClient, HTTPUpdate, WebServer, DNSServer

Install via `pio lib install` or auto-resolved from `lib_deps` in `platformio.ini`.

## License

See `LICENSE` file.

## Author & Support

**Owner:** Rob (JamesSmartCell)  
**GitHub:** https://github.com/JamesSmartCell  
**Web3E Project:** https://github.com/AlphaWallet/Web3E  
**Example Production Firmware:** https://github.com/JamesSmartCell/NFT-Gated-ESP-Control

For issues or questions, open a GitHub issue or contact the project maintainer.

---

**Version:** 1.0  
**Last Updated:** November 2026
