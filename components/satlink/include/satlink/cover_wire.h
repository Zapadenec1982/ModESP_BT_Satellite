#pragma once
/* Protocol 1.7: track-scoped cover reset, JPEG data and retry. Shared with satellite. */
#include <stdint.h>
#include <stddef.h>
#define COVER_RESET 0x17
#define COVER_DATA 0x18
#define COVER_END 0x19
#define COVER_RETRY 0x24            /* 1.7, host to device: fetch this pair again */
/* Bit 6 of the HOST capability word: the host takes streaming covers (1.6). */
#define COVER_STREAM_CAP (1u << 6)
/* Bit 6 of the DEVICE capability word: the device honours COVER_RETRY (1.7).
 * The two sixes are different words and the collision is a coincidence. */
#define COVER_RETRY_CAP (1u << 6)
/* Repeats of one image, per side. Each bounds its own cost and neither relies on
 * the other's bound; the chain ends at whichever limit is reached first. */
#define COVER_RETRY_MAX 2u
#define COVER_PART_MAX 1000u
#define COVER_BYTES_MAX (256u * 1024u)
static inline uint32_t cover_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline void cover_put32(uint8_t *p, uint32_t v) {
    for (unsigned i=0; i<4; ++i) p[i]=(uint8_t)(v>>(8*i));
}
/* CRC-32/ISO-HDLC, initial/public value zero; supports incremental updates. */
static inline uint32_t cover_crc32(uint32_t crc, const uint8_t *p, size_t len) {
    crc=~crc;
    while (len--) {
        crc^=*p++;
        for (unsigned i=0; i<8; ++i) crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    return ~crc;
}
