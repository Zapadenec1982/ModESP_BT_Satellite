#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "audio_refill.h"

typedef enum { NO_CHANGE, CLOSE_GATE, NEW_GATE, NEW_EPOCH,
               STOP_STREAM, STOP_DRIVER } change_t;
typedef struct {
    uint32_t now, delay, gate, epoch, expected_gate, expected_epoch;
    bool streaming, running, deliver, borrowed;
    change_t change;
    unsigned receives, returns, guards;
    size_t bytes, expected_limit;
    uint8_t data[16];
} fixture_t;

static void change(fixture_t *f, change_t what)
{
    switch (what) {
    case NO_CHANGE: break;
    case CLOSE_GATE: f->gate |= 1u; break;
    case NEW_GATE: f->gate += 2u; break; /* reopened format still invalidates old owner */
    case NEW_EPOCH: ++f->epoch; break;
    case STOP_STREAM: f->streaming = false; break;
    case STOP_DRIVER: f->running = false; break;
    }
}
static bool current(void *ctx)
{
    fixture_t *f = ctx;
    ++f->guards;
    return f->gate == f->expected_gate && f->epoch == f->expected_epoch &&
           !(f->gate & 1u) && f->streaming && f->running;
}
static uint32_t now_us(void *ctx) { return ((fixture_t *)ctx)->now; }
static void *receive(void *ctx, size_t *len, unsigned ticks, size_t limit)
{
    fixture_t *f = ctx;
    assert(ticks == 1); /* the helper must not retry or turn ticks into milliseconds */
    assert(limit == f->expected_limit);
    assert(!f->borrowed);
    ++f->receives;
    f->now += f->delay;
    change(f, f->change); /* publication/stop may race the blocking receive */
    if (!f->deliver) { *len = 777; return NULL; } /* NULL length must not escape */
    assert(f->bytes > 0 && f->bytes <= limit);
    *len = f->bytes;
    f->borrowed = true;
    return f->data;
}
static void return_item(void *ctx, void *item)
{
    fixture_t *f = ctx;
    assert(item == f->data);
    assert(f->borrowed); /* duplicate return is a test failure */
    f->borrowed = false;
    ++f->returns;
}
static fixture_t fixture(void)
{
    fixture_t f;
    memset(&f, 0, sizeof f);
    f.gate = f.expected_gate = 12;
    f.epoch = f.expected_epoch = 7;
    f.running = f.streaming = true;
    f.bytes = 3; /* byte-ring chunk deliberately splits a 32-bit stereo frame */
    f.expected_limit = 1024;
    return f;
}
static audio_refill_result_t run(fixture_t *f)
{
    const audio_refill_ops_t ops = {f, current, now_us, receive, return_item};
    return audio_refill_once(&ops, f->expected_limit);
}

static void recovered_timeline(void)
{
    for (unsigned delay = 0; delay <= 12000; delay += 3000) {
        fixture_t f = fixture();
        f.deliver = true;
        f.delay = delay;
        audio_refill_result_t r = run(&f);
        assert(r.status == AUDIO_REFILL_RECOVERED);
        assert(r.item == f.data && r.len == 3);
        assert(r.elapsed_us == delay);
        assert(f.receives == 1 && f.guards == 2 && f.returns == 0);
        assert(f.borrowed);
        return_item(&f, r.item); /* only the consumer returns recovered data */
        assert(f.returns == 1 && !f.borrowed);
    }
}
static void cancel_before_wait(void)
{
    for (change_t reason = CLOSE_GATE; reason <= STOP_DRIVER; ++reason) {
        fixture_t f = fixture();
        f.deliver = true;
        change(&f, reason);
        audio_refill_result_t r = run(&f);
        assert(r.status == AUDIO_REFILL_CANCELLED);
        assert(!r.item && !r.len && !r.elapsed_us);
        assert(f.receives == 0 && f.guards == 1 && f.returns == 0);
    }
}
static void cancel_during_wait(void)
{
    for (change_t reason = CLOSE_GATE; reason <= STOP_DRIVER; ++reason) {
        for (int deliver = 0; deliver <= 1; ++deliver) {
            fixture_t f = fixture();
            f.change = reason;
            f.deliver = deliver != 0;
            f.delay = 8000;
            audio_refill_result_t r = run(&f);
            assert(r.status == AUDIO_REFILL_CANCELLED); /* cancellation wins over timeout */
            assert(!r.item && !r.len && r.elapsed_us == 8000);
            assert(f.receives == 1 && f.guards == 2);
            assert(f.returns == (unsigned)deliver && !f.borrowed);
        }
    }
}
static void repeated_timeouts(void)
{
    fixture_t f = fixture();
    f.delay = 10000; /* actual satellite configuration: 100 Hz RTOS tick */
    for (unsigned attempt = 1; attempt <= 4; ++attempt) {
        audio_refill_result_t r = run(&f);
        assert(r.status == AUDIO_REFILL_TIMEOUT);
        assert(!r.item && !r.len && r.elapsed_us == 10000);
        assert(f.receives == attempt); /* exactly one attempt per call, never a loop */
        assert(f.guards == attempt * 2 && f.returns == 0);
    }
    f.deliver = true; /* a later caller-owned attempt may recover normally */
    audio_refill_result_t r = run(&f);
    assert(r.status == AUDIO_REFILL_RECOVERED && r.len == f.bytes);
    return_item(&f, r.item);
    assert(f.receives == 5 && f.returns == 1);
}
static void timer_wrap(void)
{
    fixture_t f = fixture();
    f.now = UINT32_MAX - 99u;
    f.delay = 10000;
    audio_refill_result_t r = run(&f);
    assert(r.status == AUDIO_REFILL_TIMEOUT && r.elapsed_us == 10000);
    f.now = UINT32_MAX - 99u;
    f.deliver = true;
    f.change = NEW_EPOCH;
    r = run(&f);
    assert(r.status == AUDIO_REFILL_CANCELLED && r.elapsed_us == 10000);
    assert(f.returns == 1 && !f.borrowed);
}
int main(void)
{
    recovered_timeline();
    cancel_before_wait();
    cancel_during_wait();
    repeated_timeouts();
    timer_wrap();
    puts("audio_refill: 5 scenario groups PASS (27 timed/cancellation attempts)");
    return 0;
}
