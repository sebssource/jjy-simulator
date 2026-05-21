#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <driver/mcpwm.h>
#include <esp_sleep.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "secrets.h"

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

// ----------------------- Hardware / WiFi -----------------------
static const int PIN_IA = 25;
static const int PIN_IB = 26;
#if defined(LED_BUILTIN)
static const int PIN_TX_LED = LED_BUILTIN;
#else
static const int PIN_TX_LED = 2;
#endif
static const bool TX_LED_ACTIVE_HIGH = true;

static const char *DEV_DEFAULT_SSID = WIFI_SSID;
static const char *DEV_DEFAULT_PASS = WIFI_PASS;

// ----------------------- Time / NTP -----------------------
// Warsaw, Poland (CET/CEST DST rules)
static const char *TZ_WARSAW = "CET-1CEST,M3.5.0/2,M10.5.0/3";
static const char *NTP1 = "pool.ntp.org";
static const char *NTP2 = "time.google.com";

static const uint32_t NTP_SYNC_TIMEOUT_MS = 25000;
static const uint32_t NTP_RESYNC_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;

// ----------------------- Daily TX schedule -----------------------
static const uint8_t SCHEDULED_SLOT_COUNT = 4;
static const uint8_t SCHEDULED_SLOT_HOUR[SCHEDULED_SLOT_COUNT] = {1, 4, 13, 16};
static const uint8_t SCHEDULED_SLOT_MINUTE = 16;
static const uint8_t SCHEDULED_SLOT_SECOND = 40;

static const uint32_t TX_WINDOW_SECONDS = 5UL * 60UL;
static const uint32_t WIFI_WAKE_LEAD_SECONDS = 2UL * 60UL;
static const uint32_t TX_START_TOLERANCE_SECONDS = 1;
static const uint32_t COLD_BOOT_BROADCAST_SECONDS = 10UL * 60UL;
static const uint32_t MAIN_LOOP_PERIOD_MS = 25;

// ----------------------- Carrier / Modulation -----------------------
static const uint32_t CARRIER_HZ_DEFAULT = 40000;
static const uint32_t CARRIER_HZ_MIN = 39500;
static const uint32_t CARRIER_HZ_MAX = 40500;
static const uint32_t CARRIER_HZ_STEP = 10;
static const float CARRIER_DUTY_TARGET = 45.0f;
static const mcpwm_unit_t MCPWM_UNIT = MCPWM_UNIT_0;
static const mcpwm_timer_t MCPWM_TIMER = MCPWM_TIMER_0;
static const uint32_t MCPWM_APB_CLK_HZ = 80000000;
static const uint32_t MCPWM_DEADTIME_NS = 150;
static const uint32_t CARRIER_RAMP_TOTAL_MS = 5;
static const uint8_t CARRIER_RAMP_STEP_COUNT = 5;
static const uint32_t CARRIER_RAMP_STEP_INTERVAL_MS = CARRIER_RAMP_TOTAL_MS / (CARRIER_RAMP_STEP_COUNT - 1);
static const float CARRIER_RAMP_DUTY_STEPS[CARRIER_RAMP_STEP_COUNT] = {
    0.0f,
    CARRIER_DUTY_TARGET * 0.25f,
    CARRIER_DUTY_TARGET * 0.50f,
    CARRIER_DUTY_TARGET * 0.75f,
    CARRIER_DUTY_TARGET};

static const uint16_t ON_MS_MARKER = 200;  // P
static const uint16_t ON_MS_BIT1 = 500;    // 1
static const uint16_t ON_MS_BIT0 = 800;    // 0

enum class JjySymbol : uint8_t {
  ZERO,
  ONE,
  MARKER,
};

static JjySymbol frameSymbols[60];
static int lastFrameMinute = -1;
static int lastFrameYDay = -1;
static int lastFrameYear = -1;

static int currentSecond = -1;
static bool carrierIsOn = false;
static bool symbolOnPhaseActive = false;
static uint32_t symbolOnStartedMs = 0;
static uint16_t activeOnDurationMs = 0;
static uint32_t lastResyncAttemptMs = 0;
static uint32_t currentCarrierHz = CARRIER_HZ_DEFAULT;
static bool rampUpActive = false;
static bool rampDownActive = false;
static bool rampDownStartedForSymbol = false;
static uint8_t rampStepIndex = 0;
static uint32_t rampStepStartedMs = 0;

static Preferences prefs;
static bool prefsReady = false;
static const char *NVS_NS = "jjy";
static const char *NVS_KEY_FREQ = "freq_hz";
static const char *NVS_KEY_WEB_OVERRIDE = "web_ovrd";
static const char *NVS_KEY_TZ = "tz_rule";

static String currentTzRule = "CET-1CEST,M3.5.0/2,M10.5.0/3";

static WebServer webServer(80);
static bool webServerReady = false;
static bool webOverrideEnabled = false;
static bool txWindowActive = false;
static time_t txWindowStartEpoch = 0;
static time_t txWindowEndEpoch = 0;
static int txWindowSlotIndex = -1;
static time_t lastMissedSlotEpoch = 0;
static bool coldBootStartupPending = false;
static bool coldBootBroadcastActive = false;
static bool coldBootUiHoldActive = false;
static bool coldBootUiHoldNoticePrinted = false;

