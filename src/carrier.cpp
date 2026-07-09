#include "carrier.h"

#include <Preferences.h>
#include <driver/mcpwm.h>

#include "shared_state.h"

static const mcpwm_unit_t kMcpwmUnit = MCPWM_UNIT_0;
static const mcpwm_timer_t kMcpwmTimer = MCPWM_TIMER_0;

uint32_t clampCarrierHz(uint32_t hz)
{
    if (hz < CARRIER_HZ_MIN) {
        return CARRIER_HZ_MIN;
    }
    if (hz > CARRIER_HZ_MAX) {
        return CARRIER_HZ_MAX;
    }
    return hz;
}

void saveCarrierHzToNvs(uint32_t hz)
{
    if (!prefsReady) {
        return;
    }
    prefs.putUInt(NVS_KEY_FREQ, hz);
}

void reconfigureCarrierTimer(uint32_t hz)
{
    const bool wasOn = carrierIsOn;
    if (wasOn) {
        carrierOff();
    }

    mcpwm_set_frequency(kMcpwmUnit, kMcpwmTimer, hz);

    if (wasOn) {
        carrierOn();
    }
}

void setCarrierFrequency(uint32_t hz, bool persistToNvs)
{
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

static uint32_t deadtimeNsToTicks(uint32_t ns)
{
    uint64_t ticks = (static_cast<uint64_t>(ns) * static_cast<uint64_t>(MCPWM_APB_CLK_HZ) + 999999999ULL) / 1000000000ULL;
    if (ticks == 0) {
        ticks = 1;
    }
    return static_cast<uint32_t>(ticks);
}

static void configureDeadtime()
{
    const uint32_t dtTicks = deadtimeNsToTicks(MCPWM_DEADTIME_NS);
    const esp_err_t err = mcpwm_deadtime_enable(kMcpwmUnit,
        kMcpwmTimer,
        MCPWM_ACTIVE_HIGH_MODE,
        dtTicks,
        dtTicks);
    if (err == ESP_OK) {
        Serial.printf("[MCPWM] Dead-time enabled: %lu ns (~%lu ticks)\n", MCPWM_DEADTIME_NS, dtTicks);
    } else {
        Serial.printf("[MCPWM] Dead-time enable failed: err=%d\n", static_cast<int>(err));
    }
}

void setTxLed(bool on)
{
    digitalWrite(PIN_TX_LED, (on == TX_LED_ACTIVE_HIGH) ? HIGH : LOW);
}

void setupCarrier()
{
    mcpwm_gpio_init(kMcpwmUnit, MCPWM0A, PIN_IA);
    mcpwm_gpio_init(kMcpwmUnit, MCPWM0B, PIN_IB);

    mcpwm_config_t pwmCfg = { };
    pwmCfg.frequency = currentCarrierHz;
    pwmCfg.cmpr_a = 0.0f;
    pwmCfg.cmpr_b = 0.0f;
    pwmCfg.counter_mode = MCPWM_UP_COUNTER;
    pwmCfg.duty_mode = MCPWM_DUTY_MODE_0;
    mcpwm_init(kMcpwmUnit, kMcpwmTimer, &pwmCfg);
    configureDeadtime();

    carrierOff();
    Serial.printf("[MCPWM] IA=GPIO%d, IB=GPIO%d, f=%lu Hz, anti-phase enabled\n", PIN_IA, PIN_IB, currentCarrierHz);
}

void applyCarrierDuty(float dutyPercent)
{
    float clampedDuty = dutyPercent;
    if (clampedDuty < 0.0f) {
        clampedDuty = 0.0f;
    }
    if (clampedDuty > CARRIER_DUTY_TARGET) {
        clampedDuty = CARRIER_DUTY_TARGET;
    }

    if (clampedDuty <= 0.0f) {
        mcpwm_set_duty_type(kMcpwmUnit, kMcpwmTimer, MCPWM_OPR_A, MCPWM_DUTY_MODE_0);
        mcpwm_set_duty_type(kMcpwmUnit, kMcpwmTimer, MCPWM_OPR_B, MCPWM_DUTY_MODE_0);
        mcpwm_set_duty(kMcpwmUnit, kMcpwmTimer, MCPWM_OPR_A, 0.0f);
        mcpwm_set_duty(kMcpwmUnit, kMcpwmTimer, MCPWM_OPR_B, 0.0f);
        carrierIsOn = false;
        setTxLed(false);
        return;
    }

    mcpwm_set_duty_type(kMcpwmUnit, kMcpwmTimer, MCPWM_OPR_A, MCPWM_DUTY_MODE_0);
    mcpwm_set_duty_type(kMcpwmUnit, kMcpwmTimer, MCPWM_OPR_B, MCPWM_DUTY_MODE_1);
    mcpwm_set_duty(kMcpwmUnit, kMcpwmTimer, MCPWM_OPR_A, clampedDuty);
    mcpwm_set_duty(kMcpwmUnit, kMcpwmTimer, MCPWM_OPR_B, clampedDuty);
    carrierIsOn = true;
    setTxLed(true);
}

void beginCarrierRampUp()
{
    rampUpActive = true;
    rampDownActive = false;
    rampStepIndex = 0;
    rampStepStartedMs = millis();
    applyCarrierDuty(CARRIER_RAMP_DUTY_STEPS[rampStepIndex]);
}

void beginCarrierRampDown()
{
    rampDownActive = true;
    rampUpActive = false;
    rampDownStartedForSymbol = true;
    rampStepIndex = CARRIER_RAMP_STEP_COUNT - 1;
    rampStepStartedMs = millis();
    applyCarrierDuty(CARRIER_RAMP_DUTY_STEPS[rampStepIndex]);
}

void updateCarrierRamp()
{
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

void carrierOn()
{
    if (carrierIsOn) {
        return;
    }
    rampUpActive = false;
    rampDownActive = false;
    applyCarrierDuty(CARRIER_DUTY_TARGET);
}

void carrierOff()
{
    rampUpActive = false;
    rampDownActive = false;
    applyCarrierDuty(0.0f);
}
