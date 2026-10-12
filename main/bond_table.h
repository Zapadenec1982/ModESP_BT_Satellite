/*
 * The bonded phones as the host is shown them (protocol 1.8): their names, and the
 * order in which they last connected.
 *
 * Bluedroid keeps the bonds themselves -- link key, key type, PIN length, a Secure
 * Connections flag -- and nothing else: no names, and no order this firmware may
 * rely on. This table adds both, and follows the stack's own list for which phones
 * are bonded at all. Pure state, no IDF; tests/host/test_bt_settings.c walks it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bt_settings.h"
#include "satlink/proto.h"

/* A little over the 16 a BOND_LIST carries, and at the stack's own limit: this
 * build keeps 19 security records in RAM (BTM_SEC_MAX_DEVICE_RECORDS, 15 + 4). */
#define BOND_TABLE_MAX 20

typedef struct {
    uint8_t  addr[6];
    uint32_t seq;               /* higher = connected more recently; 0 = not since 1.8 knew it */
    char     name[SATLINK_BT_NAME_MAX + 1];   /* empty = unknown */
} bond_entry_t;

typedef struct {
    uint8_t      count;
    uint32_t     next_seq;
    bond_entry_t e[BOND_TABLE_MAX];
} bond_table_t;

static inline int bond_index(const bond_table_t *t, const uint8_t addr[6])
{
    for (int i = 0; i < t->count; i++) {
        if (memcmp(t->e[i].addr, addr, 6) == 0) {
            return i;
        }
    }
    return -1;
}

static inline void bond_drop_at(bond_table_t *t, int i)
{
    memmove(&t->e[i], &t->e[i + 1], (size_t)(t->count - i - 1) * sizeof(t->e[0]));
    t->count--;
}

static inline bool bond_remove(bond_table_t *t, const uint8_t addr[6])
{
    int i = bond_index(t, addr);
    if (i < 0) {
        return false;
    }
    bond_drop_at(t, i);
    return true;
}

/* The entry for `addr`, made if there is room. -1 when the table is full. */
static inline int bond_add(bond_table_t *t, const uint8_t addr[6])
{
    int i = bond_index(t, addr);
    if (i >= 0) {
        return i;
    }
    if (t->count >= BOND_TABLE_MAX) {
        return -1;
    }
    i = t->count++;
    memset(&t->e[i], 0, sizeof(t->e[i]));
    memcpy(t->e[i].addr, addr, 6);
    return i;
}

/* Follows the stack: forgets what it no longer holds and learns what it holds. */
static inline bool bond_sync(bond_table_t *t, const uint8_t (*bonded)[6], int n)
{
    bool changed = false;
    for (int i = t->count - 1; i >= 0; i--) {
        bool held = false;
        for (int k = 0; k < n && !held; k++) {
            held = memcmp(bonded[k], t->e[i].addr, 6) == 0;
        }
        if (!held) {
            bond_drop_at(t, i);
            changed = true;
        }
    }
    for (int k = 0; k < n; k++) {
        if (bond_index(t, bonded[k]) < 0 && bond_add(t, bonded[k]) >= 0) {
            changed = true;
        }
    }
    return changed;
}

/* The order, most recent first: entries by falling seq, ties in table order. */
static inline int bond_order(const bond_table_t *t, uint8_t order[BOND_TABLE_MAX])
{
    int n = 0;
    for (int i = 0; i < t->count; i++) {
        int k = n++;
        while (k > 0 && t->e[order[k - 1]].seq < t->e[i].seq) {
            order[k] = order[k - 1];
            k--;
        }
        order[k] = (uint8_t)i;
    }
    return n;
}

/* A phone connected: it becomes the most recent, and the auto-reconnect target. */
static inline bool bond_touch(bond_table_t *t, const uint8_t addr[6])
{
    int i = bond_add(t, addr);
    if (i < 0) {
        return false;
    }
    if (t->next_seq == UINT32_MAX) {
        /* Four billion connections in: number them again, keeping the order. */
        uint8_t order[BOND_TABLE_MAX];
        int n = bond_order(t, order);
        for (int k = 0; k < n; k++) {
            t->e[order[k]].seq = t->e[order[k]].seq ? (uint32_t)(n - k) : 0;
        }
        t->next_seq = (uint32_t)n;
    }
    t->e[i].seq = ++t->next_seq;
    return true;
}

