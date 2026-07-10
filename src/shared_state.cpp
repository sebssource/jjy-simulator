#include "shared_state.h"

#include <Preferences.h>
#include <WebServer.h>
#include <driver/mcpwm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "secrets.h"

// ----------------------- Hardware / WiFi -----------------------
const int PIN_IA = 25;
const int PIN_IB = 26;
#if defined(LED_BUILTIN)
const int PIN_TX_LED = LED_BUILTIN;
#else
const int PIN_TX_LED = 2;
#endif
const bool TX_LED_ACTIVE_HIGH = true;

const char* DEV_DEFAULT_SSID = WIFI_SSID;
const char* DEV_DEFAULT_PASS = WIFI_PASS;

// ----------------------- Time / NTP -----------------------
const char* TZ_SYDNEY = "AEST-10AEDT,M10.1.0,M4.1.0/3";
const char* NTP1 = "pool.ntp.org";
const char* NTP2 = "time.google.com";

const uint32_t NTP_SYNC_TIMEOUT_MS = 25000;
const uint32_t NTP_RESYNC_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;

// ----------------------- Daily TX schedule -----------------------
const uint8_t SCHEDULED_SLOT_COUNT = 2;
const uint8_t SCHEDULED_SLOT_HOUR[SCHEDULED_SLOT_COUNT] = { 2, 16 };
const uint8_t SCHEDULED_SLOT_MINUTE = 00;
const uint8_t SCHEDULED_SLOT_SECOND = 00;

const uint32_t TX_WINDOW_SECONDS = 30UL * 60UL;
const uint32_t WIFI_WAKE_LEAD_SECONDS = 2UL * 60UL;
const uint32_t TX_START_TOLERANCE_SECONDS = 1;
const uint32_t COLD_BOOT_BROADCAST_SECONDS = 30UL * 60UL;
const uint32_t MAIN_LOOP_PERIOD_MS = 25;
const uint32_t WEB_ACTIVITY_HOLD_MS = 10UL * 60UL * 1000UL;

const int SLOT_INDEX_NONE = -1;
const int SLOT_INDEX_COLD_BOOT = -1;
const int SLOT_INDEX_PERMANENT = -2;

// ----------------------- Carrier / Modulation -----------------------
const uint32_t CARRIER_HZ_DEFAULT = 40000;
const uint32_t CARRIER_HZ_MIN = 39500;
const uint32_t CARRIER_HZ_MAX = 40500;
const uint32_t CARRIER_HZ_STEP = 10;
const float CARRIER_DUTY_TARGET = 45.0f;
const uint32_t MCPWM_APB_CLK_HZ = 80000000;
const uint32_t MCPWM_DEADTIME_NS = 150;
const uint32_t CARRIER_RAMP_TOTAL_MS = 5;
const uint8_t CARRIER_RAMP_STEP_COUNT = 5;
const uint32_t CARRIER_RAMP_STEP_INTERVAL_MS = CARRIER_RAMP_TOTAL_MS / (CARRIER_RAMP_STEP_COUNT - 1);
const float CARRIER_RAMP_DUTY_STEPS[CARRIER_RAMP_STEP_COUNT] = {
    0.0f,
    CARRIER_DUTY_TARGET * 0.25f,
    CARRIER_DUTY_TARGET * 0.50f,
    CARRIER_DUTY_TARGET * 0.75f,
    CARRIER_DUTY_TARGET
};

const uint16_t ON_MS_MARKER = 200; // P
const uint16_t ON_MS_BIT1 = 500; // 1
const uint16_t ON_MS_BIT0 = 800; // 0

JjySymbol frameSymbols[60];
int lastFrameMinute = -1;
int lastFrameYDay = -1;
int lastFrameYear = -1;

int currentSecond = -1;
bool carrierIsOn = false;
bool symbolOnPhaseActive = false;
uint32_t symbolOnStartedMs = 0;
uint16_t activeOnDurationMs = 0;
uint32_t lastResyncAttemptMs = 0;
uint32_t currentCarrierHz = CARRIER_HZ_DEFAULT;
bool rampUpActive = false;
bool rampDownActive = false;
bool rampDownStartedForSymbol = false;
uint8_t rampStepIndex = 0;
uint32_t rampStepStartedMs = 0;

Preferences prefs;
bool prefsReady = false;
const char* NVS_NS = "jjy";
const char* NVS_KEY_FREQ = "freq_hz";
const char* NVS_KEY_WIFI_MODE = "wifi_mode";
const char* NVS_KEY_BROADCAST_MODE = "bcast_mode";
const char* NVS_KEY_SLEEP_MODE = "sleep_mode";
const char* NVS_KEY_WEB_OVERRIDE = "web_ovrd";
const char* NVS_KEY_PERM_BCAST = "perm_bcst";
const char* NVS_KEY_TZ = "tz_rule";

