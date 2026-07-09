#include "wifi_time.h"

#include "shared_state.h"

void connectWifi()
{
    if (WiFi.status() == WL_CONNECTED) {
        return;
    }

    Serial.printf("[WiFi] Connecting to SSID: %s\n", DEV_DEFAULT_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(DEV_DEFAULT_SSID, DEV_DEFAULT_PASS);

    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < 20000) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[WiFi] Connection failed. Will retry in loop.");
    }
}

bool initialTimeSync()
{
    configTzTime(currentTzRule.c_str(), NTP1, NTP2);

    tm timeInfo;
    uint32_t startMs = millis();
    while ((millis() - startMs) < NTP_SYNC_TIMEOUT_MS) {
        if (getLocalTime(&timeInfo, 250)) {
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeInfo);
            Serial.printf("[NTP] Initial sync OK. Local time: %s\n", buf);
            lastResyncAttemptMs = millis();
            return true;
        }
    }

    Serial.println("[NTP] Initial sync timeout.");
    return false;
}

void periodicResync()
{
    if (lastResyncAttemptMs != 0 && (millis() - lastResyncAttemptMs) < NTP_RESYNC_INTERVAL_MS) {
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        connectWifi();
    }

    Serial.println("[NTP] Periodic re-sync request...");
    configTzTime(currentTzRule.c_str(), NTP1, NTP2);
    lastResyncAttemptMs = millis();
}

static bool s_wifiPowerState = true;

void setWifiPowerState(bool highPower)
{
    if (WiFi.status() != WL_CONNECTED && highPower) {
        connectWifi();
    }

    if (highPower) {
        if (WiFi.getSleep()) {
            Serial.println("[WiFi] Full power mode requested.");
            WiFi.setSleep(false);
        }
    } else {
        if (s_wifiPowerState || !WiFi.getSleep()) {
            Serial.println("[WiFi] Wi-Fi disabled by user mode.");
            disconnectWifi();
        }
    }
    s_wifiPowerState = highPower;
}

void setWifiAutoMode()
{
    if (WiFi.status() != WL_CONNECTED) {
        connectWifi();
    }

    if (!WiFi.getSleep()) {
        Serial.println("[WiFi] AUTO mode: enabling modem sleep between windows.");
        WiFi.setSleep(true);
    }
}

void disconnectWifi()
{
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect();
        WiFi.mode(WIFI_OFF);
    }
}
