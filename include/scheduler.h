#pragma once

#include <Arduino.h>
#include <time.h>

void updateScheduler();

bool getCurrentLocalTime(tm& localNow);
bool isValidEpoch(time_t epoch);
String formatEpochLocal(time_t epoch);
time_t buildSlotEpoch(const tm& baseLocal, int slotIndex, int dayOffset);
bool findNextSlotEpoch(time_t nowEpoch, time_t& slotEpochOut, int& slotIndexOut);
bool findCurrentSlotEpoch(time_t nowEpoch, time_t& slotEpochOut, int& slotIndexOut);

void startColdBootBroadcastWindow(time_t nowEpoch);
void startTxWindow(time_t slotEpoch, int slotIndex);
void stopTxWindow(const char* reason);
bool enterDeepSleepUntilNextSlot(time_t nowEpoch, const char* trigger);
