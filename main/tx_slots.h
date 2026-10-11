/*
 * The device's messages that must not be lost (PROTOCOL.md 13, DEVICE_SPEC rule
 * 1): one slot per message type, in static memory.
 *
 * A message waiting in a slot is replaced by the next one of its type, which
 * carries the newer state of the same thing; the host is told the latest and
 * never a stale one. Nothing is evicted and nothing is allocated, so a slot never
 * overflows and a full heap cannot lose a BT_STATE. GPIO_IN is merged rather than
 * replaced: the newest levels, and every line that changed in either.
 *
 * Slots go out oldest update first. An update moves its slot behind everything
 * queued before it, so newer state never overtakes older messages of other types.
 *
 * Pure C, no locking and no IDF: main/link.c holds its spinlock around these and
 * tests/host/test_rules.c walks them.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "satlink/proto.h"

typedef struct {
    uint8_t   type;
    uint8_t   key;          /* ADC_VALUE: the line index; 0 for every other type.
                             * A type may have several slots: an idle one is taken
                             * for a key that has none, which is how a few ADC slots
                             * serve every line. */
    uint16_t  cap;          /* bytes `buf` holds: the largest payload of the type */
    uint8_t  *buf;
    bool      pending;
    uint16_t  len;
    uint32_t  stamp;        /* order of the last update; wraps */
    uint32_t  fmt_nonce, fmt_generation;   /* STREAM_FMT / STREAM_PREPARE tags */
} tx_slot_t;

typedef enum {
    TX_SLOT_QUEUED = 0,     /* the slot was idle */
    TX_SLOT_REPLACED,       /* an unsent message of the type was superseded */
    TX_SLOT_MERGED,         /* GPIO_IN folded into the unsent one */
    TX_SLOT_NO_SLOT,        /* no slot for this type: a fault in this build */
    TX_SLOT_TOO_LONG,       /* longer than the slot: a fault in this build */
} tx_slot_result_t;

/* The slot holding (type, key), or an idle slot of the type taken for the key,
 * or NULL: every slot of the type is busy with other keys. */
static inline tx_slot_t *tx_slot_find(tx_slot_t *slots, size_t n, uint8_t type, uint8_t key)
{
    tx_slot_t *idle = NULL;
    for (size_t i = 0; i < n; i++) {
        if (slots[i].type != type) {
            continue;
        }
        if (slots[i].key == key) {
            return &slots[i];
        }
        if (idle == NULL && !slots[i].pending) {
            idle = &slots[i];
        }
    }
    if (idle != NULL) {
        idle->key = key;
    }
    return idle;
}

static inline uint32_t tx_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void tx_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Stores a message in its slot under `stamp`. `*stamp` is advanced. */
static inline tx_slot_result_t tx_slot_put(tx_slot_t *slots, size_t n, uint8_t type, uint8_t key,
                                           const uint8_t *payload, size_t len,
                                           uint32_t nonce, uint32_t generation, uint32_t *stamp)
{
    tx_slot_t *s = tx_slot_find(slots, n, type, key);
    if (s == NULL) {
        return TX_SLOT_NO_SLOT;
    }
    if (len > s->cap) {
        return TX_SLOT_TOO_LONG;
    }

    tx_slot_result_t result = s->pending ? TX_SLOT_REPLACED : TX_SLOT_QUEUED;
    uint32_t changed_before = 0;
    /* GPIO_IN: values u32, changed u32. A line that changed in the unsent one
     * still changed, whatever it did since. */
    if (type == SATLINK_MSG_GPIO_IN && s->pending && s->len == 8 && len == 8) {
        changed_before = tx_get32(s->buf + 4);
        result = TX_SLOT_MERGED;
    }
    if (len != 0) {
        memcpy(s->buf, payload, len);
    }
    if (result == TX_SLOT_MERGED) {
        tx_put32(s->buf + 4, tx_get32(s->buf + 4) | changed_before);
    }
    s->len = (uint16_t)len;
    s->fmt_nonce = nonce;
    s->fmt_generation = generation;
    s->stamp = (*stamp)++;
    s->pending = true;
    return result;
}

/* Encodes a message straight into its slot: for the one too large for a sender's
 * stack, and which must not depend on the heap. An encoder that fails leaves the
 * slot idle -- what was waiting in it is gone, and the build has a fault. */
typedef size_t (*tx_encoder_t)(uint8_t *buf, size_t cap, const void *msg);

static inline tx_slot_result_t tx_slot_encode(tx_slot_t *slots, size_t n, uint8_t type,
                                              tx_encoder_t encode, const void *msg,
                                              uint32_t *stamp)
{
    tx_slot_t *s = tx_slot_find(slots, n, type, 0);
    if (s == NULL) {
        return TX_SLOT_NO_SLOT;
    }
    const bool was_pending = s->pending;
    size_t len = encode(s->buf, s->cap, msg);
    if (len == 0 || len > s->cap) {
        s->pending = false;
        return TX_SLOT_TOO_LONG;
    }
    s->len = (uint16_t)len;
    s->fmt_nonce = 0;
    s->fmt_generation = 0;
    s->stamp = (*stamp)++;
    s->pending = true;
    return was_pending ? TX_SLOT_REPLACED : TX_SLOT_QUEUED;
}

/* The pending slot updated longest ago, or NULL. Stamps wrap; the distance
 * between pending ones is tiny, so the signed difference orders them. */
static inline tx_slot_t *tx_slot_oldest(tx_slot_t *slots, size_t n)
{
    tx_slot_t *oldest = NULL;
    for (size_t i = 0; i < n; i++) {
        if (slots[i].pending &&
            (oldest == NULL || (int32_t)(slots[i].stamp - oldest->stamp) < 0)) {
            oldest = &slots[i];
        }
    }
    return oldest;
}

/* Copies the oldest pending message out and frees its slot. False when none.
 * `out` must hold the largest slot. */
static inline bool tx_slot_take(tx_slot_t *slots, size_t n, uint8_t *out, size_t out_cap,
                                uint8_t *type, uint16_t *len,
                                uint32_t *nonce, uint32_t *generation)
{
    tx_slot_t *s = tx_slot_oldest(slots, n);
    if (s == NULL || s->len > out_cap) {
        return false;
    }
    if (s->len != 0) {
        memcpy(out, s->buf, s->len);
    }
    *type = s->type;
    *len = s->len;
    *nonce = s->fmt_nonce;
    *generation = s->fmt_generation;
    s->pending = false;
    return true;
}