static const BaseType_t JJY_TASK_CORE = 1;
static const BaseType_t NET_TASK_CORE = 0;
static const UBaseType_t JJY_TASK_PRIORITY = 4;
static const UBaseType_t NET_TASK_PRIORITY = 2;
static const uint32_t JJY_TASK_STACK_WORDS = 4096;
static const uint32_t NET_TASK_STACK_WORDS = 4096;
static const uint32_t JJY_TASK_PERIOD_MS = 1;
static const uint32_t NET_TASK_PERIOD_MS = 250;

static TaskHandle_t jjyTaskHandle = nullptr;
static TaskHandle_t netTaskHandle = nullptr;

static void connectWifi();
static bool initialTimeSync();
static void periodicResync();
static void setupCarrier();
static void carrierOn();
static void carrierOff();
static void setTxLed(bool on);
static uint16_t onDurationFor(JjySymbol symbol);
static char symbolChar(JjySymbol symbol);
static void buildJjyFrame(const tm &localNow);

static void setBit(int secondIndex, bool value);
static bool shouldRebuildFrame(const tm &localNow);
static int dayOfYear(const tm &localNow);
static void transmitSecond(const tm &localNow, int secIndex);
static uint32_t clampCarrierHz(uint32_t hz);

static bool isValidEpoch(time_t epoch);
static String formatEpochLocal(time_t epoch);
static time_t buildSlotEpoch(const tm &baseLocal, int slotIndex, int dayOffset);
static bool findNextSlotEpoch(time_t nowEpoch, time_t &slotEpochOut, int &slotIndexOut);
static bool findCurrentSlotEpoch(time_t nowEpoch, time_t &slotEpochOut, int &slotIndexOut);
static void startColdBootBroadcastWindow(time_t nowEpoch);
static void startTxWindow(time_t slotEpoch, int slotIndex);
static void stopTxWindow(const char *reason);
static bool enterDeepSleepUntilNextSlot(time_t nowEpoch, const char *trigger);
static void maybeEnterDeepSleep(time_t nowEpoch);
static void updateScheduleController();

static void setWebOverrideMode(bool enabled);
static void markUiSessionActive();
static void handleWebRoot();
static void handleWebStatus();
static void handleWebMode();
static void handleWebSave();
static void handleWebSleep();
static void setupWebServer();
static void runJjySchedulerTick();
static void jjyTask(void *arg);
static void networkTask(void *arg);

static void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.printf("[WiFi] Connecting to SSID: %s\n", DEV_DEFAULT_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(DEV_DEFAULT_SSID, DEV_DEFAULT_PASS);

  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[WiFi] Connection failed. Will retry in loop.");
  }
}

static bool initialTimeSync() {
  configTzTime(currentTzRule.c_str(), NTP1, NTP2);

  tm timeInfo;
  uint32_t startMs = millis();
  while ((millis() - startMs) < NTP_SYNC_TIMEOUT_MS) {
    if (getLocalTime(&timeInfo, 250)) {
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeInfo);
      Serial.printf("[NTP] Initial sync OK. Local time: %s\n", buf);
      lastResyncAttemptMs = millis();
      return true;
    }
  }

  Serial.println("[NTP] Initial sync timeout.");
  return false;
}

static void periodicResync() {
  if (lastResyncAttemptMs != 0 && (millis() - lastResyncAttemptMs) < NTP_RESYNC_INTERVAL_MS) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  Serial.println("[NTP] Periodic re-sync request...");
  configTzTime(currentTzRule.c_str(), NTP1, NTP2);
  lastResyncAttemptMs = millis();
}

static uint32_t clampCarrierHz(uint32_t hz) {
  if (hz < CARRIER_HZ_MIN) {
    return CARRIER_HZ_MIN;
  }
  if (hz > CARRIER_HZ_MAX) {
    return CARRIER_HZ_MAX;
  }
  return hz;
}

static void saveCarrierHzToNvs(uint32_t hz) {
  if (!prefsReady) {
    return;
  }
  prefs.putUInt(NVS_KEY_FREQ, hz);
}

static void reconfigureCarrierTimer(uint32_t hz) {
  const bool wasOn = carrierIsOn;
  if (wasOn) {
    carrierOff();
  }

  mcpwm_set_frequency(MCPWM_UNIT, MCPWM_TIMER, hz);

  if (wasOn) {
    carrierOn();
  }
}

static void setCarrierFrequency(uint32_t hz, bool persistToNvs) {
  const uint32_t clampedHz = clampCarrierHz(hz);
  if (clampedHz == currentCarrierHz) {
    if (hz != clampedHz) {
      Serial.printf("[PWM] Frequency clamp hit: %lu Hz (range %lu..%lu)\n", clampedHz, CARRIER_HZ_MIN, CARRIER_HZ_MAX);
    }
    return;
  }

  reconfigureCarrierTimer(clampedHz);
  currentCarrierHz = clampedHz;
  if (persistToNvs) {
    saveCarrierHzToNvs(currentCarrierHz);
  }
  Serial.printf("[PWM] Current Frequency: %lu Hz\n", currentCarrierHz);
}

