/*
 * Calling the last phone back (protocol 1.8, auto_reconnect): after start, and
 * when the radio comes on, at most three attempts ten seconds apart, and none once
 * any phone has connected.
 *
 * Bounded because a phone that is off or out of range answers nothing, and every
 * page attempt holds the radio for seconds. Pure state, no IDF: bt_av.c polls it
 * from its task; tests/host/test_bt_settings.c walks it.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RC_ATTEMPTS     3
#define RC_INTERVAL_MS  10000u

typedef struct {
    uint8_t  left;              /* attempts still allowed; 0 = not calling */
    uint32_t next_ms;
} rc_fsm_t;

/* Start calling: the first attempt may go now. */
static inline void rc_arm(rc_fsm_t *f, uint32_t now_ms)
{
    f->left = RC_ATTEMPTS;
    f->next_ms = now_ms;
}

/* A phone is here, or calling is no longer wanted. */
static inline void rc_stop(rc_fsm_t *f)
{
    f->left = 0;
}

/*
 * Whether to call now. `can_call` is everything the caller knows that the fsm does
 * not: the radio is on, reconnecting is on, nobody is attached or being attached,
 * the endpoints are registered and there is a phone to call. An attempt is only
 * spent when one is actually made.
 */
static inline bool rc_due(rc_fsm_t *f, uint32_t now_ms, bool can_call)
{
    if (f->left == 0 || !can_call || (int32_t)(now_ms - f->next_ms) < 0) {
        return false;
    }
    f->left--;
    f->next_ms = now_ms + RC_INTERVAL_MS;
    return true;
}
