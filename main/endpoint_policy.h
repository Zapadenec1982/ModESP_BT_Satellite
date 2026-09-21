/*
 * Which stream endpoints to register, and when to register them again.
 *
 * Bluedroid reads the allow/offer switches (A2DP_SinkAllow48kHz and friends)
 * while esp_a2d_sink_init() registers the endpoints, and never again. The
 * switches are set from the host's HELLO, so a host that greets us after the
 * endpoints exist -- a P4 booting alongside this board takes several seconds
 * to send its first HELLO -- used to leave the sink at 44.1 kHz for the whole
 * session. The only way to re-read them is esp_a2d_sink_deinit() followed by
 * esp_a2d_sink_init(), which tears the endpoints down for a moment and is
 * therefore done only with no phone attached.
 *
 * Pure state, no IDF: bt_av.c issues the calls on its own task and feeds the
 * outcomes back here, and tests/host/test_endpoints.c walks every path.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "satlink/proto.h"

/* The HELLO bits the endpoint decision is made from. Everything else the host
 * announces is applied per format transaction and needs no re-registration. */
#define EP_DECIDING_CAPS   (SATLINK_HOST_CAP_RELOCK | SATLINK_HOST_CAP_96KHZ)

/* How long a refused deinit or init is left alone before the next attempt. */
#define EP_RETRY_MS        5000u

typedef enum {
    EP_STATE_BOOT = 0,      /* not registered yet: bt_av_start owns the first one */
    EP_STATE_IDLE,          /* registered from `decided`; watching the host's caps */
    EP_STATE_DEINIT_WAIT,   /* esp_a2d_sink_deinit() issued; waiting for DEINIT_SUCCESS */
    EP_STATE_UNREGISTERED,  /* deinit done, init refused: nothing to connect to */
    EP_STATE_INIT_WAIT,     /* esp_a2d_sink_init() issued; waiting for INIT_SUCCESS */
} ep_state_t;

typedef enum {
    EP_ACT_NONE = 0,
    EP_ACT_DEINIT,          /* call esp_a2d_sink_deinit() */
    EP_ACT_INIT,            /* set the switches from the host's caps, esp_a2d_sink_init() */
} ep_action_t;

typedef struct {
    ep_state_t state;
    uint32_t   decided;     /* the EP_DECIDING_CAPS bits the endpoints were registered from */
    bool       retry_armed; /* a refusal set retry_at_ms; nothing before it */
    uint32_t   retry_at_ms;
} ep_fsm_t;

static inline uint32_t ep_deciding(uint32_t host_caps)
{
    return host_caps & EP_DECIDING_CAPS;
}

/* bt_av_start registered the endpoints, from these caps. */
static inline void ep_registered(ep_fsm_t *f, uint32_t host_caps)
{
    f->state = EP_STATE_IDLE;
    f->decided = ep_deciding(host_caps);
    f->retry_armed = false;
    f->retry_at_ms = 0;
}

/* Would the endpoints be registered differently from what the host says now?
 * Narrower counts as well as wider: a replacement host that cannot relock
 * must not be offered 48 kHz just because its predecessor could take it. */
static inline bool ep_stale(const ep_fsm_t *f, uint32_t host_caps)
{
    return f->state != EP_STATE_BOOT && ep_deciding(host_caps) != f->decided;
}

/* Signed comparison so the millisecond clock may wrap under a long uptime; an
 * armed retry, not a zero, is what says "wait", because zero is also a time. */
static inline bool ep_retry_due(const ep_fsm_t *f, uint32_t now_ms)
{
    return !f->retry_armed || (int32_t)(now_ms - f->retry_at_ms) >= 0;
}

/* Periodic. `idle` is "no A2DP connection in any stage and no AVRCP": tearing
 * the endpoints down under a phone would drop it. */
static inline ep_action_t ep_poll(ep_fsm_t *f, uint32_t host_caps, bool idle, uint32_t now_ms)
{
    switch (f->state) {
    case EP_STATE_IDLE:
        if (!ep_stale(f, host_caps) || !idle || !ep_retry_due(f, now_ms)) {
            return EP_ACT_NONE;
        }
        f->retry_armed = false;
        f->state = EP_STATE_DEINIT_WAIT;
        return EP_ACT_DEINIT;
    case EP_STATE_UNREGISTERED:
        /* No endpoints at all is worse than the wrong ones; try again whether
         * or not the caps moved, and idleness is a given with nothing to
         * connect to. */
        if (!ep_retry_due(f, now_ms)) {
            return EP_ACT_NONE;
        }
        f->retry_armed = false;
        return EP_ACT_INIT;
    default:
        return EP_ACT_NONE;
    }
}

/* The stack reported ESP_A2D_PROF_STATE_EVT. */
static inline ep_action_t ep_on_prof_state(ep_fsm_t *f, bool deinit_success)
{
    if (deinit_success) {
        if (f->state != EP_STATE_DEINIT_WAIT) {
            return EP_ACT_NONE;     /* not one we asked for */
        }
        f->state = EP_STATE_UNREGISTERED;
        return EP_ACT_INIT;
    }
    if (f->state == EP_STATE_INIT_WAIT) {
        f->state = EP_STATE_IDLE;
    }
    return EP_ACT_NONE;
}

/* esp_a2d_sink_init() accepted the request, with the switches set from these caps. */
static inline void ep_init_sent(ep_fsm_t *f, uint32_t host_caps)
{
    f->state = EP_STATE_INIT_WAIT;
    f->decided = ep_deciding(host_caps);
}

/* The API refused the action just returned. A refused deinit leaves the old
 * endpoints in place, so it is back to watching; a refused init leaves none,
 * which ep_poll keeps retrying. Either way, not before EP_RETRY_MS. */
static inline void ep_action_failed(ep_fsm_t *f, uint32_t now_ms)
{
    if (f->state == EP_STATE_DEINIT_WAIT) {
        f->state = EP_STATE_IDLE;
    }
    f->retry_armed = true;
    f->retry_at_ms = now_ms + EP_RETRY_MS;
}