static void handleSerialCommands() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    switch (c) {
      case '+':
        setCarrierFrequency(static_cast<uint32_t>(static_cast<int32_t>(currentCarrierHz) + static_cast<int32_t>(CARRIER_HZ_STEP)), true);
        break;
      case '-':
        {
          int32_t nextHz = static_cast<int32_t>(currentCarrierHz) - static_cast<int32_t>(CARRIER_HZ_STEP);
          if (nextHz < 0) {
            nextHz = 0;
          }
          setCarrierFrequency(static_cast<uint32_t>(nextHz), true);
        }
        break;
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

static uint32_t deadtimeNsToTicks(uint32_t ns) {
  uint64_t ticks = (static_cast<uint64_t>(ns) * static_cast<uint64_t>(MCPWM_APB_CLK_HZ) + 999999999ULL) / 1000000000ULL;
  if (ticks == 0) {
    ticks = 1;
  }
  return static_cast<uint32_t>(ticks);
}

static void configureDeadtime() {
  const uint32_t dtTicks = deadtimeNsToTicks(MCPWM_DEADTIME_NS);
  const esp_err_t err = mcpwm_deadtime_enable(MCPWM_UNIT,
                                               MCPWM_TIMER,
                                               MCPWM_ACTIVE_HIGH_MODE,
                                               dtTicks,
                                               dtTicks);
  if (err == ESP_OK) {
    Serial.printf("[MCPWM] Dead-time enabled: %lu ns (~%lu ticks)\n", MCPWM_DEADTIME_NS, dtTicks);
  } else {
    Serial.printf("[MCPWM] Dead-time enable failed: err=%d\n", static_cast<int>(err));
  }
}

static void setTxLed(bool on) {
  digitalWrite(PIN_TX_LED, (on == TX_LED_ACTIVE_HIGH) ? HIGH : LOW);
}

static void setupCarrier() {
  mcpwm_gpio_init(MCPWM_UNIT, MCPWM0A, PIN_IA);
  mcpwm_gpio_init(MCPWM_UNIT, MCPWM0B, PIN_IB);

  mcpwm_config_t pwmCfg = {};
  pwmCfg.frequency = currentCarrierHz;
  pwmCfg.cmpr_a = 0.0f;
  pwmCfg.cmpr_b = 0.0f;
  pwmCfg.counter_mode = MCPWM_UP_COUNTER;
  pwmCfg.duty_mode = MCPWM_DUTY_MODE_0;
  mcpwm_init(MCPWM_UNIT, MCPWM_TIMER, &pwmCfg);
  configureDeadtime();

  carrierOff();
  Serial.printf("[MCPWM] IA=GPIO%d, IB=GPIO%d, f=%lu Hz, anti-phase enabled\n", PIN_IA, PIN_IB, currentCarrierHz);
}

static void applyCarrierDuty(float dutyPercent) {
  float clampedDuty = dutyPercent;
  if (clampedDuty < 0.0f) {
    clampedDuty = 0.0f;
  }
  if (clampedDuty > CARRIER_DUTY_TARGET) {
    clampedDuty = CARRIER_DUTY_TARGET;
  }

  if (clampedDuty <= 0.0f) {
    mcpwm_set_duty_type(MCPWM_UNIT, MCPWM_TIMER, MCPWM_OPR_A, MCPWM_DUTY_MODE_0);
    mcpwm_set_duty_type(MCPWM_UNIT, MCPWM_TIMER, MCPWM_OPR_B, MCPWM_DUTY_MODE_0);
    mcpwm_set_duty(MCPWM_UNIT, MCPWM_TIMER, MCPWM_OPR_A, 0.0f);
    mcpwm_set_duty(MCPWM_UNIT, MCPWM_TIMER, MCPWM_OPR_B, 0.0f);
    carrierIsOn = false;
    setTxLed(false);
    return;
  }

  mcpwm_set_duty_type(MCPWM_UNIT, MCPWM_TIMER, MCPWM_OPR_A, MCPWM_DUTY_MODE_0);
  mcpwm_set_duty_type(MCPWM_UNIT, MCPWM_TIMER, MCPWM_OPR_B, MCPWM_DUTY_MODE_1);
  mcpwm_set_duty(MCPWM_UNIT, MCPWM_TIMER, MCPWM_OPR_A, clampedDuty);
  mcpwm_set_duty(MCPWM_UNIT, MCPWM_TIMER, MCPWM_OPR_B, clampedDuty);
  carrierIsOn = true;
  setTxLed(true);
}

static void beginCarrierRampUp() {
  rampUpActive = true;
  rampDownActive = false;
  rampStepIndex = 0;
  rampStepStartedMs = millis();
  applyCarrierDuty(CARRIER_RAMP_DUTY_STEPS[rampStepIndex]);
}

static void beginCarrierRampDown() {
  rampDownActive = true;
  rampUpActive = false;
  rampDownStartedForSymbol = true;
  rampStepIndex = CARRIER_RAMP_STEP_COUNT - 1;
  rampStepStartedMs = millis();
  applyCarrierDuty(CARRIER_RAMP_DUTY_STEPS[rampStepIndex]);
}

static void updateCarrierRamp() {
  if (rampUpActive) {
    while ((millis() - rampStepStartedMs) >= CARRIER_RAMP_STEP_INTERVAL_MS && rampStepIndex < (CARRIER_RAMP_STEP_COUNT - 1)) {
      ++rampStepIndex;
      rampStepStartedMs += CARRIER_RAMP_STEP_INTERVAL_MS;
      applyCarrierDuty(CARRIER_RAMP_DUTY_STEPS[rampStepIndex]);
    }
    if (rampStepIndex >= (CARRIER_RAMP_STEP_COUNT - 1)) {
      rampUpActive = false;
    }
  }

  if (rampDownActive) {
    while ((millis() - rampStepStartedMs) >= CARRIER_RAMP_STEP_INTERVAL_MS && rampStepIndex > 0) {
      --rampStepIndex;
      rampStepStartedMs += CARRIER_RAMP_STEP_INTERVAL_MS;
      applyCarrierDuty(CARRIER_RAMP_DUTY_STEPS[rampStepIndex]);
    }
    if (rampStepIndex == 0) {
      rampDownActive = false;
    }
  }
}

static void carrierOn() {
  if (carrierIsOn) {
    return;
  }
  rampUpActive = false;
  rampDownActive = false;
  applyCarrierDuty(CARRIER_DUTY_TARGET);
}

static void carrierOff() {
  rampUpActive = false;
  rampDownActive = false;
  applyCarrierDuty(0.0f);
}

static uint16_t onDurationFor(JjySymbol symbol) {
  switch (symbol) {
    case JjySymbol::MARKER:
      return ON_MS_MARKER;
    case JjySymbol::ONE:
      return ON_MS_BIT1;
    default:
      return ON_MS_BIT0;
  }
}

static char symbolChar(JjySymbol symbol) {
  switch (symbol) {
    case JjySymbol::MARKER:
      return 'P';
    case JjySymbol::ONE:
      return '1';
    default:
      return '0';
  }
}

static void setBit(int secondIndex, bool value) {
  if (secondIndex < 0 || secondIndex > 59) {
    return;
  }
  frameSymbols[secondIndex] = value ? JjySymbol::ONE : JjySymbol::ZERO;
}

static int dayOfYear(const tm &localNow) {
  // tm_yday is 0..365, JJY day-of-year uses 1..366.
  return localNow.tm_yday + 1;
}

static void buildJjyFrame(const tm &localNow) {
  // Initialize whole frame as 0 bits.
  for (int i = 0; i < 60; ++i) {
    frameSymbols[i] = JjySymbol::ZERO;
  }

  // Position markers.
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
  const int wday = localNow.tm_wday;  // 0..6 (Sunday=0)

  // Minute BCD: tens(40,20,10) + ones(8,4,2,1)
  setBit(1, (minute / 10) & 0x4);
  setBit(2, (minute / 10) & 0x2);
  setBit(3, (minute / 10) & 0x1);
  setBit(5, (minute % 10) & 0x8);
  setBit(6, (minute % 10) & 0x4);
  setBit(7, (minute % 10) & 0x2);
  setBit(8, (minute % 10) & 0x1);

  // Hour BCD: 10,11 fixed 0; tens(20,10) at 12,13; 14 fixed 0; ones(8,4,2,1) at 15..18
  setBit(12, (hour / 10) & 0x2);
  setBit(13, (hour / 10) & 0x1);
  setBit(15, (hour % 10) & 0x8);
  setBit(16, (hour % 10) & 0x4);
  setBit(17, (hour % 10) & 0x2);
  setBit(18, (hour % 10) & 0x1);

  // Day of year BCD(001..366): 20,21 fixed 0; hundreds(200,100) at 22,23; 24 fixed 0;
  // tens(80,40,20,10) at 25..28; ones(8,4,2,1) at 30..33
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

  // Parity bits (PA1/PA2) from encoded BCD fields, matching NICT/pico_jjy_tx mapping.
  const uint8_t pa1 =
      ((hour / 10) & 0x2 ? 1 : 0) +
      ((hour / 10) & 0x1 ? 1 : 0) +
      ((hour % 10) & 0x8 ? 1 : 0) +
      ((hour % 10) & 0x4 ? 1 : 0) +
      ((hour % 10) & 0x2 ? 1 : 0) +
      ((hour % 10) & 0x1 ? 1 : 0);
  const uint8_t pa2 =
      ((minute / 10) & 0x4 ? 1 : 0) +
      ((minute / 10) & 0x2 ? 1 : 0) +
      ((minute / 10) & 0x1 ? 1 : 0) +
      ((minute % 10) & 0x8 ? 1 : 0) +
      ((minute % 10) & 0x4 ? 1 : 0) +
      ((minute % 10) & 0x2 ? 1 : 0) +
      ((minute % 10) & 0x1 ? 1 : 0);
  setBit(36, (pa1 & 0x1) != 0);
  setBit(37, (pa2 & 0x1) != 0);

  // Year (last 2 digits) BCD: 40 fixed 0; tens at 41..44; ones at 45..48.
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

  // Weekday (0..6) in 3 bits.
  setBit(50, wday & 0x4);
  setBit(51, wday & 0x2);
  setBit(52, wday & 0x1);

  lastFrameMinute = localNow.tm_min;
  lastFrameYDay = localNow.tm_yday;
  lastFrameYear = localNow.tm_year;
}

static bool shouldRebuildFrame(const tm &localNow) {
  return lastFrameMinute != localNow.tm_min || lastFrameYDay != localNow.tm_yday || lastFrameYear != localNow.tm_year;
}

static void transmitSecond(const tm &localNow, int secIndex) {
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

static bool getCurrentLocalTime(tm &localNow) {
  const time_t nowEpoch = time(nullptr);
  if (nowEpoch < 100000) {
    return false;
  }
  localtime_r(&nowEpoch, &localNow);
  return true;
}

static bool isValidEpoch(time_t epoch) {
  return epoch >= 100000;
}

static String formatEpochLocal(time_t epoch) {
  if (!isValidEpoch(epoch)) {
    return String("n/a");
  }

  tm localNow;
  localtime_r(&epoch, &localNow);

  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &localNow);
  return String(buf);
}

static time_t buildSlotEpoch(const tm &baseLocal, int slotIndex, int dayOffset) {
  tm slotTm = baseLocal;
  slotTm.tm_mday += dayOffset;
  slotTm.tm_hour = SCHEDULED_SLOT_HOUR[slotIndex];
  slotTm.tm_min = SCHEDULED_SLOT_MINUTE;
  slotTm.tm_sec = SCHEDULED_SLOT_SECOND;
  slotTm.tm_isdst = -1;
  return mktime(&slotTm);
}

static bool findNextSlotEpoch(time_t nowEpoch, time_t &slotEpochOut, int &slotIndexOut) {
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

static bool findCurrentSlotEpoch(time_t nowEpoch, time_t &slotEpochOut, int &slotIndexOut) {
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

static void startColdBootBroadcastWindow(time_t nowEpoch) {
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

static void startTxWindow(time_t slotEpoch, int slotIndex) {
  txWindowActive = true;
  txWindowStartEpoch = slotEpoch;
  txWindowEndEpoch = slotEpoch + static_cast<time_t>(TX_WINDOW_SECONDS);
  txWindowSlotIndex = slotIndex;
  currentSecond = -1;
  symbolOnPhaseActive = false;
  carrierOff();

  Serial.printf("[SCHED] TX window START idx=%d at %s, end=%s\n",
                txWindowSlotIndex,
                formatEpochLocal(txWindowStartEpoch).c_str(),
                formatEpochLocal(txWindowEndEpoch).c_str());
}

static void stopTxWindow(const char *reason) {
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

static bool enterDeepSleepUntilNextSlot(time_t nowEpoch, const char *trigger) {
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

static void maybeEnterDeepSleep(time_t nowEpoch) {
  if (webOverrideEnabled || txWindowActive || coldBootStartupPending) {
    return;
  }

  if (!isValidEpoch(nowEpoch)) {
    return;
  }

  if (coldBootUiHoldActive) {
    if (!coldBootUiHoldNoticePrinted) {
      Serial.println("[BOOT] Deep sleep paused by UI activity during cold-boot broadcast. Press /save to continue.");
      coldBootUiHoldNoticePrinted = true;
    }
    return;
  }

  coldBootUiHoldNoticePrinted = false;
  (void)enterDeepSleepUntilNextSlot(nowEpoch, "AUTO_SCHEDULE");
}

static void updateScheduleController() {
  const time_t nowEpoch = time(nullptr);
  if (!isValidEpoch(nowEpoch)) {
    return;
  }

  if (coldBootStartupPending && !txWindowActive) {
    startColdBootBroadcastWindow(nowEpoch);
  }

  if (txWindowActive && nowEpoch >= txWindowEndEpoch) {
    stopTxWindow("window complete");
  }

  if (!txWindowActive) {
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

  maybeEnterDeepSleep(nowEpoch);
}

static void setWebOverrideMode(bool enabled) {
  if (webOverrideEnabled == enabled) {
    return;
  }

  webOverrideEnabled = enabled;
  if (prefsReady) {
    prefs.putBool(NVS_KEY_WEB_OVERRIDE, webOverrideEnabled);
  }

  Serial.printf("[WEB] Override mode %s\n", webOverrideEnabled ? "ENABLED (deep sleep paused)" : "DISABLED (auto schedule)");
}

static void markUiSessionActive() {
  if (!coldBootBroadcastActive) {
    return;
  }

  if (!coldBootUiHoldActive) {
    coldBootUiHoldActive = true;
    coldBootUiHoldNoticePrinted = false;
    Serial.println("[BOOT] Web UI opened during cold-boot broadcast. Deep sleep paused until /save.");
  }
}

static void handleWebRoot() {
  markUiSessionActive();

  String page;
  page.reserve(4000);

  page += R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
  <title>JJY Scheduler</title>
  <script src="https://cdn.tailwindcss.com"></script>
  <script>function updateTzVisibility(){var s=document.getElementById('tz_sel'),c=document.getElementById('tz_custom_div');c.style.display=(s.value==='custom')?'block':'none';}</script>
</head>
<body class="bg-slate-50 text-slate-800 font-sans min-h-screen p-4 sm:p-6 lg:p-8 text-[16px]">
  <div class="max-w-3xl mx-auto space-y-5 sm:space-y-6">
    <header class="bg-white rounded-2xl shadow-sm border border-slate-100 p-5 sm:p-6 flex flex-col sm:flex-row sm:items-center sm:justify-between gap-3 sm:gap-4">
      <h1 class="text-xl sm:text-2xl font-bold text-slate-900 tracking-tight">JJY Scheduler</h1>
      <span class="inline-flex items-center px-3 py-1 rounded-full text-xs sm:text-sm font-semibold tracking-wide uppercase )HTML";
  
  page += webOverrideEnabled ? "bg-amber-100 text-amber-800\">WEB_OVERRIDE" : "bg-emerald-100 text-emerald-800\">AUTO_SCHEDULE";
  
  page += R"HTML(</span>
    </header>
)HTML";

  if (coldBootBroadcastActive) {
    page += "<div class=\"bg-amber-50 border-l-4 border-amber-400 p-4 rounded-xl shadow-sm\"><p class=\"text-sm text-amber-800\"><strong>Cold boot broadcast active.</strong><br class=\"sm:hidden\"> ";
    page += coldBootUiHoldActive ? "Deep sleep paused until SAVE." : "Opening this paused deep sleep.";
    page += "</p></div>\n";
  } else if (coldBootUiHoldActive) {
    page += "<div class=\"bg-blue-50 border-l-4 border-blue-400 p-4 rounded-xl shadow-sm\"><p class=\"text-sm text-blue-800\"><strong>Deep sleep paused:</strong><br class=\"sm:hidden\"> press SAVE to resume.</p></div>\n";
  }

  page += R"HTML(
    <div class="grid grid-cols-1 md:grid-cols-2 gap-5 sm:gap-6">
      <section class="bg-white rounded-2xl shadow-sm border border-slate-100 p-5 sm:p-6">
        <h2 class="text-base sm:text-lg font-semibold text-slate-800 mb-4 sm:mb-5 text-center sm:text-left">Quick Actions</h2>
        <div class="space-y-3 sm:space-y-4">
          <a href="/mode?value=auto" class="block w-full text-center px-4 py-3.5 sm:py-2.5 rounded-xl sm:rounded-lg text-sm sm:text-base font-medium text-white bg-indigo-600 hover:bg-indigo-700 active:bg-indigo-800 transition-colors shadow-sm">Set AUTO_SCHEDULE</a>
          <a href="/mode?value=override" class="block w-full text-center px-4 py-3.5 sm:py-2.5 rounded-xl sm:rounded-lg text-sm sm:text-base font-medium text-indigo-700 bg-indigo-50 hover:bg-indigo-100 active:bg-indigo-200 transition-colors">Set WEB_OVERRIDE</a>
          <div class="pt-2">
            <a href="/save" class="block w-full text-center px-4 py-3.5 sm:py-2.5 rounded-xl sm:rounded-lg text-sm sm:text-base font-medium text-white bg-emerald-600 hover:bg-emerald-700 active:bg-emerald-800 transition-colors shadow-sm">Save & Resume Auto Sleep</a>
          </div>
          <a href="/sleep" class="block w-full text-center px-4 py-3.5 sm:py-2.5 rounded-xl sm:rounded-lg text-sm sm:text-base font-medium text-slate-700 bg-white border border-slate-200 hover:bg-slate-50 active:bg-slate-100 transition-colors shadow-sm">Sleep Now</a>
          <a href="/status" class="block w-full text-center px-4 py-3 sm:py-2.5 rounded-xl sm:rounded-lg text-sm sm:text-base font-medium text-slate-500 hover:text-slate-900 transition-colors">View Status JSON</a>
        </div>
      </section>

      <section class="bg-white rounded-2xl shadow-sm border border-slate-100 p-5 sm:p-6">
        <h2 class="text-base sm:text-lg font-semibold text-slate-800 mb-4 sm:mb-5">Timezone Settings</h2>
        <div class="mb-5 sm:mb-6 text-sm text-slate-600">
          <span class="block mb-1 text-slate-400 font-medium uppercase text-[10px] tracking-wider">Active rule</span>
          <code class="block w-full p-3 bg-slate-50 rounded-xl sm:rounded-lg border border-slate-200 text-xs font-mono text-slate-600 break-all leading-relaxed">)HTML";
          
  page += currentTzRule;
  
  page += R"HTML(</code>
        </div>
        <form action="/set_tz" method="GET" class="space-y-4 sm:space-y-5">
          <div>
            <label for="tz_sel" class="block text-sm font-medium text-slate-700 mb-1.5">Select Timezone</label>
            <div class="relative">
              <select id="tz_sel" name="tz" onchange="updateTzVisibility()" class="block w-full px-4 py-3.5 sm:py-2.5 border border-slate-300 rounded-xl sm:rounded-lg shadow-sm focus:ring-2 focus:ring-indigo-500 focus:border-indigo-500 text-sm sm:text-base bg-white appearance-none">
                <option value="JST-9">Japan (JST-9)</option>
                <option value="UTC0">UTC (UTC0)</option>
                <option value="CET-1CEST,M3.5.0/2,M10.5.0/3">Warsaw (CET/CEST)</option>
                <option value="custom">Custom...</option>
              </select>
              <div class="pointer-events-none absolute inset-y-0 right-0 flex items-center px-3 text-slate-400">
                <svg class="h-4 w-4 fill-current" viewBox="0 0 20 20"><path d="M5.293 7.293a1 1 0 011.414 0L10 10.586l3.293-3.293a1 1 0 111.414 1.414l-4 4a1 1 0 01-1.414 0l-4-4a1 1 0 010-1.414z"/></svg>
              </div>
            </div>
          </div>
          <div id="tz_custom_div" style="display:none;">
            <label for="tz_custom" class="block text-sm font-medium text-slate-700 mb-1.5">POSIX Rule</label>
            <input type="text" id="tz_custom" name="tz_custom" placeholder="e.g. JST-9" class="block w-full px-4 py-3.5 sm:py-2.5 border border-slate-300 rounded-xl sm:rounded-lg shadow-sm focus:ring-2 focus:ring-indigo-500 focus:border-indigo-500 text-sm sm:text-base bg-white">
          </div>
          <button type="submit" onclick="var s=document.getElementById('tz_sel');if(s.value==='custom'){s.value=document.getElementById('tz_custom').value;}" class="w-full flex justify-center py-3.5 sm:py-2.5 px-4 rounded-xl sm:rounded-lg shadow-sm text-sm sm:text-base font-medium text-white bg-blue-600 hover:bg-blue-700 active:bg-blue-800 transition-colors mt-2">Apply Timezone</button>
        </form>
      </section>
    </div>

    <footer class="bg-transparent px-2 text-center text-[10px] sm:text-xs text-slate-400 font-medium uppercase tracking-widest">
      Daily windows: 01:16, 04:16, 13:16, 16:16
    </footer>
  </div>
</body>
</html>)HTML";

  webServer.send(200, "text/html", page);
}

static void handleWebStatus() {
  markUiSessionActive();

  const time_t nowEpoch = time(nullptr);

  time_t nextSlotEpoch = 0;
  int nextSlotIndex = -1;
  const bool haveNextSlot = findNextSlotEpoch(nowEpoch, nextSlotEpoch, nextSlotIndex);
  const time_t nextWakeEpoch = haveNextSlot ? (nextSlotEpoch - static_cast<time_t>(WIFI_WAKE_LEAD_SECONDS)) : 0;

  uint32_t coldBootBroadcastRemainingSec = 0;
  if (coldBootBroadcastActive && isValidEpoch(nowEpoch) && txWindowEndEpoch > nowEpoch) {
    coldBootBroadcastRemainingSec = static_cast<uint32_t>(txWindowEndEpoch - nowEpoch);
  }

  String json = "{";
  json += "\"mode\":\"";
  json += webOverrideEnabled ? "WEB_OVERRIDE" : "AUTO_SCHEDULE";
  json += "\",";
  json += "\"time\":\"" + formatEpochLocal(nowEpoch) + "\",";
  json += "\"tz_rule\":\"" + currentTzRule + "\",";
  json += "\"next_slot\":\"" + (haveNextSlot ? formatEpochLocal(nextSlotEpoch) : String("n/a")) + "\",";
  json += "\"next_wake\":\"" + (haveNextSlot ? formatEpochLocal(nextWakeEpoch) : String("n/a")) + "\",";
  json += "\"next_slot_index\":" + String(nextSlotIndex) + ",";
  json += "\"tx_active\":" + String(txWindowActive ? "true" : "false") + ",";
  json += "\"cold_boot_startup_pending\":" + String(coldBootStartupPending ? "true" : "false") + ",";
  json += "\"cold_boot_broadcast_active\":" + String(coldBootBroadcastActive ? "true" : "false") + ",";
  json += "\"cold_boot_broadcast_remaining_sec\":" + String(coldBootBroadcastRemainingSec) + ",";
  json += "\"cold_boot_ui_hold\":" + String(coldBootUiHoldActive ? "true" : "false");
  json += "}";

  webServer.send(200, "application/json", json);
}

static void handleWebSetTz() {
  markUiSessionActive();

  if (!webServer.hasArg("tz")) {
    webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing tz parameter\"}");
    return;
  }

  const String newTz = webServer.arg("tz");
  if (newTz.length() == 0) {
    webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"empty tz parameter\"}");
    return;
  }

  currentTzRule = newTz;
  if (prefsReady) {
    prefs.putString(NVS_KEY_TZ, currentTzRule);
  }

  Serial.printf("[WEB] Timezone updated to: %s\n", currentTzRule.c_str());

  // Apply immediately
  configTzTime(currentTzRule.c_str(), NTP1, NTP2);

  if (txWindowActive) {
    stopTxWindow("timezone changed");
  }

  // Redirect back to root
  webServer.sendHeader("Location", "/", true);
  webServer.send(302, "text/plain", "");
}

static void handleWebMode() {
  markUiSessionActive();

  if (!webServer.hasArg("value")) {
    webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing value\"}");
    return;
  }

  String value = webServer.arg("value");
  value.toLowerCase();

  if (value == "override" || value == "1" || value == "on") {
    setWebOverrideMode(true);
  } else if (value == "auto" || value == "0" || value == "off") {
    setWebOverrideMode(false);
  } else {
    webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"value must be auto|override\"}");
    return;
  }

  webServer.send(200, "application/json", String("{\"ok\":true,\"mode\":\"") + (webOverrideEnabled ? "WEB_OVERRIDE" : "AUTO_SCHEDULE") + "\"}");
}

