#include "jjy.h"

#include "carrier.h"

uint16_t onDurationFor(JjySymbol symbol)
{
    switch (symbol) {
    case JjySymbol::MARKER:
        return ON_MS_MARKER;
    case JjySymbol::ONE:
        return ON_MS_BIT1;
    default:
        return ON_MS_BIT0;
    }
}

char symbolChar(JjySymbol symbol)
{
    switch (symbol) {
    case JjySymbol::MARKER:
        return 'P';
    case JjySymbol::ONE:
        return '1';
    default:
        return '0';
    }
}

void setBit(int secondIndex, bool value)
{
    if (secondIndex < 0 || secondIndex > 59) {
        return;
    }
    frameSymbols[secondIndex] = value ? JjySymbol::ONE : JjySymbol::ZERO;
}

int dayOfYear(const tm& localNow)
{
    return localNow.tm_yday + 1;
}

void buildJjyFrame(const tm& localNow)
{
    for (int i = 0; i < 60; ++i) {
        frameSymbols[i] = JjySymbol::ZERO;
    }

    frameSymbols[0] = JjySymbol::MARKER;
    frameSymbols[9] = JjySymbol::MARKER;
    frameSymbols[19] = JjySymbol::MARKER;
    frameSymbols[29] = JjySymbol::MARKER;
    frameSymbols[39] = JjySymbol::MARKER;
    frameSymbols[49] = JjySymbol::MARKER;
    frameSymbols[59] = JjySymbol::MARKER;

    const int minute = localNow.tm_min;
    const int hour = localNow.tm_hour;
    const int yday = dayOfYear(localNow);
    const int year2 = (localNow.tm_year + 1900) % 100;
    const int wday = localNow.tm_wday;

    setBit(1, (minute / 10) & 0x4);
    setBit(2, (minute / 10) & 0x2);
    setBit(3, (minute / 10) & 0x1);
    setBit(5, (minute % 10) & 0x8);
    setBit(6, (minute % 10) & 0x4);
    setBit(7, (minute % 10) & 0x2);
    setBit(8, (minute % 10) & 0x1);

    setBit(12, (hour / 10) & 0x2);
    setBit(13, (hour / 10) & 0x1);
    setBit(15, (hour % 10) & 0x8);
    setBit(16, (hour % 10) & 0x4);
    setBit(17, (hour % 10) & 0x2);
    setBit(18, (hour % 10) & 0x1);

    const int ydayHundreds = yday / 100;
    const int ydayTens = (yday / 10) % 10;
    const int ydayOnes = yday % 10;
    setBit(22, ydayHundreds & 0x2);
    setBit(23, ydayHundreds & 0x1);
    setBit(25, ydayTens & 0x8);
    setBit(26, ydayTens & 0x4);
    setBit(27, ydayTens & 0x2);
    setBit(28, ydayTens & 0x1);
    setBit(30, ydayOnes & 0x8);
    setBit(31, ydayOnes & 0x4);
    setBit(32, ydayOnes & 0x2);
    setBit(33, ydayOnes & 0x1);

    const uint8_t pa1 = ((hour / 10) & 0x2 ? 1 : 0) + ((hour / 10) & 0x1 ? 1 : 0) + ((hour % 10) & 0x8 ? 1 : 0) + ((hour % 10) & 0x4 ? 1 : 0) + ((hour % 10) & 0x2 ? 1 : 0) + ((hour % 10) & 0x1 ? 1 : 0);
    const uint8_t pa2 = ((minute / 10) & 0x4 ? 1 : 0) + ((minute / 10) & 0x2 ? 1 : 0) + ((minute / 10) & 0x1 ? 1 : 0) + ((minute % 10) & 0x8 ? 1 : 0) + ((minute % 10) & 0x4 ? 1 : 0) + ((minute % 10) & 0x2 ? 1 : 0) + ((minute % 10) & 0x1 ? 1 : 0);
    setBit(36, (pa1 & 0x1) != 0);
    setBit(37, (pa2 & 0x1) != 0);

    const int yearTens = year2 / 10;
    const int yearOnes = year2 % 10;
    setBit(41, yearTens & 0x8);
    setBit(42, yearTens & 0x4);
    setBit(43, yearTens & 0x2);
    setBit(44, yearTens & 0x1);
    setBit(45, yearOnes & 0x8);
    setBit(46, yearOnes & 0x4);
    setBit(47, yearOnes & 0x2);
    setBit(48, yearOnes & 0x1);

    setBit(50, wday & 0x4);
    setBit(51, wday & 0x2);
    setBit(52, wday & 0x1);

    lastFrameMinute = localNow.tm_min;
    lastFrameYDay = localNow.tm_yday;
    lastFrameYear = localNow.tm_year;
}

bool shouldRebuildFrame(const tm& localNow)
{
    return lastFrameMinute != localNow.tm_min || lastFrameYDay != localNow.tm_yday || lastFrameYear != localNow.tm_year;
}

void transmitSecond(const tm& localNow, int secIndex)
{
    const JjySymbol symbol = frameSymbols[secIndex];
    activeOnDurationMs = onDurationFor(symbol);
    symbolOnStartedMs = millis();
    symbolOnPhaseActive = true;
    rampDownStartedForSymbol = false;

    carrierOff();
    beginCarrierRampUp();

    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &localNow);
    const uint16_t offMs = 1000 - activeOnDurationMs;
    Serial.printf("[TX] %s sec=%02d bit=%c ON=%ums OFF=%ums\n", tbuf, secIndex, symbolChar(symbol), activeOnDurationMs, offMs);
}
