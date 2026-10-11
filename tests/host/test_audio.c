/* Driver/RTOS fakes around extracted production functions, not copies of them. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "audio_transfer.h"
#include "audio_telemetry.h"
#include "audio_refill.h"
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static int s_event_lock;
static audio_event_state_t s_events;
#define AUDIO_CHUNK_BYTES 1024
#define AUDIO_WRITE_TIMEOUT_MS 10
#define AUDIO_TRANSFER_BUDGET_MS 100
#define pdMS_TO_TICKS(n) (n)
#define pdTRUE 1
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_FAIL 2
typedef int esp_err_t;
typedef unsigned TickType_t;
typedef struct { uint32_t gate, epoch; bool silence; } transfer_context_t;
typedef struct { uint32_t gate, epoch; size_t borrowed_len; } refill_context_t;
static atomic_uint s_gate, s_play_epoch;
static atomic_uint s_decoder_stack, s_decode_max_us, s_decode_active_max_us, s_decode_gap_us;
static atomic_uint s_encoded_depth, s_encoded_max, s_short_writes, s_tx_errors;
static atomic_uint s_tx_cancelled_bytes, s_backpressure, s_pcm_cancelled_frames, s_dropped_frames;
static atomic_uint s_dropped;
static atomic_uint s_empty_polls, s_wait_recovered, s_wait_timeout, s_wait_cancel, s_wait_max_us;
static uint32_t s_packet_gate, s_packet_epoch, s_packet_start_ms, s_packet_wait_us, s_packet_src_frame;
static uint32_t s_packet_frames;
static atomic_uint s_frames_per_packet_q4;
static uint32_t s_writer_gate, s_writer_epoch;
static int64_t s_decode_start_us;
static atomic_uint s_decode_last_us, s_pcm_drop_us, s_tx_error_us;
static int s_decoder_task, task_id=1, s_producer_lock=1, s_ring, s_tx;
static bool s_dma_primed, s_streaming, s_running=true, s_tx_recover;
static unsigned s_slot_bits=32, s_src_bits=32;
static uint32_t now_ms;
static uint8_t sent_data[65536];
static size_t sent_len, ring_fill, ring_capacity=24576;
static unsigned sends, cancel_on_send, drain_per_wait, tx_limit, tx_fail;
static bool tx_zero, gate_on_send;
static unsigned refill_change, refill_receives, refill_returns, refill_delay_ms;
static size_t refill_bytes;
static bool refill_borrowed;
static uint8_t refill_data[1024];
static int64_t esp_timer_get_time(void) {return (int64_t)now_ms*1000;}
static int xTaskGetCurrentTaskHandle(void) {return task_id;}
static unsigned uxTaskGetStackHighWaterMark(void *p) {(void)p;return 1000;}
static int xSemaphoreTake(int s, unsigned t) {(void)t;return s;}
static void xSemaphoreGive(int s) {(void)s;}
static void *xRingbufferReceiveUpTo(int ring,size_t *len,TickType_t ticks,size_t limit) {
    (void)ring;
    assert(ticks==1 && limit==1024 && !refill_borrowed);
    refill_receives++;now_ms+=refill_delay_ms;
    if(refill_change==1)s_gate|=1;
    if(refill_change==2)s_gate+=2;
    if(refill_change==3)s_play_epoch++;
    if(refill_change==4)s_streaming=false;
    if(refill_change==5)s_running=false;
    if(!refill_bytes){*len=777;return NULL;}
    *len=refill_bytes;refill_borrowed=true;return refill_data;
}
static void vRingbufferReturnItem(int ring,void *item) {
    (void)ring;assert(item==refill_data && refill_borrowed);
    refill_returns++;refill_borrowed=false;
}
static void after_send(void) {
    sends++;
    if (sends==cancel_on_send) {
        if(gate_on_send) atomic_fetch_add(&s_gate,2); else atomic_fetch_add(&s_play_epoch,1);
    }
}
static int xRingbufferSend(int r,const void *p,size_t n,unsigned ticks) {
    (void)r;
    if(ring_capacity-ring_fill<n) {
        now_ms+=ticks;
        ring_fill=ring_fill>drain_per_wait ? ring_fill-drain_per_wait:0;
    }
    after_send();
    if(ring_capacity-ring_fill<n) return 0;
    memcpy(sent_data+sent_len,p,n);sent_len+=n;ring_fill+=n;return pdTRUE;
}
static int i2s_channel_write(int tx,const void *p,size_t n,size_t *written,unsigned timeout) {
    (void)tx;
    size_t count=tx_zero ? 0 : (tx_limit && n>tx_limit ? tx_limit:n);
    memcpy(sent_data+sent_len,p,count);sent_len+=count;*written=count;
    now_ms+=count<n ? timeout:1;
    after_send();return tx_fail ? ESP_FAIL : (count<n ? ESP_ERR_TIMEOUT:ESP_OK);
}
static void audio_ring_put(const void *,size_t);
static void audio_write_packet(const uint8_t *p,size_t n) {audio_ring_put(p,n);}
#include "audio_handlers.inc"
/* Replay actual generated worker scheduling with a coalesced event. */
#define UNUSED_ATTR __attribute__((unused))
#define TRUE 1
#define BTC_A2DP_SINK_STATE_ON 1
#define OSI_THREAD_MAX_TIMEOUT 0xffffffffu
#define APPL_TRACE_DEBUG(...) ((void)0)
typedef int BT_HDR;
static int btc_a2dp_sink_state=1, fake_packet;
static struct {struct {int rx_flush, RxSbcQ, data_ready_event;} btc_aa_snk_cb;} a2dp_sink_local_param;
static unsigned queued_packets, handled_packets, freed_packets, posted_events;
static bool pending_event, flush_during_decode;
void sat_audio_decode_begin(unsigned) __attribute__((weak));
void sat_audio_decode_end(void) __attribute__((weak));
static unsigned fixed_queue_length(int q) {(void)q;return queued_packets;}
static bool fixed_queue_is_empty(int q) {(void)q;return queued_packets==0;}
static void *fixed_queue_dequeue(int q,unsigned timeout) {
    (void)q;(void)timeout;
    if(!queued_packets)return NULL;
    queued_packets--;return &fake_packet;
}
static void osi_free(void *p) {(void)p;freed_packets++;}
static void btc_a2dp_sink_flush_q(int q) {(void)q;freed_packets+=queued_packets;queued_packets=0;}
static void btc_a2dp_sink_handle_inc_media(BT_HDR *p) {
    (void)p;handled_packets++;
    if(flush_during_decode)a2dp_sink_local_param.btc_aa_snk_cb.rx_flush=1;
}
static void osi_thread_post_event(int e,unsigned timeout) {
    (void)e;(void)timeout;posted_events++;pending_event=true;
}
/* Hooks live in another component in firmware; this replay defines them above. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress"
#include "audio_worker.inc"
#pragma GCC diagnostic pop
static void reset(void) {
    memset(&s_events,0,sizeof s_events);
    s_gate=2;s_play_epoch=0;s_writer_gate=2;s_writer_epoch=0;
    s_running=true;s_streaming=true;s_tx_recover=false;s_short_writes=0;s_tx_errors=0;
    s_tx_cancelled_bytes=0;s_pcm_cancelled_frames=0;s_dropped_frames=0;s_dropped=0;
    now_ms=0;sends=0;cancel_on_send=0;sent_len=0;ring_fill=0;
    drain_per_wait=0;tx_limit=0;tx_fail=0;tx_zero=false;gate_on_send=false;task_id=1;
    refill_change=0;refill_receives=0;refill_returns=0;refill_delay_ms=10;refill_bytes=0;refill_borrowed=false;
    s_empty_polls=0;s_wait_recovered=0;s_wait_timeout=0;s_wait_cancel=0;s_wait_max_us=0;
    s_frames_per_packet_q4=0;
    sat_audio_decode_begin(0);
}
int main(void) {
    uint8_t data[3840];for(size_t i=0;i<sizeof data;i++)data[i]=(uint8_t)(i*71);
    reset();tx_limit=137; /* deliberately non-frame-aligned accepted prefix */
    i2s_write_locked(data,1024);
    assert(sent_len==1024 && !memcmp(sent_data,data,1024));
    assert(s_short_writes==7 && !s_tx_errors && !s_tx_cancelled_bytes);
    reset();tx_zero=true;
    i2s_write_locked(data,1024);
    assert(now_ms==100 && s_tx_recover && s_tx_errors==1 && s_tx_cancelled_bytes==1024);
    reset();tx_limit=256;cancel_on_send=1;gate_on_send=true;
    i2s_write_locked(data,1024);
    assert(sent_len==256 && !s_tx_recover && s_tx_cancelled_bytes==768);
    reset();tx_limit=137;cancel_on_send=1;
    i2s_write_locked(data,1024);
    assert(sent_len==137 && s_tx_recover && s_tx_cancelled_bytes==887);
    reset();tx_fail=1;tx_limit=256;
    i2s_write_locked(data,1024);
    assert(sent_len==256 && s_tx_recover && s_tx_cancelled_bytes==768);
    reset();ring_fill=ring_capacity;cancel_on_send=1;
    audio_out_write(data,sizeof data);
    assert(now_ms==10 && sent_len==0 && s_pcm_cancelled_frames==480 && !s_dropped);
    reset();ring_fill=ring_capacity;
    audio_out_write(data,sizeof data);
    assert(now_ms==100 && sent_len==0 && s_dropped==3840 && s_dropped_frames==480);
    reset();ring_fill=ring_capacity;drain_per_wait=1024;
    audio_out_write(data,sizeof data);
    assert(now_ms==40 && sent_len==3840 && !memcmp(sent_data,data,sizeof data) && !s_dropped);
    /* Every callback from a packet keeps its original token after reopen. */
    reset();uint32_t token=audio_out_gate();assert(audio_out_resume(token));
    audio_out_write(data,sizeof data);audio_out_write(data,512);
    assert(!sent_len && s_pcm_cancelled_frames==544);
    sat_audio_decode_begin(4);audio_out_write(data,sizeof data);sat_audio_decode_end();
    assert(sent_len==3840 && !s_dropped);
    /* A stream event must not invalidate the pending format readiness token. */
    reset();token=audio_out_gate();audio_out_set_streaming(true);assert(audio_out_resume(token));
    reset();audio_out_set_streaming(false);audio_out_set_streaming(true);
    audio_out_write(data,512);assert(!sent_len); /* old packet after resume */
    reset();task_id=2;audio_out_write(data,512);assert(!sent_len && !sends); /* control nonblocking */
    /* Burst pacing across multiple packets: no extra ring memory or PCM loss. */
    reset();drain_per_wait=3840;
    for(unsigned i=0;i<16;i++) {sat_audio_decode_begin(16-i);audio_out_write(data,sizeof data);}
    assert(sent_len==16*sizeof data && !s_dropped && ring_fill<=ring_capacity);
    for(unsigned i=0;i<16;i++)assert(!memcmp(sent_data+i*sizeof data,data,sizeof data));
    /* What one queued packet holds: a running mean (x16, weight 1/8) of decoder frames. */
    reset();sat_audio_decode_begin(1);audio_out_write(data,sizeof data);sat_audio_decode_end();
    assert(s_frames_per_packet_q4==480*16);
    sat_audio_decode_begin(1);audio_out_write(data,1920);
    task_id=2;audio_out_write(data,1920);task_id=1; /* a control write is not the packet's */
    sat_audio_decode_end();assert(s_frames_per_packet_q4==7680-960+480);
    sat_audio_decode_begin(1);sat_audio_decode_end();
    assert(s_frames_per_packet_q4==7200); /* a packet that produced nothing leaves it alone */
    assert(audio_prefill_bytes(96000,8,24576)==9216);
    assert(audio_prefill_bytes(44100,4,24576)==2116);
    assert(audio_prefill_bytes(192000,8,24576)==12288);
    queued_packets=24;pending_event=true;
    while(pending_event) {
        pending_event=false; /* osi_thread_generic_event_handler clears before dispatch */
        unsigned before=handled_packets;
        btc_a2dp_sink_data_ready(NULL);
        assert(handled_packets-before==1); /* control work can run between every packet */
    }
    assert(!queued_packets && handled_packets==24 && freed_packets==24 && posted_events==23);
    queued_packets=24;pending_event=true;flush_during_decode=true;
    while(pending_event) {pending_event=false;btc_a2dp_sink_data_ready(NULL);}
    assert(!queued_packets && handled_packets==25 && freed_packets==48);
    /* Actual accepted silence, including retry prefixes, not requested bytes. */
    reset();tx_limit=137;i2s_write_silence_locked(data,1024);
    assert(s_events.zero_frames==128 && s_events.zero_writes==1 && s_events.zero_last_us==now_ms*1000);
    assert(s_events.zero_gate==2 && s_events.zero_epoch==0 && !s_events.zero_cancel_frames);
    reset();tx_limit=256;cancel_on_send=1;i2s_write_silence_locked(data,1024);
    assert(s_events.zero_frames==32 && s_events.zero_writes==1 && s_events.zero_cancel_frames==32);
    assert(s_events.zero_epoch==0 && s_play_epoch==1); /* accepted prefix belongs to old generation */
    reset();s_streaming=false;i2s_write_silence_locked(data,1024);
    assert(!s_events.zero_frames && !s_events.zero_writes); /* idle clocks are not streaming zeros */
    reset();s_gate=3;s_writer_gate=3;i2s_write_silence_locked(data,1024);
    assert(!s_events.zero_frames); /* closed format gate */
    reset();tx_zero=true;i2s_write_silence_locked(data,1024);
    assert(!s_events.zero_frames && !s_events.zero_writes); /* failed write accepted nothing */
    reset();now_ms=17;audio_out_write(data,512);
    assert(s_events.pcm_last_us==17000);
    ring_fill=ring_capacity;audio_out_write(data,512);
    assert(s_events.pcm_last_us==17000); /* failed enqueue must not look like arrival */
    /* Pair begin/end across timestamp wrap, preserve cancellation's old generation. */
    audio_event_t event;
    memset(&s_events,0,sizeof s_events);s_events.pcm_last_us=UINT32_MAX-50;
    s_events.zero_frames=UINT32_MAX-7;
    assert(audio_event_rebuffer(&s_events,&event,UINT32_MAX-10,12,4,0,AUDIO_REBUFFER_EMPTY));
    assert(event.kind==AUDIO_EVENT_REBUFFER_BEGIN && (uint32_t)event.b==UINT32_MAX-50);
    assert(!audio_event_rebuffer(&s_events,&event,UINT32_MAX-5,12,4,3000,AUDIO_REBUFFER_STARTUP));
    audio_event_zeros(&s_events,3,12,4,8,128,true,false);
    assert(audio_event_rebuffer(&s_events,&event,10,14,5,0,AUDIO_REBUFFER_CANCELLED));
    assert(event.kind==AUDIO_EVENT_REBUFFER_END && event.gate==12 && event.epoch==4);
    assert(event.a==16 && (uint32_t)(event.end_us-event.begin_us)==21);
    assert(event.c==AUDIO_REBUFFER_CANCELLED && !s_events.active);
    assert(!audio_event_rebuffer(&s_events,&event,11,14,5,0,AUDIO_REBUFFER_CANCELLED));
    assert(audio_event_rebuffer(&s_events,&event,12,14,5,0,AUDIO_REBUFFER_STARTUP));
    assert(audio_event_rebuffer(&s_events,&event,13,14,5,4608,AUDIO_REBUFFER_READY));
    assert(event.a==0 && event.b==4608 && event.begin_us==12 && event.end_us==13);
    /* Overflow is explicit; preserve oldest events and recover after ring wrap. */
    audio_event_fifo_t q={0};audio_event_t history[3];
    for(unsigned i=0;i<4;i++) {event.begin_us=i;audio_event_push(&q,history,3,event);}
    assert(q.count==3 && q.overflow==1);
    for(unsigned i=0;i<3;i++) {assert(audio_event_pop(&q,history,3,&event));assert(event.begin_us==i);}
    assert(!audio_event_pop(&q,history,3,&event));
    for(unsigned i=0;i<20;i++) {
        event.begin_us=i+100;audio_event_push(&q,history,3,event);
        assert(audio_event_pop(&q,history,3,&event) && event.begin_us==i+100);
    }
    assert(q.overflow==1);
    /* Execute the production RTOS adapter with the production refill helper. */
    reset();refill_bytes=3;audio_refill_result_t refill=audio_wait_refill(2,0);
    assert(refill.status==AUDIO_REFILL_RECOVERED && refill.item==refill_data && refill.len==3);
    assert(refill_receives==1 && !refill_returns && s_wait_recovered==1 && s_empty_polls==1);
    assert(s_wait_max_us==10000 && !s_wait_timeout && !s_wait_cancel);
    vRingbufferReturnItem(s_ring,refill.item); /* partial frame belongs to normal carry path */
    reset();refill=audio_wait_refill(2,0);
    assert(refill.status==AUDIO_REFILL_TIMEOUT && !refill.item && !refill.len);
    assert(s_wait_timeout==1 && refill_receives==1 && !s_wait_recovered && !s_wait_cancel);
    for(unsigned reason=1;reason<=5;reason++) {
        reset();refill_bytes=16;refill_change=reason;
        refill=audio_wait_refill(2,0);
        assert(refill.status==AUDIO_REFILL_CANCELLED && !refill.item && !refill.len);
        assert(refill_returns==1 && !refill_borrowed && s_pcm_cancelled_frames==2);
        assert(s_wait_cancel==1 && !s_wait_timeout && !s_wait_recovered);
    }
    reset();s_streaming=false;refill=audio_wait_refill(2,0);
    assert(refill.status==AUDIO_REFILL_CANCELLED && !refill_receives && !s_wait_max_us);
    reset();s_running=false;refill=audio_wait_refill(2,0);
    assert(refill.status==AUDIO_REFILL_CANCELLED && !refill_receives);
    reset();s_gate=3;refill=audio_wait_refill(3,0);
    assert(refill.status==AUDIO_REFILL_CANCELLED && !refill_receives);
    reset();refill=audio_wait_refill(4,0);
    assert(refill.status==AUDIO_REFILL_CANCELLED && !refill_receives);
    reset();refill=audio_wait_refill(2,1);
    assert(refill.status==AUDIO_REFILL_CANCELLED && !refill_receives);
    reset();refill_change=3;refill=audio_wait_refill(2,0);
    assert(refill.status==AUDIO_REFILL_CANCELLED && !refill_returns && !s_wait_timeout);
    puts("satellite audio: 17 transfer/generation/pacing/worker + 8 telemetry + 13 refill adapter scenarios PASS");
    return 0;
}
