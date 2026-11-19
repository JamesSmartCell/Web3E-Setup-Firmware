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
   - Edit `platformio.ini` → `[env:esp32dev]`
   - **CRITICAL: Set `board_build.partitions`** to match your target project firmware
     - Default: `min_spiffs.csv` (1.9 MB OTA slots, recommended)
     - If target firmware uses a different partition table, **you must use the same one here**
     - OTA cannot change partition tables - mismatch = OTA failure
   - Set `PROXY_BASE_URL` (e.g., `https://scriptproxy.percolate.one`)
   - Set `PROJECT_ID` (unique identifier for this project, e.g., `garagedoor`)
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
   - Networks are scanned once at boot; tap **Refresh Networks** to rescan (no periodic rescan)
   - Tap a network to add it (enter password when prompted)
   - Or add a hidden network manually
   - Review your "Saved Networks" list
   - Click "Done - Connect & Update Firmware" to proceed

6. **Automatic OTA:**
   - Device connects to Wi-Fi
   - Queries SmartLayerProxy → receives firmware metadata
   - Downloads firmware from R2 URL
   - Verifies SHA-256 hash and size
   - Flashes OTA partition
   - Reboots into production firmware

## Stay-in-Portal / Revoke Flag Contract (NVS)

**Storage:** ESP32 NVS via Arduino `Preferences` — **not** EEPROM. KeyID (EEPROM 0–32) and WifiStore (EEPROM 33+) are never modified by this path.

| Item | Value |
|------|--------|
| NVS namespace | `web3e_boot` |
| Key `stay_portal` | `uint8_t`: `1` = stay in setup portal; `0` = normal |
| Key `boot_count` | `uint8_t`: consecutive quick-reboot counter |

### Revocation flow (app firmware must match)

1. Proxy sends admin-signed revoke over the device TCP channel.
2. **App firmware** sets `stay_portal = 1` in NVS namespace `web3e_boot`, then OTA-installs **this** setup firmware (from the same project R2 store, sha256-verified) and reboots.
3. Setup boots, reads `stay_portal == 1`, **skips** auto Wi-Fi connect and app OTA, opens the captive portal.
4. **Saved Wi-Fi credentials are preserved** and shown in the portal so the user can confirm or change them.
5. When the user taps **Done**, setup clears `stay_portal` to `0`, then connects and downloads the (new) app firmware.

### Offline reset: triple power-cycle (app + setup must match)

| Constant | Value | Meaning |
|----------|-------|---------|
| `PORTAL_BOOT_TRIPLE_THRESHOLD` | `3` | Quick reboots required to force portal |
| `PORTAL_BOOT_STABLE_MS` | `10000` (10 s) | Uptime after which `boot_count` is cleared |

**Behaviour:**

1. Every boot increments `boot_count` (persisted in NVS).
2. If `boot_count >= 3` before the stable window, the device sets `stay_portal = 1`, resets `boot_count` to `0`, and opens the portal (same path as revoke — no auto app download).
3. After ~10 s continuous uptime, `boot_count` is cleared to `0`.
4. **A router outage / Wi-Fi down alone must never open the AP.** Only `stay_portal` or this triple-cycle path forces the portal when credentials already exist.

App firmware should use the same namespace/keys and timings so a triple power-cycle while running the app can OTA back to setup (or otherwise enter setup) consistently. The reusable implementation lives in `lib/PortalBoot/` (candidate for Web3E later).

### What must match on other repos

| Side | Requirement |
|------|-------------|
| **App (NFT-Gated-ESP-Control)** | On revoke: `Preferences("web3e_boot").putUChar("stay_portal", 1)` before OTA to setup. Same triple-cycle counter/timings. Never erase KeyID. |
| **Proxy (SmartLayerProxy)** | Serve setup firmware from project R2 like app firmware; push revoke notice on existing device channel. |

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

**Important:** This setup firmware uses `SetupWifiStore` (extended version with write operations). Production firmwares should use Web3E's built-in read-only `WifiStore` class, which is byte-compatible:

```cpp
#include <Web3.h>
#include <WifiStore.h>  // From Web3E library

WifiStore wifiStore;
wifiStore.begin();

WifiCredential networks[MAX_WIFI_NETWORKS];
int count = wifiStore.readNetworks(networks, MAX_WIFI_NETWORKS);

for (int i = 0; i < count; i++) {
    WiFi.begin(networks[i].ssid, networks[i].password);
    // ... attempt connection
}
```

Web3E's `WifiStore` (v1.45.0+, commit 59e427f or later) provides read-only access:
- `bool begin()` — Initialize EEPROM
- `bool isValid()` — Check if stored data has valid magic/CRC
- `int readNetworks(WifiCredential* creds, int maxCount)` — Read all networks
- `int getNetworkCount()` — Count stored networks

