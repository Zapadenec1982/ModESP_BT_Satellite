#include "satlink/messages.h"

#include <string.h>

#include "satlink/codec.h"

satlink_str_t satlink_str(const char *s)
{
    satlink_str_t out = { .ptr = s, .len = 0 };
    if (s != NULL) {
        size_t n = strlen(s);
        out.len = (n > 0xFFFFu) ? 0xFFFFu : (uint16_t)n;
    }
    return out;
}

satlink_str_t satlink_str_clip(const char *s, uint16_t max_bytes)
{
    satlink_str_t out = satlink_str(s);
    if (out.len <= max_bytes) {
        return out;
    }

    /* Back up off a continuation byte, so the last character is either whole or
     * gone. A continuation byte is 10xxxxxx; anything else starts a character. */
    uint16_t n = max_bytes;
    while (n > 0 && ((uint8_t)s[n] & 0xC0u) == 0x80u) {
        n--;
    }
    out.len = n;
    return out;
}

size_t satlink_utf8_prefix(const char *s, size_t len)
{
    if (s == NULL) {
        return 0;
    }
    /* Back over at most three continuation bytes (10xxxxxx) to the byte that
     * starts the last character, then ask it how long that character is. */
    size_t start = len;
    while (start > 0 && len - start < 3 && ((uint8_t)s[start - 1] & 0xC0u) == 0x80u) {
        start--;
    }
    if (start == 0) {
        return len;
    }
    uint8_t lead = (uint8_t)s[start - 1];
    size_t need = ((lead & 0xE0u) == 0xC0u) ? 2
                : ((lead & 0xF0u) == 0xE0u) ? 3
                : ((lead & 0xF8u) == 0xF0u) ? 4
                : 1;
    return (len - (start - 1) >= need) ? len : start - 1;
}

/* Every encoder ends the same way: the sticky flag decides, and a failed write
 * yields 0 rather than a half-built payload. */
static size_t wr_finish(const satlink_wr_t *w)
{
    return w->ok ? w->len : 0;
}

static void wr_str(satlink_wr_t *w, const satlink_str_t *s)
{
    satlink_wr_u16(w, s->len);
    if (s->len != 0) {
        satlink_wr_bytes(w, (const uint8_t *)s->ptr, s->len);
    }
}

static bool rd_str(satlink_rd_t *r, satlink_str_t *s)
{
    return satlink_rd_str(r, &s->ptr, &s->len);
}

/* Trailing bytes mean the two ends disagree about the layout, and that is worth
 * a rejection rather than a shrug. */
static bool rd_finish(const satlink_rd_t *r)
{
    return satlink_rd_done(r);
}

/* ---------------------------------------------------------------------- HELLO */

size_t satlink_enc_hello(uint8_t *buf, size_t cap, const satlink_hello_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u8(&w, m->major);
    satlink_wr_u8(&w, m->minor);
    satlink_wr_u32(&w, m->caps);
    wr_str(&w, &m->build);
    return wr_finish(&w);
}

bool satlink_dec_hello(const uint8_t *buf, size_t len, satlink_hello_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->major = satlink_rd_u8(&r);
    m->minor = satlink_rd_u8(&r);
    m->caps  = satlink_rd_u32(&r);
    if (!rd_str(&r, &m->build)) { return false; }
    return rd_finish(&r);
}

/* ----------------------------------------------------------------- PING, PONG */

size_t satlink_enc_ping(uint8_t *buf, size_t cap, const satlink_ping_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u32(&w, m->tick_ms);
    return wr_finish(&w);
}

bool satlink_dec_ping(const uint8_t *buf, size_t len, satlink_ping_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->tick_ms = satlink_rd_u32(&r);
    return rd_finish(&r);
}

size_t satlink_enc_pong(uint8_t *buf, size_t cap, const satlink_pong_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u32(&w, m->echo_ms);
    satlink_wr_u32(&w, m->own_ms);
    return wr_finish(&w);
}

