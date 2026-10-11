#include "satlink/cover_wire.h"
/* Actual link handlers + codecs; clock, UART queue, logging and lock are stubs.
 * This checks greeting recovery, not FreeRTOS scheduling or physical UART loss.
 */
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "harness.h"
#include "satlink/messages.h"
#include "satlink/format_sync.h"
int g_checks, g_failures;
#define LINK_BUILD_MAX 80
#define LINK_BUF_HELLO (1 + 1 + 4 + 2 + LINK_BUILD_MAX)
#define LINK_HELLO_MIN_GAP_US (1000 * 1000)
#define LINK_HELLO_RETRY_SLACK_US (50 * 1000)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static int s_hello_mux;
static atomic_bool s_host_present, s_peer_incompatible;
static uint32_t s_host_caps, s_own_caps, s_ping_outstanding;
static uint8_t s_host_minor;
static int64_t s_last_hello_us, clock_us;
static struct { unsigned rx_bad, hello_sent, rx_echoes, pings_unanswered; } s_stats;
static struct {
    void (*on_hello)(const satlink_hello_t *);
    uint32_t (*on_host_caps)(uint32_t);
} s_handlers;
typedef struct { char version[32], idf_ver[32]; } esp_app_desc_t;
static const esp_app_desc_t *esp_app_get_description(void) {
    static const esp_app_desc_t app = {"test", "5.5.5"}; return &app;
}
static int64_t esp_timer_get_time(void) { return clock_us; }
static unsigned hellos, pings, callbacks;
static void report_stats(void) {}
static void tx_push(uint8_t type, const uint8_t *payload, size_t len) {
    if (type == SATLINK_MSG_HELLO) {
        satlink_hello_t h;
        CHECK(satlink_dec_hello(payload, len, &h));
        CHECK(h.major == SATLINK_PROTO_MAJOR); hellos++;
    } else if (type == SATLINK_MSG_PING) pings++;
}
static void on_hello(const satlink_hello_t *h) { (void)h; callbacks++; }
// Stands in for the I2S role switch: device bit 8 follows host bit 8.
static uint32_t own_caps_for(uint32_t host_caps) { return host_caps & SATLINK_HOST_CAP_I2S_MASTER; }
#include "hello_handlers.inc"
static void reset_case(void) {
    s_hello_mux = 0;
    s_host_present = s_peer_incompatible = false;
    s_host_caps = s_ping_outstanding = 0; s_host_minor = 0;
    s_last_hello_us = 0; clock_us = 1000;
    memset(&s_stats, 0, sizeof s_stats);
    hellos = pings = callbacks = 0; s_handlers.on_hello = on_hello;
    s_handlers.on_host_caps = NULL;
}
static void receive_hello(unsigned major, unsigned minor, uint32_t caps) {
    satlink_hello_t h = {.major=major, .minor=minor, .caps=caps};
    uint8_t payload[LINK_BUF_HELLO];
    size_t len = satlink_enc_hello(payload, sizeof payload, &h);
    CHECK(len != 0); handle_hello(payload, len);
}
// The ping timer ticks on a fixed grid but is seen with jitter: alternate a
// tick seen 10 us late with one seen 10 us early, as esp_timer dispatch does.
static void beat(void) {
    static int late;
    late = !late;
    clock_us += late ? 1000010 : 999990;
    // Successful PONGs do not mean we received the peer's capabilities.
    s_ping_outstanding = 0; ping_tick(NULL);
}
int main(void) {
    reset_case(); answer_hello(false); CHECK(hellos == 1);
    // Drop the initial greeting and the next reply; a live PING/PONG link remains.
    beat(); CHECK(hellos == 2); CHECK(!s_host_present);
    beat(); CHECK(hellos == 3); CHECK(!s_host_present);
    receive_hello(1, 3, SATLINK_HOST_CAP_RELOCK | SATLINK_HOST_CAP_WIDE | FMT_HOST_CAP);
    CHECK(s_host_present); CHECK(s_host_caps == 0x19); CHECK(callbacks == 1);
    CHECK(hellos == 3); // no immediate HELLO echo loop
    for (int i=0; i<30; i++) beat();
    CHECK(hellos == 3); CHECK(pings == 32);
    // Late host, damaged response: keep retrying, never infer capabilities.
    reset_case(); answer_hello(false);
    for (int i=0; i<5; i++) beat();
    CHECK(hellos == 6); handle_hello(NULL, 0); CHECK(s_stats.rx_bad == 1);
    beat(); CHECK(hellos == 7);
    receive_hello(1, 2, SATLINK_HOST_CAP_WIDE | FMT_HOST_CAP);
    CHECK(s_host_present); CHECK(s_host_caps == SATLINK_HOST_CAP_WIDE);
    beat(); CHECK(hellos == 7);
    // 96 kHz needs protocol 1.4 and both transition prerequisites.
    receive_hello(1, 3, 0x39); CHECK(s_host_caps == 0x19);
    receive_hello(1, 4, 0x39); CHECK(s_host_caps == 0x39);
    receive_hello(1, 4, 0x29); CHECK(s_host_caps == 0x09);
    receive_hello(1, 4, 0x38); CHECK(s_host_caps == 0x18);
    // Track-scoped covers must never be sent to a legacy cover consumer.
    receive_hello(1, 4, 0x3b); CHECK(s_host_caps == 0x39);
    receive_hello(1, 5, 0x3b); CHECK(s_host_caps == 0x3b);
    receive_hello(1, 5, 0x39); CHECK(s_host_caps == 0x39);
    // Streaming requires both the new minor and explicit cover capability.
    receive_hello(1, 5, 0x7b); CHECK(s_host_caps == 0x3b);
    receive_hello(1, 6, 0x7b); CHECK(s_host_caps == 0x7b);
    receive_hello(1, 6, 0x79); CHECK(s_host_caps == 0x39);
    // Host-confirmed pairing is a 1.8 bit; the minor is kept for the 1.8 senders.
    receive_hello(1, 7, 0xfb); CHECK(s_host_caps == 0x7b); CHECK(s_host_minor == 7);
    receive_hello(1, 8, 0xfb); CHECK(s_host_caps == 0xfb); CHECK(s_host_minor == 8);
    receive_hello(1, 8, 0x80); CHECK(s_host_caps == 0x80);
    receive_hello(1, 9, 0xfb); CHECK(s_host_caps == 0xfb); CHECK(s_host_minor == 9);
    receive_hello(1, 6, 0xfb); CHECK(s_host_caps == 0x7b); CHECK(s_host_minor == 6);
    // Our own greeting heard back off a floating wire is not a host: nothing
    // changes, and above all our BT_SETTINGS is not taken for PAIR_CONFIRM.
    reset_case(); s_own_caps = 0xdf; answer_hello(false); CHECK(hellos == 1);
    uint8_t own[LINK_BUF_HELLO];
    size_t own_len = encode_own_hello(own, sizeof own);
    CHECK(own_len != 0);
    handle_hello(own, own_len);
    CHECK(!s_host_present); CHECK(s_host_caps == 0); CHECK(s_host_minor == 0);
    CHECK(callbacks == 0); CHECK(s_stats.rx_echoes == 1); CHECK(s_stats.rx_bad == 0);
    beat(); CHECK(hellos == 2);                 // still greeting: no host has answered
    // A real host with the very same caps but its own build is still a host.
    receive_hello(1, 8, 0xdf); CHECK(s_host_present); CHECK(s_host_caps == 0xdf);
    s_own_caps = 0;
    // A known incompatible host is not a lost greeting; do not retry forever.
    reset_case(); answer_hello(false); receive_hello(2, 0, 0);
    CHECK(s_peer_incompatible); CHECK(!s_host_present); CHECK(callbacks == 0);
    beat(); CHECK(hellos == 1); CHECK(pings == 1);
    // 1.9: a HELLO that changes our caps is answered at once, past the rate limit.
    reset_case(); s_handlers.on_host_caps = own_caps_for; answer_hello(false);
    receive_hello(1, 9, 0x1fb); CHECK(s_host_caps == 0x1fb); CHECK(s_own_caps == 0x100);
    CHECK(hellos == 2);
    receive_hello(1, 9, 0x1fb); CHECK(hellos == 2);  // unchanged: rate limited
    receive_hello(1, 8, 0x1fb); CHECK(s_host_caps == 0xfb); CHECK(s_own_caps == 0);
    CHECK(hellos == 3);
    // No host: one retry per tick, even when a tick is seen a little early.
    reset_case(); answer_hello(false);
    for (int i=0; i<10; i++) beat();
    CHECK(hellos == 11); CHECK(pings == 10);
    // The slack is for jitter only: a tick far short of a second is refused.
    reset_case(); answer_hello(false); clock_us += 900000; ping_tick(NULL);
    CHECK(hellos == 1); CHECK(pings == 1);
    // A host HELLO inside the second after ours, caps unchanged, gets no reply.
    reset_case(); answer_hello(false); clock_us += 990000;
    receive_hello(1, 9, 0xfb); CHECK(s_host_present); CHECK(hellos == 1);
    // Reply and retry share the same rate limiter.
    reset_case(); answer_hello(false); clock_us += 999999; answer_hello(false);
    CHECK(hellos == 1); clock_us++; answer_hello(false); CHECK(hellos == 2);
    printf("HELLO recovery: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
