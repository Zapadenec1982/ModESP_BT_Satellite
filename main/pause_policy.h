/*
 * The check after a pause from the host (DEVICE_SPEC "A host pause the phone
 * ignores"), as pure C: bt_av.c runs it on an esp_timer, tests/host/test_rules.c walks it.
 *
 * A PAUSE or STOP from the host arms a one-shot wait; a stream still going when it runs
 * out is suspended. The wait is for a phone that ignores the pause, so it ends early on
 * anything that shows the phone did not: another transport command from the host, the
 * phone's stream stopping, or the phone reporting that it plays again. The last two
 * matter because a stream the user restarts on the phone inside the wait would otherwise
 * be suspended after the phone's "playing" has come and gone, and Android does not start
 * a stream the sink suspended again by itself.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "satlink/proto.h"

typedef enum {
    PAUSE_TIMER_LEAVE,      /* no change */
    PAUSE_TIMER_START,      /* start the one-shot wait */
    PAUSE_TIMER_STOP,       /* drop a wait still pending */
} pause_timer_op_t;

/* A transport command from the host. A repeated PAUSE does not restart the wait: the host
 * repeats it every 3 s while the phone keeps playing, which is shorter than the wait. */
static inline pause_timer_op_t pause_on_transport(uint8_t cmd, bool waiting)
{
    if (cmd == SATLINK_XPORT_PAUSE || cmd == SATLINK_XPORT_STOP) {
        return waiting ? PAUSE_TIMER_LEAVE : PAUSE_TIMER_START;
    }
    return PAUSE_TIMER_STOP;
}

/* The phone's A2DP stream started or stopped. A stream that stopped is not "still going",
 * whatever it does next. */
static inline pause_timer_op_t pause_on_stream(bool started)
{
    return started ? PAUSE_TIMER_LEAVE : PAUSE_TIMER_STOP;
}

/* A PLAY_STATUS_CHANGE notification from the phone. Only a change gets here (the stack
 * drops INTERIM answers), so a phone that ignores the pause and keeps saying "playing"
 * never ends the wait; and a PAUSE the host repeats starts it again. */
static inline pause_timer_op_t pause_on_play_status(uint8_t play_state)
{
    return play_state == SATLINK_PLAY_PLAYING ? PAUSE_TIMER_STOP : PAUSE_TIMER_LEAVE;
}

/* The wait ran out: suspend only a stream that is still going. */
static inline bool pause_suspends(uint8_t bt_state)
{
    return bt_state == SATLINK_BT_STREAMING;
}
