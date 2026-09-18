/*
 * Payload reading and writing.
 *
 * Every number on the wire is little-endian and every string is a u16 length
 * followed by UTF-8 with no terminator. Both sides carry a sticky `ok` flag: once
 * a write overflows or a read runs off the end, everything after it is a no-op and
 * the caller checks once at the finish instead of after every field.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     ok;
} satlink_wr_t;

void satlink_wr_init(satlink_wr_t *w, uint8_t *buf, size_t cap);
void satlink_wr_u8(satlink_wr_t *w, uint8_t v);
void satlink_wr_u16(satlink_wr_t *w, uint16_t v);
void satlink_wr_u32(satlink_wr_t *w, uint32_t v);
void satlink_wr_bytes(satlink_wr_t *w, const uint8_t *data, size_t len);
/* NULL is written as an empty string, because "unknown" is a normal value here. */
void satlink_wr_str(satlink_wr_t *w, const char *s);

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    bool           ok;
} satlink_rd_t;

void     satlink_rd_init(satlink_rd_t *r, const uint8_t *buf, size_t len);
uint8_t  satlink_rd_u8(satlink_rd_t *r);
uint16_t satlink_rd_u16(satlink_rd_t *r);
uint32_t satlink_rd_u32(satlink_rd_t *r);
bool     satlink_rd_bytes(satlink_rd_t *r, uint8_t *out, size_t len);

/* Points `out` straight into the payload and gives its length. No copy and no
 * terminator, so it is only valid while the payload is. */
bool satlink_rd_str(satlink_rd_t *r, const char **out, uint16_t *out_len);

/* True when everything was read and nothing is left over. A trailing surprise
 * usually means the two sides disagree about the layout. */
bool satlink_rd_done(const satlink_rd_t *r);
