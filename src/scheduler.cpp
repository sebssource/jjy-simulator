#include "scheduler.h"

#include <esp_sleep.h>

#include "carrier.h"
#include "jjy.h"
#include "web_server.h"
#include "wifi_time.h"

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
    int bestIndex = SLOT_INDEX_NONE;

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

static void startWindow(time_t startEpoch, int slotIndex, uint32_t durationSeconds, const char* label)
{
    txWindowActive = true;
    txWindowStartEpoch = startEpoch;
    txWindowEndEpoch = startEpoch + static_cast<time_t>(durationSeconds);
    txWindowSlotIndex = slotIndex;
    currentSecond = -1;
    symbolOnPhaseActive = false;
    carrierOff();

    Serial.printf("[SCHED] %s START idx=%d at %s, end=%s\n",
        label,
        slotIndex,
        formatEpochLocal(txWindowStartEpoch).c_str(),
        formatEpochLocal(txWindowEndEpoch).c_str());
}

void startColdBootBroadcastWindow(time_t nowEpoch)
{
    if (!isValidEpoch(nowEpoch)) {
        return;
    }

    startWindow(nowEpoch, SLOT_INDEX_COLD_BOOT, COLD_BOOT_BROADCAST_SECONDS, "Cold-boot broadcast");
    coldBootStartupPending = false;
    coldBootBroadcastActive = true;
}

void startTxWindow(time_t slotEpoch, int slotIndex)
{
    startWindow(slotEpoch, slotIndex, TX_WINDOW_SECONDS, "TX window");
}

void startPermanentBroadcast(time_t nowEpoch)
{
    startWindow(nowEpoch, SLOT_INDEX_PERMANENT, 365UL * 24UL * 60UL * 60UL, "Permanent broadcast");
}

void stopTxWindow(const char* reason)
{
    if (modeState.broadcastMode == BroadcastMode::ON && txWindowActive) {
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
    txWindowSlotIndex = SLOT_INDEX_NONE;

    if (wasColdBootBroadcast) {
        coldBootBroadcastActive = false;
        Serial.println("[BOOT] Cold-boot broadcast ended.");
    }
}

bool enterDeepSleepUntilNextSlot(time_t nowEpoch, const char* trigger)
{
    if (!isValidEpoch(nowEpoch)) {
        Serial.printf("[SLEEP] %s: current time not valid yet; skip sleep request.\n", trigger);
        return false;
    }

    time_t nextSlotEpoch = 0;
    int nextSlotIndex = SLOT_INDEX_NONE;
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
    txWindowSlotIndex = SLOT_INDEX_NONE;
    coldBootStartupPending = false;
    coldBootBroadcastActive = false;
    Serial.flush();

    esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
    delay(50);
    esp_deep_sleep_start();
    return true;
}

static bool shouldStayAwake()
{
    return modeState.broadcastMode == BroadcastMode::ON
        || modeState.wifiMode == WifiMode::ON
        || txWindowActive
        || coldBootStartupPending
        || isRecentWebActivity();
}

static void updateBroadcastState(time_t nowEpoch)
{
    if (modeState.broadcastMode == BroadcastMode::ON) {
        if (!txWindowActive || txWindowSlotIndex != SLOT_INDEX_PERMANENT) {
            startPermanentBroadcast(nowEpoch);
        }
        return;
    }

    if (txWindowActive && txWindowSlotIndex == SLOT_INDEX_PERMANENT) {
        stopTxWindow("permanent broadcast disabled");
        return;
    }

    if (coldBootStartupPending && !txWindowActive) {
        startColdBootBroadcastWindow(nowEpoch);
        return;
    }

    if (txWindowActive && nowEpoch >= txWindowEndEpoch) {
        stopTxWindow("window complete");
        return;
    }

    if (!txWindowActive) {
        time_t currentSlotEpoch = 0;
        int currentSlotIndex = SLOT_INDEX_NONE;
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
}

static void updateWifiPowerState()
{
    const bool wifiNeededNow = txWindowActive || coldBootBroadcastActive;
    if (modeState.wifiMode == WifiMode::ON || wifiNeededNow) {
        if (!WiFi.getSleep()) {
            // already full power
        } else {
            Serial.println("[WiFi] Full power mode requested.");
            WiFi.setSleep(false);
        }
    } else {
        // AUTO with no active window: allow modem sleep.
        setWifiAutoMode();
    }
}

static void updateSleepState(time_t nowEpoch)
{
    if (!isValidEpoch(nowEpoch)) {
        return;
    }

    if (shouldStayAwake()) {
        return;
    }

    if (modeState.sleepMode == SleepMode::OFF) {
        return;
    }

    enterDeepSleepUntilNextSlot(nowEpoch, "AUTO_SCHEDULE");
}

void updateScheduler()
{
    const time_t nowEpoch = time(nullptr);
    if (!isValidEpoch(nowEpoch)) {
        return;
    }

    updateBroadcastState(nowEpoch);
    updateWifiPowerState();
    updateSleepState(nowEpoch);
}
