/*
 * Device-side rules of PROTOCOL.md that live in main/ as pure C, walked without a
 * board: the port expander's GPIO_CFG handling, and LOG lines from the ESP log.
 */

#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "expander_policy.h"
#include "log_line.h"

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

/* ----------------------------------------------------------------------- LOG */

/* The bridge's two steps on a copy of `in`: level first, then the tidy text. */
static uint8_t tidy(const char *in, char *out, size_t cap)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", in);
    uint8_t level = log_line_level(buf);
    snprintf(out, cap, "%s", log_line_tidy(buf));
    return level;
}

static void test_log_level(void)
{
    char text[256];

    /* IDF log v1, colours off, which is this build. */
    CHECK_EQ(tidy("E (1234) sat.link: bad\n", text, sizeof text), SATLINK_LOG_ERROR);
    CHECK(strcmp(text, "E (1234) sat.link: bad") == 0);
    CHECK_EQ(tidy("W (1) t: w\n", text, sizeof text), SATLINK_LOG_WARN);
    CHECK_EQ(tidy("I (1) t: i\n", text, sizeof text), SATLINK_LOG_INFO);
    CHECK_EQ(tidy("D (1) t: d\n", text, sizeof text), SATLINK_LOG_DEBUG);
    CHECK_EQ(tidy("V (1) t: v\n", text, sizeof text), SATLINK_LOG_DEBUG);   /* no word for it */

    /* Colours on: the escape in front no longer hides the letter, and both
     * escapes are gone from the text. */
    CHECK_EQ(tidy("\033[0;31mE (5) BT_HCI: oops\033[0m\n", text, sizeof text), SATLINK_LOG_ERROR);
    CHECK(strcmp(text, "E (5) BT_HCI: oops") == 0);
    CHECK_EQ(tidy("\033[0;33mW (5) t: careful\033[0m\n", text, sizeof text), SATLINK_LOG_WARN);
    CHECK(strcmp(text, "W (5) t: careful") == 0);
    CHECK_EQ(tidy("\033[0;32mI (5) t: fine\033[0m\n", text, sizeof text), SATLINK_LOG_INFO);
    CHECK(strcmp(text, "I (5) t: fine") == 0);

    /* No letter: a bare esp_log_write. Info, as it always was. */
    CHECK_EQ(tidy("plain text\n", text, sizeof text), SATLINK_LOG_INFO);
    CHECK(strcmp(text, "plain text") == 0);
    CHECK_EQ(tidy("Error without the shape\n", text, sizeof text), SATLINK_LOG_INFO);
    CHECK_EQ(tidy("E", text, sizeof text), SATLINK_LOG_INFO);
    CHECK_EQ(tidy("", text, sizeof text), SATLINK_LOG_INFO);
    CHECK(strcmp(text, "") == 0);

    /* A trailing 'm' that is not an escape stays. */
    CHECK_EQ(tidy("I (1) t: 42 m\n", text, sizeof text), SATLINK_LOG_INFO);
    CHECK(strcmp(text, "I (1) t: 42 m") == 0);
    tidy("I (1) t: x[0m\n", text, sizeof text);
    CHECK(strcmp(text, "I (1) t: x[0m") == 0);

    /* The reset cut short by a full buffer is dropped with the rest of it. */
    tidy("\033[0;31mE (5) t: long\033[0", text, sizeof text);
    CHECK(strcmp(text, "E (5) t: long") == 0);
    tidy("\033[0;31mE (5) t: long\033[", text, sizeof text);
    CHECK(strcmp(text, "E (5) t: long") == 0);
    tidy("\033[0;31mE (5) t: long\033", text, sizeof text);
    CHECK(strcmp(text, "E (5) t: long") == 0);

    /* Only newlines and escapes: nothing to send. */
    tidy("\033[0m\r\n", text, sizeof text);
    CHECK(strcmp(text, "") == 0);
}

int main(void)
{
    test_legal_mode();
    test_adc_period();
    test_log_level();
    printf("Device rules: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
