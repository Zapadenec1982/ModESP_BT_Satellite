/*
 * Pairing through the host (protocol 1.8): when to answer the stack, when to ask
 * the host, and which PAIR_RESULT closes the request.
 *
 * The rules are the contract's. One request at a time. Kinds 0 and 1 wait for
 * PAIR_REPLY at most 25 s and are then refused by the device itself. Kind 2 waits
 * for nothing from the host. Every PAIR_REQUEST ends in exactly one PAIR_RESULT,
 * so a request the stack never finishes is closed here too, after a bound longer
 * than the stack's own 35 s. Without the host's PAIR_CONFIRM bit nobody is asked
 * and every confirmation is answered yes, as in 1.7.
 *
 * Pure state, no IDF: bt_av.c feeds it GAP events and PAIR_REPLY on its own task
 * and carries out what comes back; tests/host/test_bt_settings.c walks it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "satlink/proto.h"

#define PAIR_REPLY_TIMEOUT_MS   25000u
/* For the part the stack finishes on its own: its timer is 35 s, and a request
 * must not stay open for ever if it never reports. */
#define PAIR_RESULT_TIMEOUT_MS  60000u

typedef enum {
    PAIR_IDLE = 0,
    PAIR_ASKING,        /* the host has the question and has not answered (kinds 0, 1) */
    PAIR_WAITING,       /* nothing left for the host; the stack finishes (kind 2, or a yes) */
} pair_phase_t;

typedef struct {
    pair_phase_t phase;
    uint8_t      addr[6];
    uint8_t      kind;              /* satlink_pair_kind_t */
    uint32_t     passkey;
    char         name[SATLINK_BT_NAME_MAX + 1];
    uint32_t     deadline_ms;
} pair_fsm_t;

/* What to do now. More than one can be set: a timeout both answers the stack and
 * reports to the host. */
typedef struct {
    bool     reply;                 /* answer the stack's confirmation */
    bool     accept;                /*   ... yes or no */
    bool     ask;                   /* send PAIR_REQUEST from the fsm's fields */
    bool     result;                /* send PAIR_RESULT */
    uint8_t  status;                /*   ... with this satlink_pair_status_t */
    uint8_t  addr[6];               /* whom the reply and the result are about */
} pair_out_t;

static inline bool pair_due(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;    /* wraps with a long uptime */
}

static inline void pair_take(pair_fsm_t *f, const uint8_t addr[6], uint8_t kind,
                             uint32_t passkey, const char *name)
{
    memcpy(f->addr, addr, 6);
    f->kind = kind;
    f->passkey = passkey;
    size_t len = name ? strlen(name) : 0;
    if (len > SATLINK_BT_NAME_MAX) {
        len = SATLINK_BT_NAME_MAX;              /* callers clip on a character first */
    }
    if (len) {
        memcpy(f->name, name, len);
    }
    f->name[len] = '\0';
}

/* The stack asks to confirm a pairing: a number to compare, or Just Works. */
static inline pair_out_t pair_on_confirm(pair_fsm_t *f, const uint8_t addr[6], uint32_t number,
                                         bool just_works, const char *name,
                                         bool host_confirms, uint32_t now_ms)
{
    pair_out_t out;
    memset(&out, 0, sizeof(out));
    memcpy(out.addr, addr, 6);

    if (!host_confirms) {
        out.reply = true;                   /* 1.7: nobody to ask, so yes */
        out.accept = true;
        return out;
    }
    if (f->phase != PAIR_IDLE && memcmp(f->addr, addr, 6) != 0) {
        out.reply = true;                   /* one at a time: the second is refused */
        out.accept = false;
        return out;
    }
    f->phase = PAIR_ASKING;
    pair_take(f, addr, just_works ? SATLINK_PAIR_CONSENT : SATLINK_PAIR_NUMERIC,
              just_works ? 0 : number % (SATLINK_PASSKEY_MAX + 1), name);
    f->deadline_ms = now_ms + PAIR_REPLY_TIMEOUT_MS;
    out.ask = true;
    return out;
}

