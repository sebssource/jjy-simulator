#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include "shared_state.h"

void connectWifi();
bool connectWifiStep();
bool initialTimeSync();
void periodicResync();

// Active Wi-Fi power states. Schedule-aware AUTO mode uses modem sleep
// between windows and full power during windows / web activity.
void setWifiAutoMode();
