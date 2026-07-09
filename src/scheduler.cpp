#include "scheduler.h"

#include "carrier.h"
#include "jjy.h"
#include "web_server.h"
#include <esp_sleep.h>

bool getCurrentLocalTime(tm& localNow)
{
    const time_t nowEpoch = time(nullptr);
    if (nowEpoch < 100000) {
        return false;
    }
    localtime_r(&nowEpoch, &localNow);
    return true;
}

bool isValidEpoch(time_t epoch)
{
    return epoch >= 100000;
}

String formatEpochLocal(time_t epoch)
{
    if (!isValidEpoch(epoch)) {
        return String("n/a");
    }

    tm localNow;
    localtime_r(&epoch, &localNow);

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &localNow);
    return String(buf);
}

time_t buildSlotEpoch(const tm& baseLocal, int slotIndex, int dayOffset)
{
    tm slotTm = baseLocal;
    slotTm.tm_mday += dayOffset;
    slotTm.tm_hour = SCHEDULED_SLOT_HOUR[slotIndex];
    slotTm.tm_min = SCHEDULED_SLOT_MINUTE;
    slotTm.tm_sec = SCHEDULED_SLOT_SECOND;
    slotTm.tm_isdst = -1;
    return mktime(&slotTm);
}

bool findNextSlotEpoch(time_t nowEpoch, time_t& slotEpochOut, int& slotIndexOut)
{
    if (!isValidEpoch(nowEpoch)) {
        return false;
    }

    tm nowLocal;
    localtime_r(&nowEpoch, &nowLocal);

    bool found = false;
    time_t bestEpoch = 0;
    int bestIndex = -1;

    for (int i = 0; i < static_cast<int>(SCHEDULED_SLOT_COUNT); ++i) {
        time_t candidate = buildSlotEpoch(nowLocal, i, 0);
        if (candidate <= nowEpoch) {
            candidate = buildSlotEpoch(nowLocal, i, 1);
        }

        if (!found || candidate < bestEpoch) {
            found = true;
            bestEpoch = candidate;
            bestIndex = i;
        }
    }

    if (!found) {
        return false;
    }

    slotEpochOut = bestEpoch;
    slotIndexOut = bestIndex;
    return true;
}

bool findCurrentSlotEpoch(time_t nowEpoch, time_t& slotEpochOut, int& slotIndexOut)
{
    if (!isValidEpoch(nowEpoch)) {
        return false;
    }

    tm nowLocal;
    localtime_r(&nowEpoch, &nowLocal);

    for (int i = 0; i < static_cast<int>(SCHEDULED_SLOT_COUNT); ++i) {
        const time_t candidate = buildSlotEpoch(nowLocal, i, 0);
        if (nowEpoch >= candidate && nowEpoch < (candidate + static_cast<time_t>(TX_WINDOW_SECONDS))) {
            slotEpochOut = candidate;
            slotIndexOut = i;
            return true;
        }
    }

    return false;
}

#include "wifi_time.h"

void startColdBootBroadcastWindow(time_t nowEpoch)
{
    if (!isValidEpoch(nowEpoch)) {
        return;
    }

    txWindowActive = true;
    txWindowStartEpoch = nowEpoch;
    txWindowEndEpoch = nowEpoch + static_cast<time_t>(COLD_BOOT_BROADCAST_SECONDS);
    txWindowSlotIndex = -1;
    currentSecond = -1;
    symbolOnPhaseActive = false;
    carrierOff();

    coldBootStartupPending = false;
    coldBootBroadcastActive = true;

    Serial.printf("[BOOT] Cold-boot broadcast START at %s, end=%s (%lu sec)\n",
        formatEpochLocal(txWindowStartEpoch).c_str(),
        formatEpochLocal(txWindowEndEpoch).c_str(),
        static_cast<unsigned long>(COLD_BOOT_BROADCAST_SECONDS));
}

void startTxWindow(time_t slotEpoch, int slotIndex)
{
    txWindowActive = true;
    txWindowStartEpoch = slotEpoch;
    if (slotIndex == -2) {
        txWindowEndEpoch = slotEpoch + static_cast<time_t>(365 * 24 * 60 * 60);
    } else {
        txWindowEndEpoch = slotEpoch + static_cast<time_t>(TX_WINDOW_SECONDS);
    }
    txWindowSlotIndex = slotIndex;
    currentSecond = -1;
    symbolOnPhaseActive = false;
    carrierOff();

    if (slotIndex == -2) {
        Serial.printf("[SCHED] TX window START (permanent broadcast) at %s\n",
            formatEpochLocal(txWindowStartEpoch).c_str());
    } else {
        Serial.printf("[SCHED] TX window START idx=%d at %s, end=%s\n",
            txWindowSlotIndex,
            formatEpochLocal(txWindowStartEpoch).c_str(),
            formatEpochLocal(txWindowEndEpoch).c_str());
    }
}