/* The auto-reconnect target: the most recently connected, if any ever was. */
static inline int bond_last(const bond_table_t *t)
{
    int best = -1;
    for (int i = 0; i < t->count; i++) {
        if (t->e[i].seq != 0 && (best < 0 || t->e[i].seq > t->e[best].seq)) {
            best = i;
        }
    }
    return best;
}

/* A name learned at pairing or from a name read: clipped to 32 bytes on a
 * character boundary, and ignored when it is not text. */
static inline bool bond_set_name(bond_table_t *t, const uint8_t addr[6], const char *name, size_t len)
{
    len = bt_utf8_clip(name, len, SATLINK_BT_NAME_MAX);
    if (len == 0 || !bt_utf8_valid(name, len)) {
        return false;
    }
    int i = bond_add(t, addr);
    if (i < 0) {
        return false;
    }
    if (strlen(t->e[i].name) == len && memcmp(t->e[i].name, name, len) == 0) {
        return false;
    }
    memcpy(t->e[i].name, name, len);
    t->e[i].name[len] = '\0';
    return true;
}

/* Whether a name learned at pairing or from a name read is the name of the phone
 * BT_STATE shows: only when it is that phone, and attached. Another phone may pair
 * while one is playing, and its name must not stand next to the first one's address. */
static inline bool bond_names_attached(uint8_t state, const uint8_t now_addr[6], const uint8_t addr[6])
{
    return state >= SATLINK_BT_CONNECTED && memcmp(now_addr, addr, 6) == 0;
}

/* The NVS record: version, count, the counter, then per entry the address, its
 * seq and the name with its length. */
#define BOND_TABLE_BLOB_VERSION 1
#define BOND_TABLE_BLOB_MAX     (1 + 1 + 4 + BOND_TABLE_MAX * (6 + 4 + 1 + SATLINK_BT_NAME_MAX))

static inline void bond_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t bond_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline size_t bond_table_pack(const bond_table_t *t, uint8_t *out, size_t cap)
{
    size_t pos = 6;
    if (cap < pos || t->count > BOND_TABLE_MAX) {
        return 0;
    }
    out[0] = BOND_TABLE_BLOB_VERSION;
    out[1] = t->count;
    bond_put32(out + 2, t->next_seq);
    for (int i = 0; i < t->count; i++) {
        size_t name_len = strlen(t->e[i].name);
        if (cap - pos < 6 + 4 + 1 + name_len) {
            return 0;
        }
        memcpy(out + pos, t->e[i].addr, 6);
        bond_put32(out + pos + 6, t->e[i].seq);
        out[pos + 10] = (uint8_t)name_len;
        memcpy(out + pos + 11, t->e[i].name, name_len);
        pos += 11 + name_len;
    }
    return pos;
}

/* False for anything this build did not write, and then `t` is left alone: the
 * caller starts empty and the next sync with the stack refills the addresses.
 * Checked in full before anything is written, so no half-read table survives, and
 * without a second table on the stack -- this runs on tasks with 3.5 KiB of it. */
static inline bool bond_table_unpack(bond_table_t *t, const uint8_t *in, size_t len)
{
    if (len < 6 || in[0] != BOND_TABLE_BLOB_VERSION || in[1] > BOND_TABLE_MAX) {
        return false;
    }
    uint8_t count = in[1];
    uint32_t next_seq = bond_get32(in + 2);
    size_t pos = 6;
    for (int i = 0; i < count; i++) {
        if (len - pos < 11) {
            return false;
        }
        size_t name_len = in[pos + 10];
        if (name_len > SATLINK_BT_NAME_MAX || len - pos - 11 < name_len ||
            !bt_utf8_valid((const char *)in + pos + 11, name_len) ||
            bond_get32(in + pos + 6) > next_seq) {
            return false;
        }
        pos += 11 + name_len;
    }
    if (pos != len) {
        return false;
    }

    memset(t, 0, sizeof(*t));
    t->count = count;
    t->next_seq = next_seq;
    pos = 6;
    for (int i = 0; i < count; i++) {
        size_t name_len = in[pos + 10];
        memcpy(t->e[i].addr, in + pos, 6);
        t->e[i].seq = bond_get32(in + pos + 6);
        memcpy(t->e[i].name, in + pos + 11, name_len);
        t->e[i].name[name_len] = '\0';
        pos += 11 + name_len;
    }
    return true;
}