bool satlink_dec_pong(const uint8_t *buf, size_t len, satlink_pong_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->echo_ms = satlink_rd_u32(&r);
    m->own_ms  = satlink_rd_u32(&r);
    return rd_finish(&r);
}

/* ------------------------------------------------------------------- BT_STATE */

size_t satlink_enc_bt_state(uint8_t *buf, size_t cap, const satlink_bt_state_t_msg *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u8(&w, m->state);
    satlink_wr_u8(&w, m->codec);
    satlink_wr_bytes(&w, m->addr, sizeof(m->addr));
    wr_str(&w, &m->name);
    return wr_finish(&w);
}

bool satlink_dec_bt_state(const uint8_t *buf, size_t len, satlink_bt_state_t_msg *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->state = satlink_rd_u8(&r);
    m->codec = satlink_rd_u8(&r);
    if (!satlink_rd_bytes(&r, m->addr, sizeof(m->addr))) { return false; }
    if (!rd_str(&r, &m->name)) { return false; }
    return rd_finish(&r);
}

/* ----------------------------------------------------------------- TRACK_META */

size_t satlink_enc_track_meta(uint8_t *buf, size_t cap, const satlink_track_meta_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    wr_str(&w, &m->title);
    wr_str(&w, &m->artist);
    wr_str(&w, &m->album);
    return wr_finish(&w);
}

bool satlink_dec_track_meta(const uint8_t *buf, size_t len, satlink_track_meta_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    if (!rd_str(&r, &m->title))  { return false; }
    if (!rd_str(&r, &m->artist)) { return false; }
    if (!rd_str(&r, &m->album))  { return false; }
    return rd_finish(&r);
}

/* ------------------------------------------------------------------ TRACK_POS */

size_t satlink_enc_track_pos(uint8_t *buf, size_t cap, const satlink_track_pos_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u8(&w, m->state);
    satlink_wr_u32(&w, m->elapsed_ms);
    satlink_wr_u32(&w, m->total_ms);
    return wr_finish(&w);
}

bool satlink_dec_track_pos(const uint8_t *buf, size_t len, satlink_track_pos_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->state      = satlink_rd_u8(&r);
    m->elapsed_ms = satlink_rd_u32(&r);
    m->total_ms   = satlink_rd_u32(&r);
    return rd_finish(&r);
}

uint32_t satlink_sanitize_total_ms(int64_t reported_ms)
{
    const int64_t one_day_ms = 24LL * 60 * 60 * 1000;
    if (reported_ms <= 0 || reported_ms > one_day_ms) {
        return 0;
    }
    return (uint32_t)reported_ms;
}

/* ----------------------------------------------------------------- STREAM_FMT */

size_t satlink_enc_stream_fmt(uint8_t *buf, size_t cap, const satlink_stream_fmt_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u32(&w, m->rate_hz);
    satlink_wr_u8(&w, m->bits);
    satlink_wr_u8(&w, m->channels);
    return wr_finish(&w);
}

bool satlink_dec_stream_fmt(const uint8_t *buf, size_t len, satlink_stream_fmt_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->rate_hz  = satlink_rd_u32(&r);
    m->bits     = satlink_rd_u8(&r);
    m->channels = satlink_rd_u8(&r);
    return rd_finish(&r);
}

/* ---------------------------------------------------------------------- COVER */

size_t satlink_enc_cover(uint8_t *buf, size_t cap, const satlink_cover_t *m)
{
    if (m->len > SATLINK_COVER_CHUNK_MAX) {
        return 0;
    }
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u16(&w, m->chunk);
    satlink_wr_u16(&w, m->total);
    satlink_wr_u8(&w, m->mime);
    satlink_wr_u32(&w, m->total_len);
    satlink_wr_u16(&w, m->len);
    if (m->len != 0) {
        satlink_wr_bytes(&w, m->data, m->len);
    }
    return wr_finish(&w);
}

