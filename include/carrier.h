#pragma once

#include <stdint.h>

void setupCarrier();
void carrierOn();
void carrierOff();
void setTxLed(bool on);
void applyCarrierDuty(float dutyPercent);
void beginCarrierRampUp();
void beginCarrierRampDown();
void updateCarrierRamp();
uint32_t clampCarrierHz(uint32_t hz);
void saveCarrierHzToNvs(uint32_t hz);
void reconfigureCarrierTimer(uint32_t hz);
void setCarrierFrequency(uint32_t hz, bool persistToNvs);
