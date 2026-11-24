#ifndef PORTAL_BOOT_H
#define PORTAL_BOOT_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Shared boot / portal control contract (NVS).
 *
 * Namespace: "web3e_boot"
 * Keys (Preferences / NVS):
 *   "stay_portal" (uint8_t): 1 = stay in setup portal; do not auto-connect
 *                            or OTA-download app firmware. Wi-Fi credentials
 *                            in EEPROM are preserved and shown pre-filled.
 *   "boot_count"  (uint8_t): consecutive quick-reboot counter.
 *
 * EEPROM bytes 0-32 (KeyID) and the WifiStore region are NEVER touched here.
 *
 * --- Revocation (app firmware must match) ---
 * Before OTA-installing this setup firmware after an admin revoke, the app
 * firmware MUST set stay_portal = 1, then reboot into setup. Setup will open
 * the captive portal and will NOT immediately re-download the app using the
 * saved Wi-Fi. The flag is cleared when the user clicks "Done" in the portal
 * (re-provision / confirm).
 *
 * --- Offline reset: triple power-cycle (app + setup must match) ---
 * On every boot the counter is incremented. If it reaches
 * PORTAL_BOOT_TRIPLE_THRESHOLD (3) before being cleared, setup (or the app)
 * forces the portal path and sets stay_portal = 1 so the device remains in
 * the portal until the user confirms Done.
 * After PORTAL_BOOT_STABLE_MS (10000 ms) of continuous uptime the counter is
 * cleared to 0. A router outage / Wi-Fi down MUST NOT open the AP; only this
 * counter or stay_portal does.
 */

#define PORTAL_BOOT_NVS_NAMESPACE   "web3e_boot"
#define PORTAL_BOOT_KEY_STAY_PORTAL "stay_portal"
#define PORTAL_BOOT_KEY_BOOT_COUNT  "boot_count"

#define PORTAL_BOOT_TRIPLE_THRESHOLD 3
#define PORTAL_BOOT_STABLE_MS        10000UL

class PortalBoot {
public:
    // Open NVS namespace. Call once early in setup() (after Serial ok).
    bool begin();

    // Increment quick-reboot counter. If threshold reached, sets stay_portal
    // and resets the counter. Returns true if portal must be forced this boot.
    bool checkAndHandlePowerCycle();

    // True if stay_portal flag is set (revoke or prior triple-cycle).
    bool shouldStayInPortal() const;

    // Set / clear the stay-in-portal flag (app sets before revoke OTA).
    bool setStayInPortal(bool stay);
    bool clearStayInPortal();

    // Call from loop() after PORTAL_BOOT_STABLE_MS uptime to clear boot_count.
    void clearBootCountIfStable(unsigned long uptimeMs);

    uint8_t getBootCount() const;
    bool isInitialized() const { return initialized; }

private:
    bool initialized = false;
    bool bootCountCleared = false;
    uint8_t bootCount = 0;
    bool stayPortal = false;

    bool readState();
    bool writeStayPortal(uint8_t value);
    bool writeBootCount(uint8_t value);
};

#endif // PORTAL_BOOT_H
