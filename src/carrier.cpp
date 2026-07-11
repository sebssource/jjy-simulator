#include "carrier.h"

#include <Preferences.h>
#include <driver/mcpwm_prelude.h>

#include "shared_state.h"

static mcpwm_timer_handle_t s_timer = nullptr;
static mcpwm_oper_handle_t s_oper = nullptr;
static mcpwm_cmpr_handle_t s_cmpr = nullptr;
static mcpwm_gen_handle_t s_genA = nullptr;  // GPIO PIN_IA
static mcpwm_gen_handle_t s_genB = nullptr;  // GPIO PIN_IB

static uint32_t timerPeriodTicks()
{
    return MCPWM_APB_CLK_HZ / currentCarrierHz;
}

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

// Recompute the comparator value for a given duty percentage (0..100) and
// apply it. Duty is expressed as a percentage of the timer period.
static void applyComparatorForDuty(float dutyPercent)
{
    const uint32_t period = timerPeriodTicks();
    uint32_t cmp;
    if (dutyPercent <= 0.0f) {
        cmp = period;  // compare never reached -> output stays low (off)
    } else {
        cmp = static_cast<uint32_t>(period * (dutyPercent / 100.0f));
        if (cmp == 0) {
            cmp = 1;
        }
        if (cmp >= period) {
            cmp = period - 1;
        }
    }
    mcpwm_comparator_set_compare_value(s_cmpr, cmp);
}

void reconfigureCarrierTimer(uint32_t hz)
{
    const bool wasOn = carrierIsOn;
    const mcpwm_timer_start_stop_cmd_t stopCmd = MCPWM_TIMER_STOP_FULL;
    const mcpwm_timer_start_stop_cmd_t startCmd = MCPWM_TIMER_START_NO_STOP;

    if (wasOn) {
        mcpwm_timer_start_stop(s_timer, stopCmd);
    }

    mcpwm_timer_set_period(s_timer, timerPeriodTicks());

    // Re-apply current duty (or off) at the new frequency.
    if (wasOn) {
        applyComparatorForDuty(CARRIER_DUTY_TARGET);
        mcpwm_timer_start_stop(s_timer, startCmd);
    } else {
        applyComparatorForDuty(0.0f);
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
    const mcpwm_dead_time_config_t dtCfg = {
        .posedge_delay_ticks = dtTicks,
        .negedge_delay_ticks = dtTicks,
        .flags = {
            .invert_output = 0,
        },
    };
    const esp_err_t err = mcpwm_generator_set_dead_time(s_genA, s_genB, &dtCfg);
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
    const mcpwm_timer_config_t timerCfg = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = MCPWM_APB_CLK_HZ,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = timerPeriodTicks(),
    };
    ESP_ERROR_CHECK(mcpwm_new_timer(&timerCfg, &s_timer));

    const mcpwm_operator_config_t operCfg = {
        .group_id = 0,
    };
    ESP_ERROR_CHECK(mcpwm_new_operator(&operCfg, &s_oper));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(s_oper, s_timer));

    const mcpwm_comparator_config_t cmprCfg = {
        .flags = {
            .update_cmp_on_tez = true,
        },
    };
    ESP_ERROR_CHECK(mcpwm_new_comparator(s_oper, &cmprCfg, &s_cmpr));

    const mcpwm_generator_config_t genACfg = {
        .gen_gpio_num = PIN_IA,
    };
    const mcpwm_generator_config_t genBCfg = {
        .gen_gpio_num = PIN_IB,
    };
    ESP_ERROR_CHECK(mcpwm_new_generator(s_oper, &genACfg, &s_genA));
    ESP_ERROR_CHECK(mcpwm_new_generator(s_oper, &genBCfg, &s_genB));

    // Anti-phase drive: GEN_A high on timer EMPTY, low on COMPARE; GEN_B is the
    // complement (low on EMPTY, high on COMPARE).
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(s_genA,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(s_genA,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, s_cmpr, MCPWM_GEN_ACTION_LOW)));
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(s_genB,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_LOW)));
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(s_genB,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, s_cmpr, MCPWM_GEN_ACTION_HIGH)));

    configureDeadtime();

    ESP_ERROR_CHECK(mcpwm_timer_enable(s_timer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(s_timer, MCPWM_TIMER_START_NO_STOP));

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
        applyComparatorForDuty(0.0f);
        carrierIsOn = false;
        setTxLed(false);
        return;
    }

    applyComparatorForDuty(clampedDuty);
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
