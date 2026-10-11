/*
 * The Bluetooth settings the host may change (protocol 1.8), and the rules for
 * taking a BT_CONFIG_SET apart.
 *
 * The device holds them, in NVS; the host only shows them and sends the whole
 * state back. Pure state, no IDF: bt_av.c keeps the one copy on its own task,
 * saves it and applies it to the stack, and tests/host/test_bt_settings.c walks
 * the rules.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "satlink/proto.h"

typedef struct {
    uint8_t radio;              /* 0/1 */
    uint8_t discoverable;       /* 0/1: 0 also refuses new pairings */
    uint8_t auto_reconnect;     /* 0/1 */
    /*
     * The codecs someone switched OFF, not the ones that are on.
     *
     * Stored this way round so that a codec a later build adds is on until
     * someone turns it off, while a codec someone did turn off stays off across
     * a build that lacks it and one that brings it back. Never holds SBC.
     */
    uint8_t codecs_off;
    char    name[SATLINK_BT_NAME_MAX + 1];
} bt_settings_t;

/* What the device offers: inside the build, and SBC always, because every A2DP
 * sink must and it is the floor a phone falls back to. */
static inline uint8_t bt_codecs_enabled(const bt_settings_t *s, uint8_t supported)
{
    return (uint8_t)((supported & ~s->codecs_off) | SATLINK_CODEC_BIT_SBC);
}

/*
 * Protocol 1.10: aptX Low Latency is a codec only a host at minor 10 can name.
 *
 * Below it the codec is not there at all: not in BT_CONFIG, not offered to a
 * phone, and out of reach of that host's BT_CONFIG_SET, so a choice a 1.10 host
 * made about it survives an older one. An endpoint a phone may take while the host
 * is never told about it is worse than none (docs/BASE.md). Zero, a host that has
 * not spoken yet, is below 10.
 */
#define BT_CODEC_MINOR_APTX_LL  10

static inline uint8_t bt_codecs_for_host(uint8_t build, uint8_t host_minor)
{
    if (host_minor < BT_CODEC_MINOR_APTX_LL) {
        build &= (uint8_t)~SATLINK_CODEC_BIT_APTX_LL;
    }
    return build;
}

/* What BT_STATE calls the codec. A connection can outlive the host that let it be
 * made, and a host below 1.10 that takes over is told aptX: what reaches it on I2S
 * is exactly that, and a number it does not know never crosses the wire. */
static inline uint8_t bt_codec_for_host(uint8_t codec, uint8_t host_minor)
{
    if (codec == SATLINK_CODEC_APTX_LL && host_minor < BT_CODEC_MINOR_APTX_LL) {
        return SATLINK_CODEC_APTX;
    }
    return codec;
}

/*
 * Well-formed UTF-8: no NUL, no stray continuation bytes, no overlong forms, no
 * surrogates, nothing past U+10FFFF. A name is shown by the panel and handed to
 * every phone nearby, and both read it as text.
 */
static inline bool bt_utf8_valid(const char *s, size_t len)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t i = 0;
    while (i < len) {
        uint8_t c = p[i];
        if (c == 0x00) {
            return false;
        }
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t n;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07u; }
        else { return false; }
        if (len - i < n + 1) {
            return false;
        }
        for (size_t k = 1; k <= n; k++) {
            uint8_t cc = p[i + k];
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += n + 1;
    }
    return true;
}

/* The longest prefix of `s` no longer than `max` bytes that does not cut a
 * character in half. Assumes `s` is UTF-8; stray bytes are simply counted. */
static inline size_t bt_utf8_clip(const char *s, size_t len, size_t max)
{
    if (len <= max) {
        return len;
    }
    size_t n = max;
    while (n > 0 && ((uint8_t)s[n] & 0xC0u) == 0x80u) {
        n--;
    }
    return n;
}

/* A device name the device will take: 1..32 bytes of valid UTF-8. */
static inline bool bt_name_ok(const char *s, size_t len)
{
    return len >= 1 && len <= SATLINK_BT_NAME_MAX && bt_utf8_valid(s, len);
}

/* The factory state PROTOCOL.md names: radio on, visible, no reconnect, every
 * codec of the build, and the name the device has always had. */