/* The stack shows a passkey for the phone's keyboard: tell the host, wait for
 * nothing. Without a host that confirms there is nowhere to show it. */
static inline pair_out_t pair_on_passkey(pair_fsm_t *f, const uint8_t addr[6], uint32_t passkey,
                                         const char *name, bool host_confirms, uint32_t now_ms)
{
    pair_out_t out;
    memset(&out, 0, sizeof(out));
    memcpy(out.addr, addr, 6);
    if (!host_confirms || (f->phase != PAIR_IDLE && memcmp(f->addr, addr, 6) != 0)) {
        return out;
    }
    f->phase = PAIR_WAITING;
    pair_take(f, addr, SATLINK_PAIR_PASSKEY, passkey % (SATLINK_PASSKEY_MAX + 1), name);
    f->deadline_ms = now_ms + PAIR_RESULT_TIMEOUT_MS;
    out.ask = true;
    return out;
}

/* PAIR_REPLY. Only the request being asked, and only its own address. */
static inline pair_out_t pair_on_reply(pair_fsm_t *f, const uint8_t addr[6], bool accept,
                                       uint32_t now_ms)
{
    pair_out_t out;
    memset(&out, 0, sizeof(out));
    memcpy(out.addr, addr, 6);
    if (f->phase != PAIR_ASKING || memcmp(f->addr, addr, 6) != 0) {
        return out;
    }
    out.reply = true;
    out.accept = accept;
    if (accept) {
        f->phase = PAIR_WAITING;
        f->deadline_ms = now_ms + PAIR_RESULT_TIMEOUT_MS;
    } else {
        /* Reported now rather than when the stack confirms the failure: the
         * host's answer is the result, and the stack's echo of it is ignored. */
        out.result = true;
        out.status = SATLINK_PAIR_REJECTED;
        f->phase = PAIR_IDLE;
    }
    return out;
}

/* Time passes. The host has 25 s to answer; the stack has the rest. */
static inline pair_out_t pair_on_tick(pair_fsm_t *f, uint32_t now_ms)
{
    pair_out_t out;
    memset(&out, 0, sizeof(out));
    memcpy(out.addr, f->addr, 6);
    if (f->phase == PAIR_IDLE || !pair_due(now_ms, f->deadline_ms)) {
        return out;
    }
    if (f->phase == PAIR_ASKING) {
        out.reply = true;
        out.accept = false;
        out.status = SATLINK_PAIR_TIMEOUT;
    } else {
        out.status = SATLINK_PAIR_FAILED;   /* the stack never said */
    }
    out.result = true;
    f->phase = PAIR_IDLE;
    return out;
}

/* The stack finished. Only for the request this fsm opened: pairings nobody was
 * asked about, and re-authentication of a bonded phone, report nothing. */
static inline pair_out_t pair_on_done(pair_fsm_t *f, const uint8_t addr[6], bool success)
{
    pair_out_t out;
    memset(&out, 0, sizeof(out));
    memcpy(out.addr, addr, 6);
    if (f->phase == PAIR_IDLE || memcmp(f->addr, addr, 6) != 0) {
        return out;
    }
    out.result = true;
    out.status = success ? SATLINK_PAIR_OK : SATLINK_PAIR_FAILED;
    f->phase = PAIR_IDLE;
    return out;
}

/* Whether a HELLO should hear this request again: while the host still has to
 * decide, or has a passkey to keep showing. After a yes it has nothing to do, and
 * asking again would only put up a dialog nobody can answer. */
static inline bool pair_should_repeat(const pair_fsm_t *f)
{
    return f->phase == PAIR_ASKING ||
           (f->phase == PAIR_WAITING && f->kind == SATLINK_PAIR_PASSKEY);
}
