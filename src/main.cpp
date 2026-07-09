#include "carrier.h"
#include "jjy.h"
#include "scheduler.h"
#include "shared_state.h"
#include "web_server.h"
#include "wifi_time.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>

/*
  JJY 40 kHz simulator (Fukushima frequency) for ESP32 + L9110S.

  Antiphase drive concept:
  - IA and IB are both 40 kHz square waves (target duty configurable, default 45%).
  - IB is the inverted PWM waveform (~180 deg relative to IA).
  - Across the tank (between OA and OB), voltage swing is increased vs single-ended GPIO.

  Modulation method:
  - Carrier ON  = both PWM channels active.
  - Carrier OFF = duty set to 0 on both channels.
*/

void handleSerialCommands()
{
    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());
        switch (c) {
        case '+':
            setCarrierFrequency(static_cast<uint32_t>(static_cast<int32_t>(currentCarrierHz) + static_cast<int32_t>(CARRIER_HZ_STEP)), true);
            break;
        case '-': {
            int32_t nextHz = static_cast<int32_t>(currentCarrierHz) - static_cast<int32_t>(CARRIER_HZ_STEP);
            if (nextHz < 0) {
                nextHz = 0;
            }
            setCarrierFrequency(static_cast<uint32_t>(nextHz), true);
        } break;
        case '?':
            Serial.printf("[PWM] Frequency=%lu Hz, Range=%lu..%lu, Step=%lu\n", currentCarrierHz, CARRIER_HZ_MIN, CARRIER_HZ_MAX, CARRIER_HZ_STEP);
            break;
        case '\r':
        case '\n':
        case ' ':
        case '\t':
            break;
        default:
            Serial.printf("[PWM] Unknown cmd '%c'. Use + / - / ?\n", c);
            break;
        }
    }
}

void runJjySchedulerTick()
{
    handleSerialCommands();
    updateCarrierRamp();

    if (!txWindowActive) {
        if (symbolOnPhaseActive || carrierIsOn) {
            carrierOff();
            symbolOnPhaseActive = false;
        }
        return;
    }

    tm nowLocal;
    if (!getCurrentLocalTime(nowLocal)) {
        carrierOff();
        symbolOnPhaseActive = false;
        return;
    }

    if (shouldRebuildFrame(nowLocal)) {
        buildJjyFrame(nowLocal);
        Serial.printf("[FRAME] rebuilt for %04d-%03d %02d:%02d (current TZ)\n",
            nowLocal.tm_year + 1900,
            dayOfYear(nowLocal),
            nowLocal.tm_hour,
            nowLocal.tm_min);
    }

    if (nowLocal.tm_sec != currentSecond) {
        currentSecond = nowLocal.tm_sec;
        transmitSecond(nowLocal, currentSecond);
    }

    if (symbolOnPhaseActive) {
        const uint32_t elapsedOnMs = millis() - symbolOnStartedMs;
        if (!rampDownStartedForSymbol && activeOnDurationMs > CARRIER_RAMP_TOTAL_MS) {
            const uint32_t rampDownStartMs = activeOnDurationMs - CARRIER_RAMP_TOTAL_MS;
            if (elapsedOnMs >= rampDownStartMs) {
                beginCarrierRampDown();
            }
        }

        if (elapsedOnMs >= activeOnDurationMs) {
            carrierOff();
            symbolOnPhaseActive = false;
        }
    }
}

void jjyTask(void* arg)
{
    (void)arg;
    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        runJjySchedulerTick();
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(JJY_TASK_PERIOD_MS));
    }
}

void networkTask(void* arg)
{
    (void)arg;

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            connectWifi();
        }
        periodicResync();
        vTaskDelay(pdMS_TO_TICKS(NET_TASK_PERIOD_MS));
    }
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\nJJY 40kHz simulator starting...");

    const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
    const bool isColdBoot = wakeCause != ESP_SLEEP_WAKEUP_TIMER;
    Serial.printf("[BOOT] Wake cause: %d\n", static_cast<int>(wakeCause));

    coldBootStartupPending = isColdBoot;
    coldBootBroadcastActive = false;

    pinMode(PIN_TX_LED, OUTPUT);
    setTxLed(false);
    Serial.printf("[IO] TX LED GPIO%d\n", PIN_TX_LED);

    prefsReady = prefs.begin(NVS_NS, false);
    if (!prefsReady) {
        Serial.println("[NVS] Preferences begin failed. Using default settings.");
    } else {
        const uint32_t savedHz = prefs.getUInt(NVS_KEY_FREQ, CARRIER_HZ_DEFAULT);
        currentCarrierHz = clampCarrierHz(savedHz);
        currentTzRule = prefs.getString(NVS_KEY_TZ, currentTzRule);
        loadModeState();

        if (savedHz != currentCarrierHz) {
            saveCarrierHzToNvs(currentCarrierHz);
        }
        Serial.printf("[NVS] Loaded carrier frequency: %lu Hz\n", currentCarrierHz);
        Serial.printf("[NVS] Loaded timezone rule: %s\n", currentTzRule.c_str());
    }

    setupCarrier();
    Serial.printf("[PWM] Soft ramp enabled: up/down %lums with %u steps\n", CARRIER_RAMP_TOTAL_MS, CARRIER_RAMP_STEP_COUNT);
    connectWifi();

    if (!initialTimeSync()) {
        Serial.println("[NTP] Continuing; will retry while keeping carrier OFF until time is valid.");
        lastResyncAttemptMs = 0;
    }

    setupWebServer();

    if (coldBootStartupPending) {
        if (modeState.broadcastMode == BroadcastMode::ON) {
            coldBootStartupPending = false;
            Serial.println("[BOOT] Permanent broadcast active; skipping cold-boot broadcast window.");
        } else {
            const time_t nowEpoch = time(nullptr);
            if (isValidEpoch(nowEpoch)) {
                startColdBootBroadcastWindow(nowEpoch);
            } else {
                Serial.println("[BOOT] Cold-boot broadcast pending until time becomes valid.");
            }
        }
    }

    xTaskCreatePinnedToCore(jjyTask,
        "jjy_tx",
        JJY_TASK_STACK_WORDS,
        nullptr,
        JJY_TASK_PRIORITY,
        &jjyTaskHandle,
        JJY_TASK_CORE);

    xTaskCreatePinnedToCore(networkTask,
        "net_maint",
        NET_TASK_STACK_WORDS,
        nullptr,
        NET_TASK_PRIORITY,
        &netTaskHandle,
        NET_TASK_CORE);

    Serial.printf("[TASK] JJY task core=%d prio=%u, NET task core=%d prio=%u\n",
        static_cast<int>(JJY_TASK_CORE),
        static_cast<unsigned>(JJY_TASK_PRIORITY),
        static_cast<int>(NET_TASK_CORE),
        static_cast<unsigned>(NET_TASK_PRIORITY));
}

void loop()
{
    if (webServerReady && WiFi.status() == WL_CONNECTED) {
        webServer.handleClient();
    }

    updateScheduler();
    delay(MAIN_LOOP_PERIOD_MS);
}