The EEPROM layout is identical between `SetupWifiStore` (this firmware) and Web3E's `WifiStore`, so production firmware can read credentials written by the setup firmware.

**For custom setup firmware** that needs write operations, copy `lib/WifiStore/` (renamed to `SetupWifiStore`) into your project. It provides additional write methods:
- `bool writeNetworks(const WifiCredential* creds, int count)` — Overwrite all networks
- `bool appendNetwork(const char* ssid, const char* password)` — Add one network
- `bool removeNetwork(int index)` — Remove by index
- `bool clearNetworks()` — Erase all networks (keeps valid structure)

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
| `board_build.partitions` | **CRITICAL** | `min_spiffs.csv` | **Partition table - MUST match target firmware!** |
| `APP_NAME` | No | `PROJECT_ID` | Application name shown in portal title (e.g., `GarageDoor`) |
| `PROXY_BASE_URL` | Yes* | - | SmartLayerProxy base URL (e.g., `https://scriptproxy.percolate.one`) |
| `PROJECT_ID` | Yes* | - | Unique project identifier for proxy API (e.g., `garagedoor`) |
| `FIRMWARE_URL` | No | - | Direct firmware URL (overrides proxy, for testing) |
| `FIRMWARE_SHA256` | No | - | Direct firmware SHA-256 (64 hex chars, only with `FIRMWARE_URL`) |
| `AP_SSID_PREFIX` | No | `ESP32-Setup` | Access point SSID prefix (suffixed with `-XXXX` from MAC) |
| `AP_PASSWORD` | No | (empty) | Access point password (empty = open network) |
| `MAX_WIFI_NETWORKS` | No | 8 | Maximum networks to store in EEPROM |

*Either `PROXY_BASE_URL` + `PROJECT_ID` OR `FIRMWARE_URL` must be configured.

**⚠️ CRITICAL:** The `board_build.partitions` setting is not a build flag - it goes directly under `[env:esp32dev]` in `platformio.ini`, not inside `build_flags`. This setting determines the flash partition layout and **must match** your target project firmware's partition table.

## Captive Portal Features

The setup firmware provides a mobile-friendly captive portal with automatic Wi-Fi provisioning:

- **Scan at boot + Refresh only:** One scan when the portal starts; **Refresh Networks** triggers another. No periodic ~12 s rescans (those used `WIFI_AP_STA` continuously and made classic ESP32 softAP unreliable — phone joins AP but `192.168.4.1` does not respond).
- **No scan while AP clients connected (auto path):** Boot/auto scans are skipped if a station is already on the AP; after a scan finishes, mode returns to `WIFI_AP`.
- **Deduplicated list:** SSIDs are deduplicated, sorted by signal strength (strongest first)
- **Signal indicators:** Visual signal strength bars (▂▄▆█) and lock icons (🔒) for encrypted networks
- **Tap to add:** Tap any network in the list to add it (password prompt appears for secured networks)
- **Hidden networks:** Manual entry field for SSIDs not broadcast
- **Saved Networks list:** Shows all networks queued for connection (SSID only, passwords never displayed)
- **Password visibility toggle:** 👁 button to show/hide passwords during entry
- **Done button:** Clears `stay_portal`, keeps Wi-Fi EEPROM, attempts connection, and starts firmware update
- **Mobile compatibility:** Tested on iOS and Android captive portal browsers (no external dependencies)

## OTA Partition Scheme

### CRITICAL: Partition Table Must Match Target Firmware

**The setup firmware's partition table MUST match the target project firmware's partition table.**

When you flash the setup firmware via USB, it **writes the partition table to the ESP32's flash**. OTA updates **cannot change** the partition table. If the setup firmware uses a smaller partition table than the target firmware needs, OTA will fail with "Not enough space" errors.

### Default Configuration

This firmware uses **`min_spiffs.csv`** by default (configured in `platformio.ini` → `board_build.partitions`):

```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x5000
otadata,  data, ota,     0xe000,  0x2000
app0,     app,  ota_0,   0x10000, 0x1E0000
app1,     app,  ota_1,   0x1F0000,0x1E0000
spiffs,   data, spiffs,  0x3D0000,0x30000
```

- **Two OTA slots:** `app0` and `app1` (1.9 MB / ~1,966,080 bytes each)
- **Minimal SPIFFS:** 192 KB (adjust if target firmware needs more storage)
- **Supports large firmwares:** Up to ~1.9 MB (suitable for Web3/crypto projects)

### Changing the Partition Table

If your target project firmware uses a **different partition table**, you **must** update the setup firmware:

