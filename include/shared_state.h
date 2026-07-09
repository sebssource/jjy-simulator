#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <driver/mcpwm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

// ----------------------- Hardware / WiFi -----------------------
extern const int PIN_IA;
extern const int PIN_IB;
extern const int PIN_TX_LED;
extern const bool TX_LED_ACTIVE_HIGH;

extern const char* DEV_DEFAULT_SSID;
extern const char* DEV_DEFAULT_PASS;

// ----------------------- Time / NTP -----------------------
extern const char* TZ_SYDNEY;
extern const char* NTP1;
extern const char* NTP2;

extern const uint32_t NTP_SYNC_TIMEOUT_MS;
extern const uint32_t NTP_RESYNC_INTERVAL_MS;

// ----------------------- Daily TX schedule -----------------------
extern const uint8_t SCHEDULED_SLOT_COUNT;
extern const uint8_t SCHEDULED_SLOT_HOUR[];
extern const uint8_t SCHEDULED_SLOT_MINUTE;
extern const uint8_t SCHEDULED_SLOT_SECOND;

extern const uint32_t TX_WINDOW_SECONDS;
extern const uint32_t WIFI_WAKE_LEAD_SECONDS;
extern const uint32_t TX_START_TOLERANCE_SECONDS;
extern const uint32_t COLD_BOOT_BROADCAST_SECONDS;
extern const uint32_t MAIN_LOOP_PERIOD_MS;

// ----------------------- Carrier / Modulation -----------------------
extern const uint32_t CARRIER_HZ_DEFAULT;
extern const uint32_t CARRIER_HZ_MIN;
extern const uint32_t CARRIER_HZ_MAX;
extern const uint32_t CARRIER_HZ_STEP;
extern const float CARRIER_DUTY_TARGET;
extern const mcpwm_unit_t MCPWM_UNIT;
extern const mcpwm_timer_t MCPWM_TIMER;
extern const uint32_t MCPWM_APB_CLK_HZ;
extern const uint32_t MCPWM_DEADTIME_NS;
extern const uint32_t CARRIER_RAMP_TOTAL_MS;
extern const uint8_t CARRIER_RAMP_STEP_COUNT;
extern const uint32_t CARRIER_RAMP_STEP_INTERVAL_MS;
extern const float CARRIER_RAMP_DUTY_STEPS[];

extern const uint16_t ON_MS_MARKER;
extern const uint16_t ON_MS_BIT1;
extern const uint16_t ON_MS_BIT0;

enum class JjySymbol : uint8_t {
    ZERO,
    ONE,
    MARKER,
};

extern JjySymbol frameSymbols[60];
extern int lastFrameMinute;
extern int lastFrameYDay;
extern int lastFrameYear;

extern int currentSecond;
extern bool carrierIsOn;
extern bool symbolOnPhaseActive;
extern uint32_t symbolOnStartedMs;
extern uint16_t activeOnDurationMs;
extern uint32_t lastResyncAttemptMs;
extern uint32_t currentCarrierHz;
extern bool rampUpActive;
extern bool rampDownActive;
extern bool rampDownStartedForSymbol;
extern uint8_t rampStepIndex;
extern uint32_t rampStepStartedMs;

extern Preferences prefs;
extern bool prefsReady;
extern const char* NVS_NS;
extern const char* NVS_KEY_FREQ;
extern const char* NVS_KEY_WIFI_MODE;
extern const char* NVS_KEY_BROADCAST_MODE;
extern const char* NVS_KEY_SLEEP_MODE;
extern const char* NVS_KEY_WEB_OVERRIDE;
extern const char* NVS_KEY_PERM_BCAST;
extern const char* NVS_KEY_TZ;

// ----------------------- Persisted modes -----------------------
enum class WifiMode : uint8_t { AUTO = 0, ON = 1, OFF = 2 };
enum class BroadcastMode : uint8_t { AUTO = 0, ON = 1 };
enum class SleepMode : uint8_t { AUTO = 0, OFF = 1 };

struct ModeState {
    WifiMode wifiMode;
    BroadcastMode broadcastMode;
    SleepMode sleepMode;
};

extern ModeState modeState;

void loadModeState();
void persistModeState(const ModeState& state);
void applyModeState(const ModeState& state);

// Derived from modeState for compatibility with existing scheduler code.
extern bool webOverrideEnabled;   // true when wifiMode == ON
extern bool permanentBroadcastEnabled; // true when broadcastMode == ON
extern const char* BUILD_INFO;
extern String currentTzRule;

extern WebServer webServer;
extern bool webServerReady;
extern bool txWindowActive;
extern time_t txWindowStartEpoch;
extern time_t txWindowEndEpoch;
extern int txWindowSlotIndex;
extern time_t lastMissedSlotEpoch;
extern bool coldBootStartupPending;
extern bool coldBootBroadcastActive;
extern bool coldBootUiHoldActive;
extern bool coldBootUiHoldNoticePrinted;

extern const BaseType_t JJY_TASK_CORE;
extern const BaseType_t NET_TASK_CORE;
extern const UBaseType_t JJY_TASK_PRIORITY;
extern const UBaseType_t NET_TASK_PRIORITY;
extern const uint32_t JJY_TASK_STACK_WORDS;
extern const uint32_t NET_TASK_STACK_WORDS;
extern const uint32_t JJY_TASK_PERIOD_MS;
extern const uint32_t NET_TASK_PERIOD_MS;

extern TaskHandle_t jjyTaskHandle;
extern TaskHandle_t netTaskHandle;
