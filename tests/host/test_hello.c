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
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static int s_hello_mux;
static atomic_bool s_host_present, s_peer_incompatible;
static uint32_t s_host_caps, s_own_caps, s_ping_outstanding;
static int64_t s_last_hello_us, clock_us;
static struct { unsigned rx_bad, hello_sent, pings_unanswered; } s_stats;
static struct { void (*on_hello)(const satlink_hello_t *); } s_handlers;
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
#include "hello_handlers.inc"
static void reset_case(void) {
    s_hello_mux = 0;
    s_host_present = s_peer_incompatible = false;
    s_host_caps = s_ping_outstanding = 0;
    s_last_hello_us = 0; clock_us = 1000;
    memset(&s_stats, 0, sizeof s_stats);
    hellos = pings = callbacks = 0; s_handlers.on_hello = on_hello;
}
static void receive_hello(unsigned major, unsigned minor, uint32_t caps) {
    satlink_hello_t h = {.major=major, .minor=minor, .caps=caps};
    uint8_t payload[LINK_BUF_HELLO];
    size_t len = satlink_enc_hello(payload, sizeof payload, &h);
    CHECK(len != 0); handle_hello(payload, len);
}
static void beat(void) {
    clock_us += 1000000;
    // Successful PONGs do not mean we received the peer's capabilities.
    s_ping_outstanding = 0; ping_tick(NULL);
}
int main(void) {
    reset_case(); answer_hello(); CHECK(hellos == 1);
    // Drop the initial greeting and the next reply; a live PING/PONG link remains.
    beat(); CHECK(hellos == 2); CHECK(!s_host_present);
    beat(); CHECK(hellos == 3); CHECK(!s_host_present);
    receive_hello(1, 3, SATLINK_HOST_CAP_RELOCK | SATLINK_HOST_CAP_WIDE | FMT_HOST_CAP);
    CHECK(s_host_present); CHECK(s_host_caps == 0x19); CHECK(callbacks == 1);
    CHECK(hellos == 3); // no immediate HELLO echo loop
    for (int i=0; i<30; i++) beat();
    CHECK(hellos == 3); CHECK(pings == 32);
    // Late host, damaged response: keep retrying, never infer capabilities.
    reset_case(); answer_hello();
    for (int i=0; i<5; i++) beat();
    CHECK(hellos == 6); handle_hello(NULL, 0); CHECK(s_stats.rx_bad == 1);
    beat(); CHECK(hellos == 7);
    receive_hello(1, 2, SATLINK_HOST_CAP_WIDE | FMT_HOST_CAP);
    CHECK(s_host_present); CHECK(s_host_caps == SATLINK_HOST_CAP_WIDE);
    beat(); CHECK(hellos == 7);
    // A known incompatible host is not a lost greeting; do not retry forever.
    reset_case(); answer_hello(); receive_hello(2, 0, 0);
    CHECK(s_peer_incompatible); CHECK(!s_host_present); CHECK(callbacks == 0);
    beat(); CHECK(hellos == 1); CHECK(pings == 1);
    // Reply and retry share the same rate limiter.
    reset_case(); answer_hello(); clock_us += 999999; answer_hello();
    CHECK(hellos == 1); clock_us++; answer_hello(); CHECK(hellos == 2);
    printf("HELLO recovery: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