static inline void bt_settings_defaults(bt_settings_t *s, const char *name)
{
    memset(s, 0, sizeof(*s));
    s->radio = 1;
    s->discoverable = 1;
    s->auto_reconnect = 0;
    s->codecs_off = 0;
    size_t len = strlen(name);
    len = bt_utf8_clip(name, len, SATLINK_BT_NAME_MAX);
    memcpy(s->name, name, len);
    s->name[len] = '\0';
}

/* What bt_settings_apply() changed, so the caller touches only that. */
#define BT_CHANGED_RADIO         (1u << 0)
#define BT_CHANGED_DISCOVERABLE  (1u << 1)
#define BT_CHANGED_RECONNECT     (1u << 2)
#define BT_CHANGED_CODECS        (1u << 3)
#define BT_CHANGED_NAME          (1u << 4)

/*
 * Takes a BT_CONFIG_SET: the whole state, not a delta.
 *
 * In the three flags 0 is no and anything else yes. Codec bits outside the build
 * are ignored and SBC is put back whatever the bit says. The name changes only to
 * a valid one: empty means "keep", and too long or not UTF-8 is refused the same
 * way, since the contract says BT_CONFIG then shows the old name.
 */
static inline unsigned bt_settings_apply(bt_settings_t *s, uint8_t supported,
                                         uint8_t radio, uint8_t discoverable,
                                         uint8_t auto_reconnect, uint8_t codecs_enabled,
                                         const char *name, size_t name_len)
{
    unsigned changed = 0;
    uint8_t r = radio ? 1 : 0, d = discoverable ? 1 : 0, a = auto_reconnect ? 1 : 0;
    if (r != s->radio)          { s->radio = r;          changed |= BT_CHANGED_RADIO; }
    if (d != s->discoverable)   { s->discoverable = d;   changed |= BT_CHANGED_DISCOVERABLE; }
    if (a != s->auto_reconnect) { s->auto_reconnect = a; changed |= BT_CHANGED_RECONNECT; }

    /* Only the build's own codecs are decided here; a choice about one this
     * build lacks is kept as it was. */
    uint8_t off = (uint8_t)((s->codecs_off & ~supported) | (supported & ~codecs_enabled));
    off &= (uint8_t)~SATLINK_CODEC_BIT_SBC;
    if (off != s->codecs_off)   { s->codecs_off = off;   changed |= BT_CHANGED_CODECS; }

    if (name_len != 0 && bt_name_ok(name, name_len) &&
        (strlen(s->name) != name_len || memcmp(s->name, name, name_len) != 0)) {
        memcpy(s->name, name, name_len);
        s->name[name_len] = '\0';
        changed |= BT_CHANGED_NAME;
    }
    return changed;
}

/*
 * The NVS record: a version byte, the four fields, then the name with its length.
 * The version is there so a future layout reads as "not ours" and the device
 * starts from the factory state, rather than reading someone else's bytes as a
 * name.
 */
#define BT_SETTINGS_BLOB_VERSION  1
#define BT_SETTINGS_BLOB_MAX      (1 + 4 + 1 + SATLINK_BT_NAME_MAX)

static inline size_t bt_settings_pack(const bt_settings_t *s, uint8_t *out, size_t cap)
{
    size_t name_len = strlen(s->name);
    size_t len = 6 + name_len;
    if (cap < len || name_len > SATLINK_BT_NAME_MAX) {
        return 0;
    }
    out[0] = BT_SETTINGS_BLOB_VERSION;
    out[1] = s->radio;
    out[2] = s->discoverable;
    out[3] = s->auto_reconnect;
    out[4] = s->codecs_off;
    out[5] = (uint8_t)name_len;
    memcpy(out + 6, s->name, name_len);
    return len;
}

/* False for anything that is not exactly a record this build wrote; the caller
 * then keeps the factory state. */
static inline bool bt_settings_unpack(bt_settings_t *s, const uint8_t *in, size_t len)
{
    if (len < 6 || in[0] != BT_SETTINGS_BLOB_VERSION || len != (size_t)6 + in[5]) {
        return false;
    }
    if (in[1] > 1 || in[2] > 1 || in[3] > 1 || (in[4] & SATLINK_CODEC_BIT_SBC)) {
        return false;
    }
    if (!bt_name_ok((const char *)in + 6, in[5])) {
        return false;
    }
    s->radio = in[1];
    s->discoverable = in[2];
    s->auto_reconnect = in[3];
    s->codecs_off = in[4];
    memcpy(s->name, in + 6, in[5]);
    s->name[in[5]] = '\0';
    return true;
}
