#ifndef SETUPWIFISTORE_H
#define SETUPWIFISTORE_H

#include <stdint.h>
#include <string.h>

// EEPROM Layout Constants
// KeyID (Web3E) uses bytes 0-32: [0]=marker(64), [1-32]=32-byte private key
#define KEYID_EEPROM_SIZE 33
#define WIFI_EEPROM_OFFSET 33

// Wi-Fi EEPROM structure (starts at offset 33):
// [33-36]: Magic "WIFI" (4 bytes)
// [37]:    Version byte (currently 1)
// [38+]:   Records: [ssidLen:1][ssid bytes][pwLen:1][pw bytes]
// Terminator: 0x00 0x00 (empty SSID + empty password)
// Last 4 bytes: CRC32 over magic..terminator

#define WIFI_MAGIC_SIZE 4
#define WIFI_VERSION_SIZE 1
#define WIFI_HEADER_SIZE (WIFI_MAGIC_SIZE + WIFI_VERSION_SIZE)
#define WIFI_CRC_SIZE 4
#define WIFI_TERMINATOR_SIZE 2

#ifndef MAX_WIFI_NETWORKS
#define MAX_WIFI_NETWORKS 8
#endif

#define MAX_SSID_LENGTH 32
#define MAX_PASSWORD_LENGTH 64

// Calculate required EEPROM size
// Header + max networks * (1+32+1+64) + terminator + CRC
#define WIFI_MAX_STORAGE_SIZE (WIFI_HEADER_SIZE + (MAX_WIFI_NETWORKS * (1 + MAX_SSID_LENGTH + 1 + MAX_PASSWORD_LENGTH)) + WIFI_TERMINATOR_SIZE + WIFI_CRC_SIZE)

// Use WEB3E_EEPROM_SIZE if available (from Web3E master), otherwise calculate
#ifdef WEB3E_EEPROM_SIZE
#define TOTAL_EEPROM_SIZE WEB3E_EEPROM_SIZE
#else
#define TOTAL_EEPROM_SIZE (WIFI_EEPROM_OFFSET + WIFI_MAX_STORAGE_SIZE)
#endif

// Forward declare WifiCredential if not already defined by Web3E
#ifndef WIFICREDENTIAL_DEFINED
#define WIFICREDENTIAL_DEFINED
struct WifiCredential {
    char ssid[MAX_SSID_LENGTH + 1];
    char password[MAX_PASSWORD_LENGTH + 1];
    uint8_t ssidLen;
    uint8_t pwLen;
};
#endif

// SetupWifiStore: Extended WifiStore with write operations for provisioning
// Uses the same EEPROM layout as Web3E's WifiStore (offset 33+)
// Byte-compatible with Web3E's read-only WifiStore
class SetupWifiStore {
public:
    SetupWifiStore();
    
    // Initialize and read stored networks
    bool begin();
    
    // Read all stored networks
    int readNetworks(WifiCredential* credentials, int maxCount);
    
    // Write networks (replaces all)
    bool writeNetworks(const WifiCredential* credentials, int count);
    
    // Append a new network
    bool appendNetwork(const char* ssid, const char* password);
    
    // Remove a network by index
    bool removeNetwork(int index);
    
    // Clear all networks
    bool clearNetworks();
    
    // Get count of stored networks
    int getNetworkCount();
    
    // Validate stored data
    bool isValid();

private:
    bool isInitialized;
    
    // CRC32 calculation
    uint32_t calculateCRC32(const uint8_t* data, size_t length);
    
    // EEPROM abstraction (overridden in tests)
    virtual uint8_t eepromRead(int address);
    virtual void eepromWrite(int address, uint8_t value);
    virtual bool eepromCommit();
    
    // Internal helpers
    bool writeHeader();
    bool writeTerminator(int offset);
    bool writeCRC(int dataEndOffset);
    int readStoredNetworks(WifiCredential* credentials, int maxCount, bool validate);
};

#endif // SETUPWIFISTORE_H
