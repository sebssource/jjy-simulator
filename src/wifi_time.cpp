#include "wifi_time.h"

#include "shared_state.h"

enum class WifiConnectState { DISCONNECTED, CONNECTING, CONNECTED };

static WifiConnectState s_wifiState = WifiConnectState::DISCONNECTED;
static uint32_t s_connectStartMs = 0;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

static void startWifiConnection()
{
    Serial.printf("[WiFi] Connecting to SSID: %s\n", DEV_DEFAULT_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(DEV_DEFAULT_SSID, DEV_DEFAULT_PASS);
    s_connectStartMs = millis();
    s_wifiState = WifiConnectState::CONNECTING;
}

// Non-blocking. Returns true once associated. Initiates the association in
// the background on first call (or after a timeout) and lets the WiFi driver
// progress asynchronously between calls.
bool connectWifiStep()
{
    if (WiFi.status() == WL_CONNECTED) {
        if (s_wifiState != WifiConnectState::CONNECTED) {
            Serial.printf("[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
        }
        s_wifiState = WifiConnectState::CONNECTED;
        return true;
    }

    if (s_wifiState != WifiConnectState::CONNECTING) {
        startWifiConnection();
        return false;
    }

    if (millis() - s_connectStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
        Serial.println("[WiFi] Connection timed out; will retry.");
        WiFi.disconnect(false);
        s_wifiState = WifiConnectState::DISCONNECTED;
    }
    return false;
}

// Bounded blocking wrapper — used only once during setup().
void connectWifi()
{
    const uint32_t startMs = millis();
    while (millis() - startMs < WIFI_CONNECT_TIMEOUT_MS) {
        if (connectWifiStep()) {
            return;
        }
        delay(250);
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
        connectWifiStep();
        return;
    }

    Serial.println("[NTP] Periodic re-sync request...");
    configTzTime(currentTzRule.c_str(), NTP1, NTP2);
    lastResyncAttemptMs = millis();
}

void setWifiAutoMode()
{
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (!WiFi.getSleep()) {
        Serial.println("[WiFi] AUTO mode: enabling modem sleep between windows.");
        WiFi.setSleep(true);
    }
}
