#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "harness.h"
int g_checks, g_failures;
#define REQUIRE(cond) do { bool ok = (cond); CHECK(ok); if (!ok) return; } while (0)
#include "cover_stream.h"

static uint8_t streamed[COVER_BYTES_MAX];
static uint32_t stream_used, stream_crc;
static unsigned emits, reject_emit, ends;
static bool collect(void *ctx,uint32_t off,const uint8_t *p,size_t len,uint32_t crc) {
    (void)ctx;
    if (++emits==reject_emit) return false;
    CHECK(off==stream_used);
    if (len) {
        CHECK(len<=COVER_PART_MAX);
        memcpy(streamed+off,p,len); stream_used+=(uint32_t)len;
    } else { ++ends; stream_crc=crc; }
    return true;
}
static void stream_tests(void) {
    static uint8_t source[COVER_BYTES_MAX];
    for (size_t i=0;i<sizeof source;++i) source[i]=(uint8_t)(i*17);
    source[0]=0xff; source[1]=0xd8; source[2]=0xff;
    CHECK(cover_crc32(0,(const uint8_t*)"123456789",9)==0xcbf43926u);
    for (size_t split=1;split<=1691;split+=169) {
        cover_stream_t s={0}; emits=reject_emit=ends=stream_used=0;
        for(size_t off=0;off<sizeof source;off+=split) {
            size_t n=sizeof source-off; if(n>split)n=split;
            REQUIRE(cover_stream_feed(&s,source+off,n,false,collect,NULL));
            CHECK(!s.finished); CHECK(ends==0);
        }
        REQUIRE(cover_stream_feed(&s,NULL,0,true,collect,NULL));
        CHECK(s.finished); CHECK(ends==1); CHECK(stream_used==sizeof source);
        CHECK(memcmp(streamed,source,sizeof source)==0);
        CHECK(stream_crc==cover_crc32(0,source,sizeof source));
        CHECK(!cover_stream_feed(&s,source,1,true,collect,NULL));
    }
    for (unsigned fail=1;fail<=4;++fail) {
        cover_stream_t s={0}; emits=ends=stream_used=0; reject_emit=fail;
        CHECK(!cover_stream_feed(&s,source,2501,true,collect,NULL));
        CHECK(s.failed); CHECK(!s.finished); CHECK(ends==0);
        CHECK(!cover_stream_feed(&s,NULL,0,true,collect,NULL));
    }
    cover_stream_t s={0}; emits=ends=reject_emit=stream_used=0;
    REQUIRE(cover_stream_feed(&s,source,sizeof source,false,collect,NULL));
    CHECK(!cover_stream_feed(&s,source,1,true,collect,NULL)); CHECK(ends==0);
    s=(cover_stream_t){0}; stream_used=0;
    CHECK(!cover_stream_feed(&s,source,2,true,collect,NULL)); CHECK(ends==0);
    s=(cover_stream_t){0}; stream_used=0;
    CHECK(!cover_stream_feed(&s,source+3,10,true,collect,NULL)); CHECK(ends==0);
    CHECK(sizeof(cover_stream_t)<32); // JPEG size cannot grow sender storage.
}

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
/* What bt_av.c does when the OBEX channel closes, walked through the ways it closes. */
static void refetch_tests(void) {
    /* The old track's drain ended in an OBEX error: cover_on_data cleared busy and
     * asked for the close. The new track's handle came during the drain. */
    CHECK(cover_refetch_on_close(true,false,false,true));
    // The channel dropped by itself in the middle of that drain.
    CHECK(cover_refetch_on_close(false,true,true,true));
    // No handle for the new track yet: its metadata answer will fetch it.
    CHECK(!cover_refetch_on_close(true,false,false,false));
    CHECK(!cover_refetch_on_close(false,true,true,false));
    // The current track's own transfer died: part of it may be out, so COVER_RETRY.
    CHECK(!cover_refetch_on_close(false,false,false,true));
    CHECK(!cover_refetch_on_close(false,true,false,true));
    // A GET of the current track went out before the drain's close arrived.
    CHECK(!cover_refetch_on_close(true,true,false,true));
    /* The flag is spent on the first close, so the reconnect failing as well closes
     * with nothing pending and no further attempt. */
    CHECK(!cover_refetch_on_close(false,false,true,true));
}
int main(void) {
    pacing_tests(); stream_tests(); refetch_tests();
    printf("Cover streaming/pacing: %d checks, %d failures\n",g_checks,g_failures);
    return g_failures?1:0;
}