bool satlink_dec_cover(const uint8_t *buf, size_t len, satlink_cover_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->chunk     = satlink_rd_u16(&r);
    m->total     = satlink_rd_u16(&r);
    m->mime      = satlink_rd_u8(&r);
    m->total_len = satlink_rd_u32(&r);
    m->len       = satlink_rd_u16(&r);
    if (!r.ok || m->len > SATLINK_COVER_CHUNK_MAX) {
        return false;
    }
    if (r.pos + m->len != r.len) {
        return false;
    }
    m->data = (m->len != 0) ? (buf + r.pos) : NULL;
    return true;
}

/* --------------------------------------------------------------- VOLUME_STATE */

size_t satlink_enc_volume_state(uint8_t *buf, size_t cap, const satlink_volume_state_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u8(&w, m->volume);
    satlink_wr_u8(&w, m->source);
    return wr_finish(&w);
}

bool satlink_dec_volume_state(const uint8_t *buf, size_t len, satlink_volume_state_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->volume = satlink_rd_u8(&r);
    m->source = satlink_rd_u8(&r);
    return rd_finish(&r);
}

/* ------------------------------------------------ PAIR_REQUEST .. BOND_LIST, 1.8 */

size_t satlink_enc_pair_request(uint8_t *buf, size_t cap, const satlink_pair_request_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_bytes(&w, m->addr, sizeof(m->addr));
    satlink_wr_u8(&w, m->kind);
    satlink_wr_u32(&w, m->passkey);
    wr_str(&w, &m->name);
    return wr_finish(&w);
}

bool satlink_dec_pair_request(const uint8_t *buf, size_t len, satlink_pair_request_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    if (!satlink_rd_bytes(&r, m->addr, sizeof(m->addr))) { return false; }
    m->kind    = satlink_rd_u8(&r);
    m->passkey = satlink_rd_u32(&r);
    if (!rd_str(&r, &m->name)) { return false; }
    if (!rd_finish(&r)) { return false; }
    /* The ranges are the contract's, and each one is a code a person would read
     * off a screen and compare: a number with seven digits, or a code on a request
     * that has none, is a broken frame and not something to show. */
    if (m->kind > SATLINK_PAIR_PASSKEY || m->passkey > SATLINK_PASSKEY_MAX) { return false; }
    if (m->kind == SATLINK_PAIR_CONSENT && m->passkey != 0) { return false; }
    return m->name.len <= SATLINK_BT_NAME_MAX;
}

size_t satlink_enc_pair_result(uint8_t *buf, size_t cap, const satlink_pair_result_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_bytes(&w, m->addr, sizeof(m->addr));
    satlink_wr_u8(&w, m->status);
    return wr_finish(&w);
}

bool satlink_dec_pair_result(const uint8_t *buf, size_t len, satlink_pair_result_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    if (!satlink_rd_bytes(&r, m->addr, sizeof(m->addr))) { return false; }
    /* Not range-checked: from 4 up is reserved and read as FAILED, which is a
     * meaning, not an error. */
    m->status = satlink_rd_u8(&r);
    return rd_finish(&r);
}

size_t satlink_enc_bt_config(uint8_t *buf, size_t cap, const satlink_bt_config_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u8(&w, m->radio);
    satlink_wr_u8(&w, m->discoverable);
    satlink_wr_u8(&w, m->auto_reconnect);
    satlink_wr_u8(&w, m->codecs_supported);
    satlink_wr_u8(&w, m->codecs_enabled);
    wr_str(&w, &m->name);
    return wr_finish(&w);
}

