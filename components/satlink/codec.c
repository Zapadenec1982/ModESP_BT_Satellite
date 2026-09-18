#include "satlink/codec.h"

#include <string.h>

void satlink_wr_init(satlink_wr_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->ok  = (buf != NULL);
}

static bool wr_room(satlink_wr_t *w, size_t need)
{
    if (!w->ok || w->len + need > w->cap) {
        w->ok = false;
        return false;
    }
    return true;
}

void satlink_wr_u8(satlink_wr_t *w, uint8_t v)
{
    if (!wr_room(w, 1)) { return; }
    w->buf[w->len++] = v;
}

void satlink_wr_u16(satlink_wr_t *w, uint16_t v)
{
    if (!wr_room(w, 2)) { return; }
    w->buf[w->len++] = (uint8_t)(v & 0xFF);
    w->buf[w->len++] = (uint8_t)(v >> 8);
}

void satlink_wr_u32(satlink_wr_t *w, uint32_t v)
{
    if (!wr_room(w, 4)) { return; }
    w->buf[w->len++] = (uint8_t)(v & 0xFF);
    w->buf[w->len++] = (uint8_t)((v >> 8) & 0xFF);
    w->buf[w->len++] = (uint8_t)((v >> 16) & 0xFF);
    w->buf[w->len++] = (uint8_t)((v >> 24) & 0xFF);
}

void satlink_wr_bytes(satlink_wr_t *w, const uint8_t *data, size_t len)
{
    if (len == 0) { return; }
    if (data == NULL) { w->ok = false; return; }
    if (!wr_room(w, len)) { return; }
    memcpy(w->buf + w->len, data, len);
    w->len += len;
}

void satlink_wr_str(satlink_wr_t *w, const char *s)
{
    size_t len = (s != NULL) ? strlen(s) : 0;
    if (len > 0xFFFFu) {
        w->ok = false;
        return;
    }
    satlink_wr_u16(w, (uint16_t)len);
    satlink_wr_bytes(w, (const uint8_t *)s, len);
}

void satlink_rd_init(satlink_rd_t *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
    r->ok  = (buf != NULL || len == 0);
}

static bool rd_have(satlink_rd_t *r, size_t need)
{
    if (!r->ok || r->pos + need > r->len) {
        r->ok = false;
        return false;
    }
    return true;
}

uint8_t satlink_rd_u8(satlink_rd_t *r)
{
    if (!rd_have(r, 1)) { return 0; }
    return r->buf[r->pos++];
}

uint16_t satlink_rd_u16(satlink_rd_t *r)
{
    if (!rd_have(r, 2)) { return 0; }
    uint16_t v = (uint16_t)r->buf[r->pos] | (uint16_t)((uint16_t)r->buf[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}

uint32_t satlink_rd_u32(satlink_rd_t *r)
{
    if (!rd_have(r, 4)) { return 0; }
    uint32_t v = (uint32_t)r->buf[r->pos]
               | ((uint32_t)r->buf[r->pos + 1] << 8)
               | ((uint32_t)r->buf[r->pos + 2] << 16)
               | ((uint32_t)r->buf[r->pos + 3] << 24);
    r->pos += 4;
    return v;
}

bool satlink_rd_bytes(satlink_rd_t *r, uint8_t *out, size_t len)
{
    if (len == 0) { return r->ok; }
    if (out == NULL || !rd_have(r, len)) { r->ok = false; return false; }
    memcpy(out, r->buf + r->pos, len);
    r->pos += len;
    return true;
}

bool satlink_rd_str(satlink_rd_t *r, const char **out, uint16_t *out_len)
{
    uint16_t len = satlink_rd_u16(r);
    if (!rd_have(r, len)) { return false; }
    if (out != NULL)     { *out = (const char *)(r->buf + r->pos); }
    if (out_len != NULL) { *out_len = len; }
    r->pos += len;
    return true;
}

bool satlink_rd_done(const satlink_rd_t *r)
{
    return r->ok && r->pos == r->len;
}
