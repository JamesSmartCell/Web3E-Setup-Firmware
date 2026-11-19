#ifndef NATIVE_TEST
#include <Arduino.h>
#include <Preferences.h>
#endif

#include "PortalBoot.h"

#ifndef NATIVE_TEST
static Preferences portalPrefs;
#endif

bool PortalBoot::begin() {
#ifndef NATIVE_TEST
    // Read/write NVS namespace shared with app firmware. Never touches EEPROM.
    if (!portalPrefs.begin(PORTAL_BOOT_NVS_NAMESPACE, false)) {
        Serial.println("ERROR: PortalBoot NVS begin failed");
        initialized = false;
        return false;
    }
#endif
    initialized = true;
    bootCountCleared = false;
    return readState();
}

bool PortalBoot::readState() {
    if (!initialized) return false;
#ifndef NATIVE_TEST
    stayPortal = portalPrefs.getUChar(PORTAL_BOOT_KEY_STAY_PORTAL, 0) != 0;
    bootCount = portalPrefs.getUChar(PORTAL_BOOT_KEY_BOOT_COUNT, 0);
#else
    stayPortal = false;
    bootCount = 0;
#endif
    return true;
}

bool PortalBoot::writeStayPortal(uint8_t value) {
    if (!initialized) return false;
#ifndef NATIVE_TEST
    size_t written = portalPrefs.putUChar(PORTAL_BOOT_KEY_STAY_PORTAL, value);
    if (written == 0) {
        // putUChar returns 0 on failure; also returns 0 if value unchanged on
        // some cores — re-read to confirm.
        if (portalPrefs.getUChar(PORTAL_BOOT_KEY_STAY_PORTAL, 0xFF) != value) {
            return false;
        }
    }
#endif
    stayPortal = (value != 0);
    return true;
}

bool PortalBoot::writeBootCount(uint8_t value) {
    if (!initialized) return false;
#ifndef NATIVE_TEST
    size_t written = portalPrefs.putUChar(PORTAL_BOOT_KEY_BOOT_COUNT, value);
    if (written == 0) {
        if (portalPrefs.getUChar(PORTAL_BOOT_KEY_BOOT_COUNT, 0xFF) != value) {
            return false;
        }
    }
#endif
    bootCount = value;
    return true;
}

bool PortalBoot::checkAndHandlePowerCycle() {
    if (!initialized) return shouldStayInPortal();

    uint8_t next = bootCount;
    if (next < 255) {
        next++;
    }
    writeBootCount(next);

#ifndef NATIVE_TEST
    Serial.printf("PortalBoot: quick-reboot count = %u (threshold %u)\n",
                  (unsigned)bootCount, (unsigned)PORTAL_BOOT_TRIPLE_THRESHOLD);
#endif

    if (bootCount >= PORTAL_BOOT_TRIPLE_THRESHOLD) {
#ifndef NATIVE_TEST
        Serial.println("PortalBoot: triple power-cycle detected — forcing portal");
#endif
        writeStayPortal(1);
        writeBootCount(0);
        bootCountCleared = true; // avoid immediate re-trigger logic confusion
        return true;
    }

    return shouldStayInPortal();
}

bool PortalBoot::shouldStayInPortal() const {
    return stayPortal;
}

bool PortalBoot::setStayInPortal(bool stay) {
    return writeStayPortal(stay ? 1 : 0);
}

bool PortalBoot::clearStayInPortal() {
    return writeStayPortal(0);
}

void PortalBoot::clearBootCountIfStable(unsigned long uptimeMs) {
    if (!initialized || bootCountCleared) return;
    if (uptimeMs < PORTAL_BOOT_STABLE_MS) return;
    if (bootCount == 0) {
        bootCountCleared = true;
        return;
    }
#ifndef NATIVE_TEST
    Serial.printf("PortalBoot: uptime >= %lu ms — clearing boot_count\n",
                  (unsigned long)PORTAL_BOOT_STABLE_MS);
#endif
    writeBootCount(0);
    bootCountCleared = true;
}

uint8_t PortalBoot::getBootCount() const {
    return bootCount;
}
