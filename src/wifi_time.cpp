#include "wifi_time.h"

#include <esp_sntp.h>

#include "shared_state.h"

// Called by the SNTP daemon only when a sync actually succeeds (it does not
// fire on failure; the daemon just retries on the next interval). Records the
// sync time and logs it.
static void onSntpSync(struct timeval* tv)
{
    lastSyncEpoch = tv->tv_sec;
    struct tm t;
    localtime_r(&tv->tv_sec, &t);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    Serial.printf("[NTP] Daemon sync OK: %s\n", buf);
}

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

    // Hand ongoing sync ownership to the SNTP daemon instead of re-initializing
    // it from periodicResync(). The daemon polls at this interval on its own
    // (default 1h), which keeps the clock correct to <0.2s in permanent-broadcast
    // mode and is more than enough given the ESP32's sub-second daily drift.
    sntp_set_sync_interval(NTP_RESYNC_INTERVAL_MS);
    sntp_set_time_sync_notification_cb(onSntpSync);

    tm timeInfo;
    uint32_t startMs = millis();
    while ((millis() - startMs) < NTP_SYNC_TIMEOUT_MS) {
        if (getLocalTime(&timeInfo, 250)) {
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeInfo);
            Serial.printf("[NTP] Initial sync OK. Local time: %s\n", buf);
            Serial.printf("[NTP] SNTP daemon polling every %lu ms\n", NTP_RESYNC_INTERVAL_MS);
            lastSyncEpoch = time(nullptr);
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
    // NOTE: Re-initializing the SNTP daemon here is intentionally disabled.
    // configTzTime() calls sntp_stop()+sntp_init() again, which both restarts
    // the poll timer and can briefly disrupt an in-flight sync. The daemon now
    // owns ongoing sync via sntp_set_sync_interval() set in initialTimeSync(),
    // so a redundant re-init is unnecessary. Kept for reference:
    // configTzTime(currentTzRule.c_str(), NTP1, NTP2);
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
