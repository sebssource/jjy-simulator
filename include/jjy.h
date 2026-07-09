#pragma once

#include "shared_state.h"

uint16_t onDurationFor(JjySymbol symbol);
char symbolChar(JjySymbol symbol);
void setBit(int secondIndex, bool value);
int dayOfYear(const tm& localNow);
void buildJjyFrame(const tm& localNow);
bool shouldRebuildFrame(const tm& localNow);
void transmitSecond(const tm& localNow, int secIndex);
