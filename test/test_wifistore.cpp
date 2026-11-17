#include <unity.h>
#include <string.h>
#include <cstdio>
#include "../lib/WifiStore/SetupWifiStore.h"

// Mock EEPROM for testing
static uint8_t mockEEPROM[TOTAL_EEPROM_SIZE];

class TestableWifiStore : public SetupWifiStore {
protected:
    uint8_t eepromRead(int address) override {
        if (address >= 0 && address < TOTAL_EEPROM_SIZE) {
            return mockEEPROM[address];
        }
        return 0;
    }
    
    void eepromWrite(int address, uint8_t value) override {
        if (address >= 0 && address < TOTAL_EEPROM_SIZE) {
            mockEEPROM[address] = value;
        }
    }
    
    bool eepromCommit() override {
        return true;
    }
};

void setUp(void) {
    memset(mockEEPROM, 0xFF, sizeof(mockEEPROM));
}

void tearDown(void) {
}

void test_empty_eeprom_invalid(void) {
    TestableWifiStore store;
    store.begin();
    TEST_ASSERT_FALSE(store.isValid());
}

void test_write_and_read_single_network(void) {
    TestableWifiStore store;
    store.begin();
    
    WifiCredential creds[1];
    strcpy(creds[0].ssid, "TestNetwork");
    creds[0].ssidLen = strlen(creds[0].ssid);
    strcpy(creds[0].password, "password123");
    creds[0].pwLen = strlen(creds[0].password);
    
    TEST_ASSERT_TRUE(store.writeNetworks(creds, 1));
    TEST_ASSERT_TRUE(store.isValid());
    
    WifiCredential readCreds[MAX_WIFI_NETWORKS];
    int count = store.readNetworks(readCreds, MAX_WIFI_NETWORKS);
    
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL_STRING("TestNetwork", readCreds[0].ssid);
    TEST_ASSERT_EQUAL_STRING("password123", readCreds[0].password);
}

void test_write_multiple_networks(void) {
    TestableWifiStore store;
    store.begin();
    
    WifiCredential creds[3];
    
    strcpy(creds[0].ssid, "Network1");
    creds[0].ssidLen = strlen(creds[0].ssid);
    strcpy(creds[0].password, "pass1");
    creds[0].pwLen = strlen(creds[0].password);
    
    strcpy(creds[1].ssid, "Network2");
    creds[1].ssidLen = strlen(creds[1].ssid);
    strcpy(creds[1].password, "");
    creds[1].pwLen = 0;
    
    strcpy(creds[2].ssid, "LongNetworkNameTest12345678");
    creds[2].ssidLen = strlen(creds[2].ssid);
    strcpy(creds[2].password, "VeryLongPassword1234567890ABCDEF");
    creds[2].pwLen = strlen(creds[2].password);
    
    TEST_ASSERT_TRUE(store.writeNetworks(creds, 3));
    TEST_ASSERT_TRUE(store.isValid());
    
    WifiCredential readCreds[MAX_WIFI_NETWORKS];
    int count = store.readNetworks(readCreds, MAX_WIFI_NETWORKS);
    
    TEST_ASSERT_EQUAL(3, count);
    TEST_ASSERT_EQUAL_STRING("Network1", readCreds[0].ssid);
    TEST_ASSERT_EQUAL_STRING("pass1", readCreds[0].password);
    TEST_ASSERT_EQUAL_STRING("Network2", readCreds[1].ssid);
    TEST_ASSERT_EQUAL_STRING("", readCreds[1].password);
    TEST_ASSERT_EQUAL_STRING("LongNetworkNameTest12345678", readCreds[2].ssid);
    TEST_ASSERT_EQUAL_STRING("VeryLongPassword1234567890ABCDEF", readCreds[2].password);
}

void test_append_network(void) {
    TestableWifiStore store;
    store.begin();
    
    TEST_ASSERT_TRUE(store.appendNetwork("First", "pass1"));
    TEST_ASSERT_EQUAL(1, store.getNetworkCount());
    
    TEST_ASSERT_TRUE(store.appendNetwork("Second", "pass2"));
    TEST_ASSERT_EQUAL(2, store.getNetworkCount());
    
    WifiCredential readCreds[MAX_WIFI_NETWORKS];
    int count = store.readNetworks(readCreds, MAX_WIFI_NETWORKS);
    
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL_STRING("First", readCreds[0].ssid);
    TEST_ASSERT_EQUAL_STRING("Second", readCreds[1].ssid);
}

