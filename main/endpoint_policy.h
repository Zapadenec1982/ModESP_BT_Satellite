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
 * Since protocol 1.8 the offered codecs are part of the same decision: the host
 * can switch them off and on, the A2DP_SinkOffer* switches are read at
 * registration too, and a change waits for the same moment -- no phone attached --
 * rather than dropping one.
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

/*
 * Whether the host can follow a stream at this rate, from the same bits the
 * endpoints are decided from: 48 kHz needs RELOCK, 96 kHz needs 96KHZ (which the
 * HELLO normalisation already clears without RELOCK). Endpoints change only
 * between connections, so a host that greets mid-stream can find the phone at a
 * rate offered to its predecessor. Such a stream stays gated and unannounced,
 * PROTOCOL.md 6.5 for 96 kHz and by the same reasoning for 48 kHz, until a host
 * that can follow it greets or the phone reconnects to endpoints decided for this
 * one. Other rates are never offered and are left to the caller.
 */
static inline bool ep_host_can_follow(uint32_t rate_hz, uint32_t host_caps)
{
    if (rate_hz == 48000) {
        return (host_caps & SATLINK_HOST_CAP_RELOCK) != 0;
    }
    if (rate_hz == 96000) {
        return (host_caps & SATLINK_HOST_CAP_96KHZ) != 0;
    }
    return true;
}

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
    uint8_t    codecs;      /* and the codecs they offered, SATLINK_CODEC_BIT_* (1.8) */
    bool       retry_armed; /* a refusal set retry_at_ms; nothing before it */
    uint32_t   retry_at_ms;
} ep_fsm_t;

static inline uint32_t ep_deciding(uint32_t host_caps)
{
    return host_caps & EP_DECIDING_CAPS;
}

/* bt_av_start registered the endpoints, from these caps and these codecs. */
static inline void ep_registered(ep_fsm_t *f, uint32_t host_caps, uint8_t codecs)
{
    f->state = EP_STATE_IDLE;
    f->decided = ep_deciding(host_caps);
    f->codecs = codecs;
    f->retry_armed = false;
    f->retry_at_ms = 0;
}

/* Would the endpoints be registered differently from what the host says now?
 * Narrower counts as well as wider: a replacement host that cannot relock
 * must not be offered 48 kHz just because its predecessor could take it. The
 * same for codecs, in both directions. */
static inline bool ep_stale(const ep_fsm_t *f, uint32_t host_caps, uint8_t codecs)
{
    return f->state != EP_STATE_BOOT &&
           (ep_deciding(host_caps) != f->decided || codecs != f->codecs);
}

/* Signed comparison so the millisecond clock may wrap under a long uptime; an
 * armed retry, not a zero, is what says "wait", because zero is also a time. */
static inline bool ep_retry_due(const ep_fsm_t *f, uint32_t now_ms)
{
    return !f->retry_armed || (int32_t)(now_ms - f->retry_at_ms) >= 0;
}

/* Periodic. `idle` is "no A2DP connection in any stage and no AVRCP": tearing
 * the endpoints down under a phone would drop it. */
static inline ep_action_t ep_poll(ep_fsm_t *f, uint32_t host_caps, uint8_t codecs, bool idle,
                                  uint32_t now_ms)
{
    switch (f->state) {
    case EP_STATE_IDLE:
        if (!ep_stale(f, host_caps, codecs) || !idle || !ep_retry_due(f, now_ms)) {
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

/* esp_a2d_sink_init() accepted the request, with the switches set from these caps
 * and these codecs. */
static inline void ep_init_sent(ep_fsm_t *f, uint32_t host_caps, uint8_t codecs)
{
    f->state = EP_STATE_INIT_WAIT;
    f->decided = ep_deciding(host_caps);
    f->codecs = codecs;
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