bool satlink_dec_bt_config(const uint8_t *buf, size_t len, satlink_bt_config_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->radio            = satlink_rd_u8(&r);
    m->discoverable     = satlink_rd_u8(&r);
    m->auto_reconnect   = satlink_rd_u8(&r);
    m->codecs_supported = satlink_rd_u8(&r);
    m->codecs_enabled   = satlink_rd_u8(&r);
    if (!rd_str(&r, &m->name)) { return false; }
    if (!rd_finish(&r)) { return false; }
    if (m->radio > 1 || m->discoverable > 1 || m->auto_reconnect > 1) { return false; }
    /* SBC is the floor every A2DP sink must offer, so a report without it, or one
     * enabling a codec the build does not have, describes no device that exists. */
    if (!(m->codecs_supported & SATLINK_CODEC_BIT_SBC) ||
        !(m->codecs_enabled & SATLINK_CODEC_BIT_SBC) ||
        (m->codecs_enabled & ~m->codecs_supported) != 0) { return false; }
    return m->name.len >= 1 && m->name.len <= SATLINK_BT_NAME_MAX;
}

size_t satlink_enc_bond_list(uint8_t *buf, size_t cap, const satlink_bond_list_t *m)
{
    if (m->count > SATLINK_BOND_LIST_MAX) { return 0; }
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u8(&w, m->total);
    satlink_wr_u8(&w, m->count);
    for (uint8_t i = 0; i < m->count; i++) {
        satlink_wr_bytes(&w, m->bonds[i].addr, sizeof(m->bonds[i].addr));
        satlink_wr_u8(&w, m->bonds[i].flags);
        wr_str(&w, &m->bonds[i].name);
    }
    return wr_finish(&w);
}

bool satlink_dec_bond_list(const uint8_t *buf, size_t len, satlink_bond_list_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->total = satlink_rd_u8(&r);
    m->count = satlink_rd_u8(&r);
    if (!r.ok || m->count > SATLINK_BOND_LIST_MAX || m->count > m->total) { return false; }
    for (uint8_t i = 0; i < m->count; i++) {
        if (!satlink_rd_bytes(&r, m->bonds[i].addr, sizeof(m->bonds[i].addr))) { return false; }
        m->bonds[i].flags = satlink_rd_u8(&r);
        if (!rd_str(&r, &m->bonds[i].name)) { return false; }
        if (m->bonds[i].name.len > SATLINK_BT_NAME_MAX) { return false; }
    }
    return rd_finish(&r);
}

/* ------------------------------------------------------------- host commands */

size_t satlink_enc_cmd_transport(uint8_t *buf, size_t cap, const satlink_cmd_transport_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u8(&w, m->cmd);
    return wr_finish(&w);
}

bool satlink_dec_cmd_transport(const uint8_t *buf, size_t len, satlink_cmd_transport_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->cmd = satlink_rd_u8(&r);
    return rd_finish(&r);
}

size_t satlink_enc_cmd_volume(uint8_t *buf, size_t cap, const satlink_cmd_volume_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u8(&w, m->volume);
    return wr_finish(&w);
}

bool satlink_dec_cmd_volume(const uint8_t *buf, size_t len, satlink_cmd_volume_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->volume = satlink_rd_u8(&r);
    return rd_finish(&r);
}

size_t satlink_enc_cmd_radio(uint8_t *buf, size_t cap, const satlink_cmd_radio_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u8(&w, m->radio);
    satlink_wr_u8(&w, m->discoverable);
    satlink_wr_u8(&w, m->disconnect);
    return wr_finish(&w);
}

bool satlink_dec_cmd_radio(const uint8_t *buf, size_t len, satlink_cmd_radio_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->radio        = satlink_rd_u8(&r);
    m->discoverable = satlink_rd_u8(&r);
    m->disconnect   = satlink_rd_u8(&r);
    return rd_finish(&r);
}

/* ----------------------------------------------------------------- COVER_RETRY */

size_t satlink_enc_cover_retry(uint8_t *buf, size_t cap, const satlink_cover_retry_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u32(&w, m->boot_nonce);
    satlink_wr_u32(&w, m->track_generation);
    return wr_finish(&w);
}