void test_remove_network(void) {
    TestableWifiStore store;
    store.begin();
    
    store.appendNetwork("First", "pass1");
    store.appendNetwork("Second", "pass2");
    store.appendNetwork("Third", "pass3");
    
    TEST_ASSERT_EQUAL(3, store.getNetworkCount());
    
    TEST_ASSERT_TRUE(store.removeNetwork(1));
    TEST_ASSERT_EQUAL(2, store.getNetworkCount());
    
    WifiCredential readCreds[MAX_WIFI_NETWORKS];
    int count = store.readNetworks(readCreds, MAX_WIFI_NETWORKS);
    
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL_STRING("First", readCreds[0].ssid);
    TEST_ASSERT_EQUAL_STRING("Third", readCreds[1].ssid);
}

void test_clear_networks(void) {
    TestableWifiStore store;
    store.begin();
    
    store.appendNetwork("First", "pass1");
    store.appendNetwork("Second", "pass2");
    
    TEST_ASSERT_EQUAL(2, store.getNetworkCount());
    
    TEST_ASSERT_TRUE(store.clearNetworks());
    TEST_ASSERT_EQUAL(0, store.getNetworkCount());
    TEST_ASSERT_TRUE(store.isValid());
}

void test_crc_corruption_detection(void) {
    TestableWifiStore store;
    store.begin();
    
    WifiCredential creds[1];
    strcpy(creds[0].ssid, "TestNet");
    creds[0].ssidLen = strlen(creds[0].ssid);
    strcpy(creds[0].password, "testpass");
    creds[0].pwLen = strlen(creds[0].password);
    
    TEST_ASSERT_TRUE(store.writeNetworks(creds, 1));
    TEST_ASSERT_TRUE(store.isValid());
    
    // Corrupt a byte in the data
    mockEEPROM[WIFI_EEPROM_OFFSET + WIFI_HEADER_SIZE + 5]++;
    
    TEST_ASSERT_FALSE(store.isValid());
}

void test_max_ssid_length(void) {
    TestableWifiStore store;
    store.begin();
    
    char longSSID[MAX_SSID_LENGTH + 1];
    memset(longSSID, 'A', MAX_SSID_LENGTH);
    longSSID[MAX_SSID_LENGTH] = '\0';
    
    TEST_ASSERT_TRUE(store.appendNetwork(longSSID, "pass"));
    
    WifiCredential readCreds[MAX_WIFI_NETWORKS];
    int count = store.readNetworks(readCreds, MAX_WIFI_NETWORKS);
    
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL(MAX_SSID_LENGTH, readCreds[0].ssidLen);
}

void test_max_password_length(void) {
    TestableWifiStore store;
    store.begin();
    
    char longPass[MAX_PASSWORD_LENGTH + 1];
    memset(longPass, 'P', MAX_PASSWORD_LENGTH);
    longPass[MAX_PASSWORD_LENGTH] = '\0';
    
    TEST_ASSERT_TRUE(store.appendNetwork("TestSSID", longPass));
    
    WifiCredential readCreds[MAX_WIFI_NETWORKS];
    int count = store.readNetworks(readCreds, MAX_WIFI_NETWORKS);
    
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL(MAX_PASSWORD_LENGTH, readCreds[0].pwLen);
}

void test_open_network(void) {
    TestableWifiStore store;
    store.begin();
    
    TEST_ASSERT_TRUE(store.appendNetwork("OpenNetwork", ""));
    
    WifiCredential readCreds[MAX_WIFI_NETWORKS];
    int count = store.readNetworks(readCreds, MAX_WIFI_NETWORKS);
    
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL_STRING("OpenNetwork", readCreds[0].ssid);
    TEST_ASSERT_EQUAL_STRING("", readCreds[0].password);
    TEST_ASSERT_EQUAL(0, readCreds[0].pwLen);
}

void test_max_networks_limit(void) {
    TestableWifiStore store;
    store.begin();
    
    char ssid[20];
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        sprintf(ssid, "Network%d", i);
        TEST_ASSERT_TRUE(store.appendNetwork(ssid, "pass"));
    }
    
    TEST_ASSERT_EQUAL(MAX_WIFI_NETWORKS, store.getNetworkCount());
    
    // Try to add one more - should fail
    TEST_ASSERT_FALSE(store.appendNetwork("ExtraNetwork", "pass"));
    TEST_ASSERT_EQUAL(MAX_WIFI_NETWORKS, store.getNetworkCount());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    RUN_TEST(test_empty_eeprom_invalid);
    RUN_TEST(test_write_and_read_single_network);
    RUN_TEST(test_write_multiple_networks);
    RUN_TEST(test_append_network);
    RUN_TEST(test_remove_network);
    RUN_TEST(test_clear_networks);
    RUN_TEST(test_crc_corruption_detection);
    RUN_TEST(test_max_ssid_length);
    RUN_TEST(test_max_password_length);
    RUN_TEST(test_open_network);
    RUN_TEST(test_max_networks_limit);
    
    return UNITY_END();
}
