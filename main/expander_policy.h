/*
 * The rules of GPIO_CFG (PROTOCOL.md section 11): which mode a line really gets,
 * and how often a mode 6 line reports.
 *
 * Pure C, no IDF: main/expander.c applies what these decide, and
 * tests/host/test_rules.c walks them -- with an empty table the expander code
 * that uses them cannot be reached on the board at all.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "satlink/proto.h"

/* Poll fast, accept slowly. The spec allows 20 ms of debounce and asks for the
 * frame to be on the wire within 30 ms of the edge: 5 ms of granularity plus 20
 * ms of settling leaves room for the frame itself.
 *
 * The period comes from an esp_timer, not from the FreeRTOS tick: at the default
 * 100 Hz, pdMS_TO_TICKS(5) is 0 and the poller would spin instead of waiting. */
#define EXP_POLL_MS        5
#define EXP_DEBOUNCE_MS    20
#define EXP_STABLE_SAMPLES (EXP_DEBOUNCE_MS / EXP_POLL_MS)

_Static_assert(EXP_DEBOUNCE_MS % EXP_POLL_MS == 0, "debounce must be whole poll periods");
_Static_assert(100 % EXP_POLL_MS == 0, "exp_adc_period_ticks needs 100 ms in whole polls");
_Static_assert(EXP_POLL_MS + EXP_DEBOUNCE_MS < 30, "edge-to-wire bound of PROTOCOL.md 11");

/* Why a requested mode was replaced, for the warning in LOG. */
typedef enum {
    EXP_MODE_AS_ASKED = 0,
    EXP_MODE_NO_PULL,           /* pull-up or pull-down on a line without one: plain input */
    EXP_MODE_INPUT_ONLY,        /* output on an input-only line: plain input */
    EXP_MODE_NO_ADC,            /* analog on a line without an ADC: plain input */
    EXP_MODE_UNKNOWN,           /* a mode the protocol does not define: disabled */
} exp_mode_fix_t;

/*
 * The mode a line gets when the host asks for `mode`. The protocol has no request
 * and response pairs to say no with, so an impossible mode becomes the nearest
 * legal one. A mode above 6 has no nearest: it is disabled, which drives nothing
 * and reports nothing.
 */
static inline uint8_t exp_legal_mode(uint8_t mode, uint8_t flags, exp_mode_fix_t *why)
{
    exp_mode_fix_t fix = EXP_MODE_AS_ASKED;
    if (mode > SATLINK_PIN_ANALOG) {
        fix = EXP_MODE_UNKNOWN;
        mode = SATLINK_PIN_DISABLED;
    } else if ((mode == SATLINK_PIN_INPUT_PU || mode == SATLINK_PIN_INPUT_PD) &&
               (flags & SATLINK_PIN_HAS_PULL) == 0) {
        fix = EXP_MODE_NO_PULL;
        mode = SATLINK_PIN_INPUT;
    } else if ((mode == SATLINK_PIN_OUTPUT || mode == SATLINK_PIN_OUTPUT_OD) &&
               (flags & SATLINK_PIN_CAN_OUTPUT) == 0) {
        fix = EXP_MODE_INPUT_ONLY;
        mode = SATLINK_PIN_INPUT;
    } else if (mode == SATLINK_PIN_ANALOG && (flags & SATLINK_PIN_CAN_ADC) == 0) {
        fix = EXP_MODE_NO_ADC;
        mode = SATLINK_PIN_INPUT;
    }
    if (why != NULL) {
        *why = fix;
    }
    return mode;
}

/* Lines in these modes are polled, debounced and reported in GPIO_IN. */
static inline bool exp_mode_is_input(uint8_t mode)
{
    return mode == SATLINK_PIN_INPUT || mode == SATLINK_PIN_INPUT_PU ||
           mode == SATLINK_PIN_INPUT_PD;
}

/*
 * One poll of an input line, `bit` its place in the masks and `level` either `bit`
 * or 0. A level must be read EXP_STABLE_SAMPLES polls in a row after it first
 * differs before it replaces the reported one. True when the reported level of
 * this line changed, which is when the host is told.
 */
static inline bool exp_debounce_step(uint32_t level, uint32_t bit, uint32_t *candidate,
                                     uint8_t *stable, uint32_t *reported)
{
    if (level == (*candidate & bit)) {
        if (*stable < EXP_STABLE_SAMPLES) {
            (*stable)++;
        }
    } else {
        *candidate = (*candidate & ~bit) | level;
        *stable = 0;
    }

    if (*stable >= EXP_STABLE_SAMPLES && (*reported & bit) != level) {
        *reported = (*reported & ~bit) | level;
        return true;
    }
    return false;
}

/*
 * GPIO_CFG made the lines in `lines` inputs, and `levels` is what they read now.
 * Those levels are taken as settled and reported straight away, because the host
 * has no other way to learn a level that never changes. Only those lines: one
 * that is already an input may be in the middle of its debounce, and starting
 * that over would put its edge past the 30 ms of PROTOCOL.md 11.
 */
static inline void exp_debounce_adopt(uint32_t lines, uint32_t levels, uint32_t *candidate,
                                      uint8_t stable[SATLINK_GPIO_MAX], uint32_t *reported)
{
    levels &= lines;
    *reported  = (*reported & ~lines) | levels;
    *candidate = (*candidate & ~lines) | levels;
    for (unsigned i = 0; i < SATLINK_GPIO_MAX; i++) {
        if (lines & (1u << i)) {
            stable[i] = EXP_STABLE_SAMPLES;
        }
    }
}

/*
 * Whether ADC_READ may sample a line: only one that has an ADC and is in mode 6
 * now. A line without CAN_ADC has no channel of its own, and a CAN_ADC line in a
 * digital mode has neither the attenuation the millivolts assume nor its pad
 * handed to the ADC -- taking it would undo what GPIO_CFG set up.
 */
static inline bool exp_adc_readable(uint8_t flags, bool analog)
{
    return (flags & SATLINK_PIN_CAN_ADC) != 0 && analog;
}

/* Mode 6: `initial` is the report period in hundreds of milliseconds, 0 = only on
 * ADC_READ. In poll ticks, which is what the expander counts. */
static inline uint16_t exp_adc_period_ticks(uint8_t initial)
{
    return (uint16_t)(initial * (100u / EXP_POLL_MS));
}

/*
 * One poll tick of a mode 6 line. `countdown` starts at the period; true when a
 * sample is due, and then the countdown starts over. A period of 0 is never due.
 */
static inline bool exp_adc_tick(uint16_t *countdown, uint16_t period)
{
    if (period == 0) {
        return false;
    }
    if (*countdown > 1) {
        (*countdown)--;
        return false;
    }
    *countdown = period;
    return true;
}
