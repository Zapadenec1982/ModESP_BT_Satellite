#pragma once
/* Protocol 1.3. Keep this header identical in both firmware repositories. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define FMT_PREPARE 0x16u
#define FMT_READY 0x23u
#define FMT_HOST_CAP (1u << 4)
#define FMT_OK 0u
#define FMT_REFUSED 1u
#define FMT_SILENCE_MS 50u
#define FMT_RETRY_MS 250u
typedef struct {
    uint32_t nonce, generation, rate_hz;
    uint8_t bits, channels;
} fmt_request_t;
typedef struct { uint32_t nonce, generation; uint8_t status; } fmt_ready_t;
static inline uint32_t fmt_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline void fmt_put32(uint8_t *p, uint32_t v) {
    for (unsigned i=0; i<4; ++i) p[i]=(uint8_t)(v>>(8*i));
}
static inline bool fmt_valid(const fmt_request_t *r) {
    return r->rate_hz && (r->bits==16 || r->bits==32) && r->channels==2;
}
static inline bool fmt_same_id(const fmt_request_t *a, const fmt_request_t *b) {
    return a->nonce==b->nonce && a->generation==b->generation;
}
static inline bool fmt_same(const fmt_request_t *a, const fmt_request_t *b) {
    return fmt_same_id(a,b) && a->rate_hz==b->rate_hz && a->bits==b->bits && a->channels==b->channels;
}
static inline void fmt_encode(uint8_t p[14], const fmt_request_t *r) {
    fmt_put32(p,r->nonce); fmt_put32(p+4,r->generation); fmt_put32(p+8,r->rate_hz);
    p[12]=r->bits; p[13]=r->channels;
}
static inline bool fmt_decode(const uint8_t *p,size_t n,fmt_request_t *r) {
    if (n!=14) return false;
    r->nonce=fmt_get32(p); r->generation=fmt_get32(p+4); r->rate_hz=fmt_get32(p+8);
    r->bits=p[12]; r->channels=p[13];
    return r->nonce && r->generation;
}
static inline void fmt_encode_ready(uint8_t p[9],const fmt_ready_t *r) {
    fmt_put32(p,r->nonce); fmt_put32(p+4,r->generation); p[8]=r->status;
}
static inline bool fmt_decode_ready(const uint8_t *p,size_t n,fmt_ready_t *r) {
    if(n!=9) return false;
    r->nonce=fmt_get32(p); r->generation=fmt_get32(p+4); r->status=p[8];
    return r->nonce && r->generation && r->status<=FMT_REFUSED;
}
static inline bool fmt_ack_capable(uint8_t minor,uint32_t caps) {
    return minor>=3 && (caps & FMT_HOST_CAP)!=0;
}
static inline uint8_t fmt_slot_bits(uint8_t decoder_bits,bool wide,bool acknowledged) {
    return wide && (!acknowledged || decoder_bits==32) ? 32 : 16;
}
/* Pure state machine used by the real transmitter and both host-test suites. */
typedef struct {
    fmt_request_t request;
    bool require_ack, configured, transmitted, accepted, refused;
    uint32_t sent_ms;
} fmt_barrier_t;
static inline void fmt_begin(fmt_barrier_t *b,fmt_request_t r,bool ack) {
    b->request=r; b->require_ack=ack; b->configured=false; b->transmitted=false;
    b->accepted=false; b->refused=false; b->sent_ms=0;
}
static inline void fmt_ack(fmt_barrier_t *b,const fmt_ready_t *r) {
    if(r->nonce!=b->request.nonce || r->generation!=b->request.generation) return;
    if(r->status==FMT_REFUSED) { b->refused=true; b->accepted=false; }
    else if(r->status==FMT_OK && !b->refused) b->accepted=true;
}
static inline void fmt_sent(fmt_barrier_t *b,uint32_t when) {
    if(!b->transmitted) { b->sent_ms=when; b->transmitted=true; }
}
static inline bool fmt_can_resume(const fmt_barrier_t *b,uint32_t now) {
    return fmt_valid(&b->request) && b->configured && b->transmitted && !b->refused &&
           (uint32_t)(now-b->sent_ms)>=FMT_SILENCE_MS && (!b->require_ack || b->accepted);
}

static inline bool fmt_can_resume_source(const fmt_barrier_t *b,uint32_t now,
                                         uint32_t handled_epoch,uint32_t latest_epoch) {
    return handled_epoch==latest_epoch && fmt_can_resume(b,now);
}
