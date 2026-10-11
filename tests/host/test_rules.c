/*
 * Device-side rules of PROTOCOL.md that live in main/ as pure C, walked without a
 * board: the port expander's GPIO_CFG handling.
 */

#include <stdio.h>

#include "harness.h"
#include "expander_policy.h"

int g_checks, g_failures;

/* ------------------------------------------------------------------ expander */

#define CAN_ALL (SATLINK_PIN_CAN_INPUT | SATLINK_PIN_CAN_OUTPUT | \
                 SATLINK_PIN_CAN_ADC | SATLINK_PIN_HAS_PULL)
#define IN_ONLY SATLINK_PIN_CAN_INPUT       /* GPIO34..39: no output, no pull */

static void test_legal_mode(void)
{
    exp_mode_fix_t why;

    /* Everything a capable line is asked for, it gets. */
    for (uint8_t mode = SATLINK_PIN_DISABLED; mode <= SATLINK_PIN_ANALOG; mode++) {
        CHECK_EQ(exp_legal_mode(mode, CAN_ALL, &why), mode);
        CHECK_EQ(why, EXP_MODE_AS_ASKED);
    }

    /* An input-only line without pulls: the nearest legal thing is a plain input. */
    CHECK_EQ(exp_legal_mode(SATLINK_PIN_INPUT_PU, IN_ONLY, &why), SATLINK_PIN_INPUT);
    CHECK_EQ(why, EXP_MODE_NO_PULL);
    CHECK_EQ(exp_legal_mode(SATLINK_PIN_INPUT_PD, IN_ONLY, &why), SATLINK_PIN_INPUT);
    CHECK_EQ(why, EXP_MODE_NO_PULL);
    CHECK_EQ(exp_legal_mode(SATLINK_PIN_OUTPUT, IN_ONLY, &why), SATLINK_PIN_INPUT);
    CHECK_EQ(why, EXP_MODE_INPUT_ONLY);
    CHECK_EQ(exp_legal_mode(SATLINK_PIN_OUTPUT_OD, IN_ONLY, &why), SATLINK_PIN_INPUT);
    CHECK_EQ(why, EXP_MODE_INPUT_ONLY);
    CHECK_EQ(exp_legal_mode(SATLINK_PIN_ANALOG, IN_ONLY, &why), SATLINK_PIN_INPUT);
    CHECK_EQ(why, EXP_MODE_NO_ADC);
    CHECK_EQ(exp_legal_mode(SATLINK_PIN_INPUT, IN_ONLY, &why), SATLINK_PIN_INPUT);
    CHECK_EQ(why, EXP_MODE_AS_ASKED);

    /* A mode the protocol does not define is disabled, and a disabled line is not
     * an input: it used to be configured as one and polled as the other. */
    for (unsigned mode = SATLINK_PIN_ANALOG + 1; mode <= 0xFF; mode++) {
        CHECK_EQ(exp_legal_mode((uint8_t)mode, CAN_ALL, &why), SATLINK_PIN_DISABLED);
        CHECK_EQ(why, EXP_MODE_UNKNOWN);
    }
    CHECK(!exp_mode_is_input(SATLINK_PIN_DISABLED));
    CHECK(exp_mode_is_input(SATLINK_PIN_INPUT));
    CHECK(exp_mode_is_input(SATLINK_PIN_INPUT_PU));
    CHECK(exp_mode_is_input(SATLINK_PIN_INPUT_PD));
    CHECK(!exp_mode_is_input(SATLINK_PIN_OUTPUT));
    CHECK(!exp_mode_is_input(SATLINK_PIN_OUTPUT_OD));
    CHECK(!exp_mode_is_input(SATLINK_PIN_ANALOG));

    CHECK_EQ(exp_legal_mode(9, CAN_ALL, NULL), SATLINK_PIN_DISABLED);   /* `why` is optional */
}

/* Ticks until each of the first `n` samples, for a line configured with `initial`. */
static void sample_ticks(uint8_t initial, unsigned *at, unsigned n, unsigned limit)
{
    const uint16_t period = exp_adc_period_ticks(initial);
    uint16_t countdown = period;
    unsigned got = 0;
    for (unsigned tick = 1; tick <= limit && got < n; tick++) {
        if (exp_adc_tick(&countdown, period)) {
            at[got++] = tick;
        }
    }
    for (; got < n; got++) {
        at[got] = 0;
    }
}

static void test_adc_period(void)
{
    /* `initial` is hundreds of milliseconds: 1 is 100 ms, which is 20 polls of 5 ms.
     * It used to be read as poll ticks, (initial + 1) x 5 ms. */
    CHECK_EQ(exp_adc_period_ticks(0), 0);
    CHECK_EQ(exp_adc_period_ticks(1), 100 / EXP_POLL_MS);
    CHECK_EQ(exp_adc_period_ticks(10), 1000 / EXP_POLL_MS);
    CHECK_EQ(exp_adc_period_ticks(255), 25500 / EXP_POLL_MS);   /* fits in u16 */

    unsigned at[3];
    sample_ticks(1, at, 3, 1000);
    CHECK_EQ(at[0] * EXP_POLL_MS, 100);
    CHECK_EQ(at[1] * EXP_POLL_MS, 200);
    CHECK_EQ(at[2] * EXP_POLL_MS, 300);

    sample_ticks(10, at, 3, 10000);
    CHECK_EQ(at[0] * EXP_POLL_MS, 1000);
    CHECK_EQ(at[1] * EXP_POLL_MS, 2000);
    CHECK_EQ(at[2] * EXP_POLL_MS, 3000);

    sample_ticks(255, at, 2, 20000);
    CHECK_EQ(at[0] * EXP_POLL_MS, 25500);
    CHECK_EQ(at[1] * EXP_POLL_MS, 51000);

    /* 0: only on ADC_READ, never by itself. */
    sample_ticks(0, at, 1, 100000);
    CHECK_EQ(at[0], 0);
}

int main(void)
{
    test_legal_mode();
    test_adc_period();
    printf("Device rules: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