void stopTxWindow(const char* reason)
{
    if (permanentBroadcastEnabled && txWindowActive) {
        Serial.printf("[SCHED] TX window stop request ignored while permanent broadcast is active.\n");
        return;
    }

    if (!txWindowActive) {
        return;
    }

    const bool wasColdBootBroadcast = coldBootBroadcastActive;

    txWindowActive = false;
    symbolOnPhaseActive = false;
    carrierOff();

    Serial.printf("[SCHED] TX window STOP (%s) at %s\n",
        reason,
        formatEpochLocal(time(nullptr)).c_str());
    txWindowSlotIndex = -1;

    if (wasColdBootBroadcast) {
        coldBootBroadcastActive = false;
        if (coldBootUiHoldActive) {
            Serial.println("[BOOT] Cold-boot broadcast ended. Waiting for /save before deep sleep.");
        }
    }
}

bool enterDeepSleepUntilNextSlot(time_t nowEpoch, const char* trigger)
{
    if (!isValidEpoch(nowEpoch)) {
        Serial.printf("[SLEEP] %s: current time not valid yet; skip sleep request.\n", trigger);
        return false;
    }

    time_t nextSlotEpoch = 0;
    int nextSlotIndex = -1;
    if (!findNextSlotEpoch(nowEpoch, nextSlotEpoch, nextSlotIndex)) {
        Serial.printf("[SLEEP] %s: next slot unavailable.\n", trigger);
        return false;
    }

    const time_t wakeEpoch = nextSlotEpoch - static_cast<time_t>(WIFI_WAKE_LEAD_SECONDS);
    if (wakeEpoch <= nowEpoch) {
        Serial.printf("[SLEEP] %s: wake time already passed.\n", trigger);
        return false;
    }

    const uint64_t sleepSeconds = static_cast<uint64_t>(wakeEpoch - nowEpoch);
    if (sleepSeconds == 0) {
        return false;
    }

    Serial.printf("[SLEEP] %s -> next slot idx=%d at %s, wake=%s, sleep=%llu sec\n",
        trigger,
        nextSlotIndex,
        formatEpochLocal(nextSlotEpoch).c_str(),
        formatEpochLocal(wakeEpoch).c_str(),
        static_cast<unsigned long long>(sleepSeconds));

    carrierOff();
    symbolOnPhaseActive = false;
    txWindowActive = false;
    txWindowSlotIndex = -1;
    coldBootStartupPending = false;
    coldBootBroadcastActive = false;
    coldBootUiHoldNoticePrinted = false;
    Serial.flush();

    esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
    delay(50);
    esp_deep_sleep_start();
    return true;
}

bool maybeEnterDeepSleep(time_t nowEpoch)
{
    if (modeState.broadcastMode == BroadcastMode::ON
        || modeState.wifiMode == WifiMode::ON
        || txWindowActive
        || coldBootStartupPending) {
        return false;
    }

    if (!isValidEpoch(nowEpoch)) {
        return false;
    }

    if (modeState.sleepMode == SleepMode::OFF) {
        if (!coldBootUiHoldActive) {
            return false;
        }
    }

    if (coldBootUiHoldActive) {
        if (!coldBootUiHoldNoticePrinted) {
            Serial.println("[BOOT] Deep sleep paused by UI activity during cold-boot broadcast. Press /save to continue.");
            coldBootUiHoldNoticePrinted = true;
        }
        return false;
    }

    coldBootUiHoldNoticePrinted = false;
    return enterDeepSleepUntilNextSlot(nowEpoch, "AUTO_SCHEDULE");
}

void updateScheduleController()
{
    const time_t nowEpoch = time(nullptr);
    if (!isValidEpoch(nowEpoch)) {
        return;
    }

    // State-machine-driven broadcast control.
    if (modeState.broadcastMode == BroadcastMode::ON) {
        // Permanent broadcast: keep an open-ended window alive.
        if (!txWindowActive) {
            startTxWindow(nowEpoch, -2);
        }
    } else if (coldBootStartupPending && !txWindowActive) {
        startColdBootBroadcastWindow(nowEpoch);
    } else if (txWindowActive && nowEpoch >= txWindowEndEpoch) {
        stopTxWindow("window complete");
    } else if (!txWindowActive) {
        time_t currentSlotEpoch = 0;
        int currentSlotIndex = -1;
        if (findCurrentSlotEpoch(nowEpoch, currentSlotEpoch, currentSlotIndex)) {
            if (nowEpoch <= (currentSlotEpoch + static_cast<time_t>(TX_START_TOLERANCE_SECONDS))) {
                startTxWindow(currentSlotEpoch, currentSlotIndex);
            } else if (lastMissedSlotEpoch != currentSlotEpoch) {
                const time_t lateBy = nowEpoch - currentSlotEpoch;
                Serial.printf("[SCHED] Missed slot idx=%d at %s (late by %ld sec). Skipping.\n",
                    currentSlotIndex,
                    formatEpochLocal(currentSlotEpoch).c_str(),
                    static_cast<long>(lateBy));
                lastMissedSlotEpoch = currentSlotEpoch;
            }
        }
    }

    // Apply Wi-Fi power policy based on user mode and schedule state.
    const bool wifiNeededNow = txWindowActive || coldBootBroadcastActive;
    if (modeState.wifiMode == WifiMode::OFF) {
        setWifiPowerState(false);
    } else if (modeState.wifiMode == WifiMode::ON || wifiNeededNow) {
        setWifiPowerState(true);
    } else {
        // AUTO and no active window: allow modem sleep.
        setWifiAutoMode();
    }

    maybeEnterDeepSleep(nowEpoch);
}