static void handleWebSave() {
  const bool hadUiHold = coldBootUiHoldActive;
  coldBootUiHoldActive = false;

  coldBootUiHoldNoticePrinted = false;

  if (hadUiHold) {
    Serial.println("[WEB] Save pressed. Cold-boot UI hold cleared; auto deep sleep may resume.");
  }

  webServer.send(200,
                 "application/json",
                 String("{\"ok\":true,\"mode\":\"") + (webOverrideEnabled ? "WEB_OVERRIDE" : "AUTO_SCHEDULE") +
                     "\",\"cold_boot_ui_hold\":false}");
}

static void handleWebSleep() {
  markUiSessionActive();

  webServer.send(200, "application/json", "{\"ok\":true,\"sleep_request\":\"accepted\"}");
  delay(25);

  if (txWindowActive) {
    stopTxWindow("web sleep request");
  }
  coldBootStartupPending = false;
  coldBootBroadcastActive = false;
  coldBootUiHoldActive = false;
  coldBootUiHoldNoticePrinted = false;

  const time_t nowEpoch = time(nullptr);
  if (!enterDeepSleepUntilNextSlot(nowEpoch, "WEB_REQUEST")) {
    Serial.println("[WEB] Sleep request could not enter deep sleep.");
  }
}

static void setupWebServer() {
  webServer.on("/", HTTP_GET, handleWebRoot);
  webServer.on("/status", HTTP_GET, handleWebStatus);
  webServer.on("/mode", HTTP_GET, handleWebMode);
  webServer.on("/save", HTTP_GET, handleWebSave);
  webServer.on("/sleep", HTTP_GET, handleWebSleep);
  webServer.on("/set_tz", HTTP_GET, handleWebSetTz);

  webServer.begin();
  webServerReady = true;
  Serial.println("[WEB] Server started (HTTP :80)");
}

