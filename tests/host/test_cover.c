#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "harness.h"
int g_checks, g_failures;
#define REQUIRE(cond) do { bool ok = (cond); CHECK(ok); if (!ok) return; } while (0)
static unsigned allocations, frees, fail_after;
static void *limited_alloc(size_t size) {
    CHECK(size <= 1024 + 2*sizeof(void*));
    if (allocations == fail_after) return NULL;
    ++allocations; return malloc(size);
}
static void counted_free(void *p) { if (p) ++frees; free(p); }
#define free counted_free
#include "cover_buffer.h"
#undef free

typedef unsigned char UINT8, BOOLEAN;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
#define TRUE 1
#define FALSE 0
#define BTA_AV_NUM_RCB 3
#define BTA_AV_RC_HANDLE_NONE 255
typedef struct timer { unsigned param; void (*p_cback)(void*); } TIMER_LIST_ENT;
typedef struct { unsigned handle, cover_art_goep_hdl; } tBTA_AV_RCB;
static struct { tBTA_AV_RCB rcb[BTA_AV_NUM_RCB]; } bta_av_cb;
static unsigned sent, delay_ms, armed, stopped;
static UINT8 get_rcb_idx(tBTA_AV_RCB *r) { return r-bta_av_cb.rcb; }
static void build_and_send_empty_get_req(tBTA_AV_RCB *r) { (void)r; ++sent; }
static void bta_sys_start_timer(TIMER_LIST_ENT *t,unsigned event,unsigned ms) {
    (void)t; CHECK(event==0); delay_ms=ms; ++armed;
}
static void bta_sys_stop_timer(TIMER_LIST_ENT *t) { (void)t; ++stopped; }
#include "cover_pacing.inc"

static void buffer_tests(void) {
    uint8_t source[49152];
    for (size_t i=0;i<sizeof source;++i) source[i]=(uint8_t)(i*13);
    for (size_t split=1; split<=1691; split+=169) {
        cover_buffer_t b={0}; allocations=frees=0; fail_after=100;
        for(size_t off=0;off<sizeof source;off+=split) {
            size_t n=sizeof source-off; if(n>split)n=split;
            REQUIRE(cover_buffer_append(&b,source+off,n,sizeof source,limited_alloc));
        }
        CHECK(b.size==sizeof source);
        size_t off=0;
        for(cover_block_t *n=b.head;n;n=n->next) {
            CHECK(n->used>0 && n->used<=COVER_PART_MAX);
            CHECK(memcmp(n->data,source+off,n->used)==0); off+=n->used;
        }
        CHECK(off==sizeof source);
        CHECK(!cover_buffer_append(&b,source,1,sizeof source,limited_alloc));
        cover_buffer_clear(&b); CHECK(allocations==frees);
        cover_buffer_clear(&b); CHECK(allocations==frees);
    }
    for(unsigned fail=0;fail<50;++fail) {
        cover_buffer_t b={0}; allocations=frees=0; fail_after=fail;
        CHECK(!cover_buffer_append(&b,source,sizeof source,sizeof source,limited_alloc));
        cover_buffer_clear(&b); CHECK(allocations==frees);
    }
    cover_buffer_t b={0}; allocations=frees=0; fail_after=100;
    REQUIRE(cover_buffer_append(&b,source,11836,sizeof source,limited_alloc));
    cover_buffer_t moved=b; b=(cover_buffer_t){0};
    cover_buffer_clear(&b); CHECK(frees==0);
    cover_buffer_clear(&moved); CHECK(frees==allocations);
}
static void pacing_tests(void) {
    tBTA_AV_RCB *r=&bta_av_cb.rcb[1]; r->handle=1; r->cover_art_goep_hdl=7;
    cover_schedule_continue(r); CHECK(sent==0); CHECK(armed==1); CHECK(delay_ms==50);
    cover_continue_cb(&cover_continue_timer[1]); CHECK(sent==1);
    cover_continue_cb(&cover_continue_timer[1]); CHECK(sent==1);
    cover_schedule_continue(r); cover_cancel_continue(r);
    cover_continue_cb(&cover_continue_timer[1]); CHECK(sent==1); CHECK(stopped==1);
    cover_schedule_continue(r); r->cover_art_goep_hdl=8;
    cover_continue_cb(&cover_continue_timer[1]); CHECK(sent==1);
    cover_schedule_continue(r); r->handle=BTA_AV_RC_HANDLE_NONE;
    cover_continue_cb(&cover_continue_timer[1]); CHECK(sent==1);
    r->handle=1; cover_schedule_continue(r);
    cover_continue_cb(&cover_continue_timer[1]); CHECK(sent==2);
}
int main(void) {
    buffer_tests(); pacing_tests();
    printf("Cover buffering/pacing: %d checks, %d failures\n",g_checks,g_failures);
    return g_failures?1:0;
}