1. Edit `platformio.ini` → `[env:esp32dev]` → `board_build.partitions`
2. Set it to the **same partition CSV** as your target firmware
3. Rebuild: `pio run -e esp32dev`
4. **Re-flash via USB** to write the new partition table

**Example:** If target firmware uses `default.csv` (1.25 MB slots):
```ini
board_build.partitions = default.csv
```

### Verifying Partition Size

The setup firmware **checks partition size before OTA** and prints a clear error if there's a mismatch:

```
=== OTA PARTITION SIZE MISMATCH ===
ERROR: Firmware too large for OTA partition!
  Target firmware size: 1426768 bytes (1.36 MB)
  Available OTA slot:   1310720 bytes (1.25 MB)

CAUSE: Setup firmware partition table does not match target project firmware.
FIX: Edit platformio.ini -> board_build.partitions, rebuild, and
     re-flash setup firmware via USB.
```

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
- **SHA-256 hash:** Provided by SmartLayerProxy, **cryptographically verified** using mbedtls during download
- **Streaming verification:** Hash is computed as the firmware is downloaded (not post-download)
- **Abort on mismatch:** Update is aborted immediately if SHA-256 does not match expected value
- **Size check:** Expected size validated during download
- **Applies to both paths:** Direct URL (`FIRMWARE_URL`) and proxy-based downloads

## Recovery Procedure

### Triple power-cycle (no USB)

Power the device off and on **3 times**, each time within ~10 seconds of boot (before the stable window clears the counter). The device sets `stay_portal` and opens the setup portal. KeyID and saved Wi-Fi are preserved.

### USB re-flash

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

**Do not** full-erase flash for revoke or normal recovery — that destroys the KeyID private key.

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

## Hardware Test Provisioning

The `test_provision` environment builds a special firmware variant for automated hardware test harnesses. This firmware auto-provisions devices with test credentials without user interaction.

### Purpose

Production manufacturing and testing environments can flash this firmware to:
1. Generate or verify the device's Web3E private key
2. Pre-load test Wi-Fi credentials into EEPROM
3. Verify the device can connect to the test network
4. Output machine-parseable test results over serial

After provisioning completes, the production firmware is flashed over USB (no OTA in test mode).

### Configuration

1. **Create test secrets file:**
   ```bash
   cp test/test_secrets.ini.example test/test_secrets.ini
   # Edit test/test_secrets.ini with your test network credentials
   ```

2. **Format of `test/test_secrets.ini`:**
   ```ini
   ; Test secrets for hardware provisioning
   WIFI_SSID = YourTestNetwork
   WIFI_PASS = YourTestPassword123
   ```

   - Simple `KEY = value` format (no `[section]` headers)
   - Lines starting with `;` are comments
   - This file is **gitignored** and must never be committed

3. **Build test provisioning firmware:**
   ```bash
   pio run -e test_provision
   ```

   The pre-build script (`scripts/inject_test_secrets.py`) reads `test_secrets.ini` and injects the credentials as compile-time defines.

4. **Flash to device:**
   ```bash
   pio run -e test_provision -t upload
   ```

### Serial Output (115200 baud)

The firmware outputs machine-parseable lines for test harness automation:

```
=== ESP32 Web3E Setup Firmware ===
Mode: TEST_PROVISION (Hardware Test Harness)
Initializing...

=== Hardware Test Provisioning ===
Step 1: Initialize device key...
TEST:KEY_ADDR=0x1234567890abcdef1234567890abcdef12345678
Step 2: Write test Wi-Fi credentials to EEPROM...
Step 3: Verify Wi-Fi credentials...
TEST:WIFI_STORED=OK
Step 4: Connect to test Wi-Fi network...
...........
Connected to Wi-Fi!
TEST:WIFI_CONNECT=OK ip=192.168.1.100 ms=5234

=== Test Provisioning Complete ===
Device Address: 0x1234567890abcdef1234567890abcdef12345678
Wi-Fi SSID: YourTestNetwork
Wi-Fi IP: 192.168.1.100
Connection Time: 5234 ms

TEST:PROVISION_DONE

Device is now provisioned and ready for production firmware flash.
Idling... (no OTA in test mode)
```

### Parseable Test Lines

| Line | Format | Meaning |
|------|--------|---------|
| `TEST:KEY_ADDR=0x...` | Device Ethereum address (40 hex chars) | Device key initialized |
| `TEST:WIFI_STORED=OK` | Success | Credentials written and verified |
| `TEST:WIFI_STORED=FAIL reason=...` | Failure | EEPROM write/verify failed |
| `TEST:WIFI_CONNECT=OK ip=... ms=...` | Success with IP and connection time | Wi-Fi connected |
| `TEST:WIFI_CONNECT=FAIL reason=...` | Failure | Wi-Fi connection failed |
| `TEST:PROVISION_DONE` | Final marker | Provisioning complete, device ready |
| `TEST:PROVISION_FAIL` | Fatal error | Provisioning failed, device unusable |