static void runJjySchedulerTick() {
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
    Serial.printf("[FRAME] rebuilt for %04d-%03d %02d:%02d (Warsaw local)\n",
                  nowLocal.tm_year + 1900,
                  dayOfYear(nowLocal),
                  nowLocal.tm_hour,
                  nowLocal.tm_min);
  }

  // Start a new symbol exactly once per wall-clock second.
  if (nowLocal.tm_sec != currentSecond) {
    currentSecond = nowLocal.tm_sec;
    transmitSecond(nowLocal, currentSecond);
  }

  // Apply down-ramp near end of ON phase, then stop carrier exactly on symbol boundary.
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

static void jjyTask(void *arg) {
  (void)arg;
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    runJjySchedulerTick();
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(JJY_TASK_PERIOD_MS));
  }
}

static void networkTask(void *arg) {
  (void)arg;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      connectWifi();
    }
    periodicResync();
    vTaskDelay(pdMS_TO_TICKS(NET_TASK_PERIOD_MS));
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nJJY 40kHz simulator starting...");

  const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  const bool isColdBoot = wakeCause != ESP_SLEEP_WAKEUP_TIMER;
  Serial.printf("[BOOT] Wake cause: %d\n", static_cast<int>(wakeCause));

  coldBootStartupPending = isColdBoot;
  coldBootBroadcastActive = false;
  coldBootUiHoldActive = false;
  coldBootUiHoldNoticePrinted = false;

  pinMode(PIN_TX_LED, OUTPUT);
  setTxLed(false);
  Serial.printf("[IO] TX LED GPIO%d\n", PIN_TX_LED);

  prefsReady = prefs.begin(NVS_NS, false);
  if (!prefsReady) {
    Serial.println("[NVS] Preferences begin failed. Using default settings.");
  } else {
    const uint32_t savedHz = prefs.getUInt(NVS_KEY_FREQ, CARRIER_HZ_DEFAULT);
    currentCarrierHz = clampCarrierHz(savedHz);
    webOverrideEnabled = prefs.getBool(NVS_KEY_WEB_OVERRIDE, false);
    currentTzRule = prefs.getString(NVS_KEY_TZ, currentTzRule);

    if (savedHz != currentCarrierHz) {
      saveCarrierHzToNvs(currentCarrierHz);
    }
    Serial.printf("[NVS] Loaded carrier frequency: %lu Hz\n", currentCarrierHz);
    Serial.printf("[NVS] Loaded web override: %s\n", webOverrideEnabled ? "ON" : "OFF");
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
    const time_t nowEpoch = time(nullptr);
    if (isValidEpoch(nowEpoch)) {
      startColdBootBroadcastWindow(nowEpoch);
    } else {
      Serial.println("[BOOT] Cold-boot broadcast pending until time becomes valid.");
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

void loop() {
  if (webServerReady && WiFi.status() == WL_CONNECTED) {
    webServer.handleClient();
  }

  updateScheduleController();
  delay(MAIN_LOOP_PERIOD_MS);
}