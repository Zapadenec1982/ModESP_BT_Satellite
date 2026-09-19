#pragma once
/* Protocol 1.5: track-scoped cover reset and JPEG data. Shared with satellite. */
#include <stdint.h>
#include <stddef.h>
#define COVER_RESET 0x17
#define COVER_DATA 0x18
#define COVER_PART_MAX 1000u
#define COVER_BYTES_MAX (256u * 1024u)
static inline uint32_t cover_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline void cover_put32(uint8_t *p, uint32_t v) {
    for (unsigned i=0; i<4; ++i) p[i]=(uint8_t)(v>>(8*i));
}