ModeState modeState = { WifiMode::AUTO, BroadcastMode::AUTO, SleepMode::AUTO };

uint32_t lastWebActivityMs = 0;

const char* BUILD_INFO = "Built " __DATE__ " " __TIME__;

String currentTzRule = "AEST-10AEDT,M10.1.0,M4.1.0/3";

WebServer webServer(80);
bool webServerReady = false;
bool txWindowActive = false;

time_t txWindowStartEpoch = 0;
time_t txWindowEndEpoch = 0;
int txWindowSlotIndex = SLOT_INDEX_NONE;
time_t lastMissedSlotEpoch = 0;
bool coldBootStartupPending = false;
bool coldBootBroadcastActive = false;

const BaseType_t JJY_TASK_CORE = 1;
const BaseType_t NET_TASK_CORE = 0;
const UBaseType_t JJY_TASK_PRIORITY = 4;
const UBaseType_t NET_TASK_PRIORITY = 2;
const uint32_t JJY_TASK_STACK_WORDS = 4096;
const uint32_t NET_TASK_STACK_WORDS = 4096;
const uint32_t JJY_TASK_PERIOD_MS = 1;
const uint32_t NET_TASK_PERIOD_MS = 250;

TaskHandle_t jjyTaskHandle = nullptr;
TaskHandle_t netTaskHandle = nullptr;

static uint8_t clampMode(uint8_t value, uint8_t max)
{
    return (value > max) ? 0 : value;
}

const char* wifiModeLabel(WifiMode mode)
{
    switch (mode) {
    case WifiMode::ON:  return "On";
    default:            return "Auto";
    }
}

const char* broadcastModeLabel(BroadcastMode mode)
{
    switch (mode) {
    case BroadcastMode::ON: return "On";
    default:                return "Auto";
    }
}

const char* sleepModeLabel(SleepMode mode)
{
    switch (mode) {
    case SleepMode::OFF: return "Off";
    default:             return "Auto";
    }
}

bool isRecentWebActivity()
{
    return (millis() - lastWebActivityMs) < WEB_ACTIVITY_HOLD_MS;
}

void markWebActivity()
{
    lastWebActivityMs = millis();
}

void loadModeState()
{
    if (!prefsReady) {
        modeState = { WifiMode::AUTO, BroadcastMode::AUTO, SleepMode::AUTO };
        return;
    }

    bool loaded = false;
    if (prefs.isKey(NVS_KEY_WIFI_MODE)) {
        modeState.wifiMode = static_cast<WifiMode>(clampMode(prefs.getUChar(NVS_KEY_WIFI_MODE, 0), 1));
        modeState.broadcastMode = static_cast<BroadcastMode>(clampMode(prefs.getUChar(NVS_KEY_BROADCAST_MODE, 0), 1));
        modeState.sleepMode = static_cast<SleepMode>(clampMode(prefs.getUChar(NVS_KEY_SLEEP_MODE, 0), 1));
        loaded = true;
    }

    if (!loaded && (prefs.isKey(NVS_KEY_WEB_OVERRIDE) || prefs.isKey(NVS_KEY_PERM_BCAST))) {
        const bool legacyWebOverride = prefs.getBool(NVS_KEY_WEB_OVERRIDE, false);
        const bool legacyPermBcast = prefs.getBool(NVS_KEY_PERM_BCAST, false);

        modeState.wifiMode = legacyWebOverride ? WifiMode::ON : WifiMode::AUTO;
        modeState.broadcastMode = legacyPermBcast ? BroadcastMode::ON : BroadcastMode::AUTO;
        modeState.sleepMode = SleepMode::AUTO;

        persistModeState(modeState);
        prefs.remove(NVS_KEY_WEB_OVERRIDE);
        prefs.remove(NVS_KEY_PERM_BCAST);
        loaded = true;
    }

    if (!loaded) {
        modeState = { WifiMode::AUTO, BroadcastMode::AUTO, SleepMode::AUTO };
    }
}

void persistModeState(const ModeState& state)
{
    if (!prefsReady) {
        return;
    }
    prefs.putUChar(NVS_KEY_WIFI_MODE, static_cast<uint8_t>(state.wifiMode));
    prefs.putUChar(NVS_KEY_BROADCAST_MODE, static_cast<uint8_t>(state.broadcastMode));
    prefs.putUChar(NVS_KEY_SLEEP_MODE, static_cast<uint8_t>(state.sleepMode));
}

void applyModeState(const ModeState& state)
{
    modeState = state;
}