bool satlink_dec_cover_retry(const uint8_t *buf, size_t len, satlink_cover_retry_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->boot_nonce       = satlink_rd_u32(&r);
    m->track_generation = satlink_rd_u32(&r);
    /* "Both nonzero" is the contract's own wording, and it is what makes the
     * all-zero frame -- the shape a truncated or padded one takes -- fail here
     * rather than name a pair and cost the phone an OBEX round trip. */
    return rd_finish(&r) && m->boot_nonce != 0 && m->track_generation != 0;
}

/* ---------------------------------------------- PAIR_REPLY .. BT_REFRESH, 1.8 */

size_t satlink_enc_pair_reply(uint8_t *buf, size_t cap, const satlink_pair_reply_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_bytes(&w, m->addr, sizeof(m->addr));
    satlink_wr_u8(&w, m->accept);
    return wr_finish(&w);
}

bool satlink_dec_pair_reply(const uint8_t *buf, size_t len, satlink_pair_reply_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    if (!satlink_rd_bytes(&r, m->addr, sizeof(m->addr))) { return false; }
    m->accept = satlink_rd_u8(&r);
    /* Strictly 0 or 1. Everywhere else "nonzero is yes" is harmless; here yes means
     * a new phone joins, and a stray byte must not be the thing that says so. A
     * refused reply leaves the request to time out, which is a no. */
    return rd_finish(&r) && m->accept <= 1;
}

size_t satlink_enc_bt_config_set(uint8_t *buf, size_t cap, const satlink_bt_config_set_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u8(&w, m->radio);
    satlink_wr_u8(&w, m->discoverable);
    satlink_wr_u8(&w, m->auto_reconnect);
    satlink_wr_u8(&w, m->codecs_enabled);
    wr_str(&w, &m->name);
    return wr_finish(&w);
}

bool satlink_dec_bt_config_set(const uint8_t *buf, size_t len, satlink_bt_config_set_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->radio          = satlink_rd_u8(&r);
    m->discoverable   = satlink_rd_u8(&r);
    m->auto_reconnect = satlink_rd_u8(&r);
    m->codecs_enabled = satlink_rd_u8(&r);
    if (!rd_str(&r, &m->name)) { return false; }
    return rd_finish(&r);
}

size_t satlink_enc_bond_remove(uint8_t *buf, size_t cap, const satlink_bond_remove_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_bytes(&w, m->addr, sizeof(m->addr));
    return wr_finish(&w);
}

bool satlink_dec_bond_remove(const uint8_t *buf, size_t len, satlink_bond_remove_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    if (!satlink_rd_bytes(&r, m->addr, sizeof(m->addr))) { return false; }
    return rd_finish(&r);
}

bool satlink_dec_bt_refresh(const uint8_t *buf, size_t len)
{
    (void)buf;
    return len == 0;
}

/* --------------------------------------------------------------- GPIO_IN, OUT */

size_t satlink_enc_gpio_in(uint8_t *buf, size_t cap, const satlink_gpio_in_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u32(&w, m->values);
    satlink_wr_u32(&w, m->changed);
    return wr_finish(&w);
}

bool satlink_dec_gpio_in(const uint8_t *buf, size_t len, satlink_gpio_in_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->values  = satlink_rd_u32(&r);
    m->changed = satlink_rd_u32(&r);
    return rd_finish(&r);
}

size_t satlink_enc_gpio_out(uint8_t *buf, size_t cap, const satlink_gpio_out_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u32(&w, m->values);
    satlink_wr_u32(&w, m->mask);
    return wr_finish(&w);
}

bool satlink_dec_gpio_out(const uint8_t *buf, size_t len, satlink_gpio_out_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->values = satlink_rd_u32(&r);
    m->mask   = satlink_rd_u32(&r);
    return rd_finish(&r);
}

/* ------------------------------------------------------------------- GPIO_CFG */