**Note:** The test firmware **never prints** the Wi-Fi password or device private key to serial.

### Failure Modes

If provisioning fails, the device halts and prints an error code:

- `reason=key_init` — Device key initialization failed
- `reason=clear_failed` — Could not clear existing Wi-Fi data
- `reason=write_failed` — Could not write test credentials
- `reason=crc_fail` — CRC validation failed after write
- `reason=count_mismatch` — Wrong number of networks stored
- `reason=ssid_mismatch` — SSID readback verification failed
- `reason=password_mismatch` — Password readback verification failed
- `reason=timeout` — Wi-Fi connection timeout (30 seconds)

### Test Harness Integration

Example Python test harness:

```python
import serial
import time

def provision_device(port='/dev/ttyUSB0'):
    ser = serial.Serial(port, 115200, timeout=60)
    
    results = {
        'key_addr': None,
        'wifi_stored': False,
        'wifi_connected': False,
        'ip_address': None,
        'connect_time_ms': None,
        'done': False
    }
    
    start_time = time.time()
    
    while time.time() - start_time < 60:  # 60 second timeout
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        
        if line.startswith('TEST:KEY_ADDR='):
            results['key_addr'] = line.split('=')[1]
        elif line.startswith('TEST:WIFI_STORED='):
            results['wifi_stored'] = 'OK' in line
        elif line.startswith('TEST:WIFI_CONNECT='):
            if 'OK' in line:
                results['wifi_connected'] = True
                # Parse ip=... and ms=...
                parts = line.split()
                for part in parts:
                    if part.startswith('ip='):
                        results['ip_address'] = part.split('=')[1]
                    elif part.startswith('ms='):
                        results['connect_time_ms'] = int(part.split('=')[1])
        elif line.startswith('TEST:PROVISION_DONE'):
            results['done'] = True
            break
        elif line.startswith('TEST:PROVISION_FAIL'):
            raise Exception(f"Provisioning failed: {line}")
    
    ser.close()
    
    if not results['done']:
        raise Exception("Provisioning timeout")
    
    return results
```

### EEPROM Layout After Provisioning

The test firmware writes the exact same EEPROM layout as the production setup firmware:

```
[0-32]   Device private key (Web3E KeyID format)
[33+]    Test Wi-Fi credentials (WifiStore format)
```

Production firmware flashed afterward can read these credentials using the WifiStore library.

### Security Note

**Do not use test credentials in production devices.** The test network credentials are only for factory testing. Production devices should use the regular setup firmware's captive portal to provision customer Wi-Fi networks.

## Project Structure

```
.
├── platformio.ini                 # PlatformIO configuration (3 environments)
├── src/
│   └── main.cpp                   # Setup firmware logic (production + test modes)
├── lib/
│   ├── WifiStore/                 # Reusable Wi-Fi credential storage library
│   │   ├── SetupWifiStore.h
│   │   └── SetupWifiStore.cpp
│   └── PortalBoot/                # NVS stay_portal + triple power-cycle (shared contract)
│       ├── PortalBoot.h
│       └── PortalBoot.cpp
├── scripts/
│   └── inject_test_secrets.py     # Pre-build script for test_provision env
├── test/
│   ├── test_wifistore.cpp         # Native unit tests for WifiStore
│   ├── test_secrets.ini.example   # Template for test credentials
│   └── test_secrets.ini           # (gitignored) Actual test credentials
├── .gitignore                     # Excludes .pio/, test_secrets.ini, etc.
├── LICENSE                        # Project license
└── README.md                      # This file
```

### PlatformIO Environments

- **`esp32dev`** (default): Production setup firmware with captive portal and OTA
- **`test_provision`**: Hardware test variant for automated provisioning
- **`native`**: Native unit tests for WifiStore library

## Dependencies

- **PlatformIO:** ESP32 framework (Arduino)
- **Web3E:** https://github.com/AlphaWallet/Web3E#59e427f (device key management, pinned to commit 59e427f)
- **ArduinoJson:** JSON parsing for proxy responses (v7.0.0+)
- **ESP32 Arduino Core:** WiFi, HTTPClient, HTTPUpdate, WebServer, DNSServer
- **mbedtls:** SHA-256 verification (included with ESP32 Arduino Core)

Install via `pio lib install` or auto-resolved from `lib_deps` in `platformio.ini`.

**Note:** Web3E is pinned to commit `59e427f` (or later) which includes `WEB3E_EEPROM_SIZE` definition and the read-only `WifiStore` class. This ensures compatibility between setup firmware and production firmware.

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