size_t satlink_enc_gpio_cfg(uint8_t *buf, size_t cap, const satlink_gpio_cfg_t *m)
{
    if (m->count > SATLINK_GPIO_MAX) {
        return 0;
    }
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u8(&w, m->count);
    for (uint8_t i = 0; i < m->count; i++) {
        satlink_wr_u8(&w, m->pins[i].index);
        satlink_wr_u8(&w, m->pins[i].mode);
        satlink_wr_u8(&w, m->pins[i].initial);
    }
    return wr_finish(&w);
}

bool satlink_dec_gpio_cfg(const uint8_t *buf, size_t len, satlink_gpio_cfg_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->count = satlink_rd_u8(&r);
    if (!r.ok || m->count > SATLINK_GPIO_MAX) {
        return false;
    }
    for (uint8_t i = 0; i < m->count; i++) {
        m->pins[i].index   = satlink_rd_u8(&r);
        m->pins[i].mode    = satlink_rd_u8(&r);
        m->pins[i].initial = satlink_rd_u8(&r);
    }
    return rd_finish(&r);
}

/* ------------------------------------------------------------------- GPIO_MAP */

size_t satlink_enc_gpio_map(uint8_t *buf, size_t cap, const satlink_gpio_map_t *m)
{
    if (m->count > SATLINK_GPIO_MAX) {
        return 0;
    }
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u8(&w, m->count);
    for (uint8_t i = 0; i < m->count; i++) {
        satlink_wr_u8(&w, m->pins[i].index);
        satlink_wr_u8(&w, m->pins[i].gpio);
        satlink_wr_u8(&w, m->pins[i].flags);
    }
    return wr_finish(&w);
}

bool satlink_dec_gpio_map(const uint8_t *buf, size_t len, satlink_gpio_map_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->count = satlink_rd_u8(&r);
    if (!r.ok || m->count > SATLINK_GPIO_MAX) {
        return false;
    }
    for (uint8_t i = 0; i < m->count; i++) {
        m->pins[i].index = satlink_rd_u8(&r);
        m->pins[i].gpio  = satlink_rd_u8(&r);
        m->pins[i].flags = satlink_rd_u8(&r);
    }
    return rd_finish(&r);
}

/* ------------------------------------------------------ ADC_VALUE, ADC_READ */

size_t satlink_enc_adc_value(uint8_t *buf, size_t cap, const satlink_adc_value_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u8(&w, m->index);
    satlink_wr_u16(&w, m->raw);
    satlink_wr_u16(&w, m->mv);
    return wr_finish(&w);
}

bool satlink_dec_adc_value(const uint8_t *buf, size_t len, satlink_adc_value_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->index = satlink_rd_u8(&r);
    m->raw   = satlink_rd_u16(&r);
    m->mv    = satlink_rd_u16(&r);
    return rd_finish(&r);
}

size_t satlink_enc_adc_read(uint8_t *buf, size_t cap, const satlink_adc_read_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u8(&w, m->index);
    return wr_finish(&w);
}

bool satlink_dec_adc_read(const uint8_t *buf, size_t len, satlink_adc_read_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->index = satlink_rd_u8(&r);
    return rd_finish(&r);
}

/* ------------------------------------------------------------------------ LOG */

size_t satlink_enc_log(uint8_t *buf, size_t cap, const satlink_log_t *m)
{
    satlink_wr_t w;
    satlink_wr_init(&w, buf, cap);
    satlink_wr_u8(&w, m->level);
    wr_str(&w, &m->text);
    return wr_finish(&w);
}

bool satlink_dec_log(const uint8_t *buf, size_t len, satlink_log_t *m)
{
    satlink_rd_t r;
    satlink_rd_init(&r, buf, len);
    m->level = satlink_rd_u8(&r);
    if (!rd_str(&r, &m->text)) { return false; }
    return rd_finish(&r);
}
