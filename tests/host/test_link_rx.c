/* Actual link RX helpers + reassembler; the UART ring and event queue are stubs.
 * This checks what is read per event, not the driver's ISR or its timing.
 */
#include <stdio.h>
#include <string.h>
#include "harness.h"
#include "satlink/frame.h"
#include "satlink/proto.h"
int g_checks, g_failures;
#define TAG "link"
#define BOARD_LINK_UART_NUM 1
#define BOARD_LINK_RX_GPIO 16
#define ESP_LOGI(...) ((void)0)
static int s_uart_events;
static satlink_rx_t s_rx;
static struct { uint32_t rx_bytes, rx_seq_gaps, rx_bad_frames; } s_stats;
// The driver's ring buffer and the number of events still queued behind this one.
static uint8_t ring[4096];
static size_t ring_head, ring_tail;
static unsigned queued;
static unsigned commands, pings;
static int uart_read_bytes(int port, void *buf, size_t len, int ticks) {
    (void)port; (void)ticks;
    size_t n = ring_tail - ring_head;
    if (n > len) n = len;
    memcpy(buf, ring + ring_head, n); ring_head += n;
    return (int)n;
}
static unsigned uxQueueMessagesWaiting(int q) { (void)q; return queued; }
static void on_frame(void *ctx, uint8_t type, uint8_t seq, const uint8_t *payload, size_t len) {
    (void)ctx; (void)seq; (void)payload; (void)len;
    if (type == SATLINK_MSG_CMD_TRANSPORT) commands++;
    else if (type == SATLINK_MSG_PING) pings++;
}
#include "link_rx.inc"
static void reset_case(void) {
    satlink_rx_init(&s_rx); memset(&s_stats, 0, sizeof s_stats);
    ring_head = ring_tail = 0; queued = 0; commands = pings = 0;
}
// Puts one host frame into the ring, as the ISR does before it posts the event.
static size_t arrive(uint8_t type, uint8_t seq) {
    const uint8_t payload[4] = {1, 2, 3, 4};
    size_t len = 0;
    CHECK(satlink_frame_encode(type, seq, payload, sizeof payload, ring + ring_tail,
                               sizeof ring - ring_tail, &len));
    ring_tail += len;
    return len;
}
static void deliver(size_t announced) {
    uint8_t chunk[256];
    rx_data_event(announced, chunk, sizeof chunk);
}
int main(void) {
    // Normal traffic: each event announces exactly its frame.
    reset_case(); deliver(arrive(SATLINK_MSG_PING, 0));
    CHECK(pings == 1); CHECK(ring_head == ring_tail);
    // A PING whose event was lost (queue full, or erased by xQueueReset), then a
    // CMD_TRANSPORT whose event announces only its own bytes. The command must not
    // wait in the ring for the next host transmission.
    reset_case(); arrive(SATLINK_MSG_PING, 0);
    deliver(arrive(SATLINK_MSG_CMD_TRANSPORT, 1));
    CHECK(pings == 1); CHECK(commands == 1); CHECK(ring_head == ring_tail);
    CHECK(s_stats.rx_bytes == ring_tail); CHECK(s_stats.rx_bad_frames == 0);
    // And it stays caught up: later frames are applied on their own event.
    deliver(arrive(SATLINK_MSG_CMD_TRANSPORT, 2)); CHECK(commands == 2);
    deliver(arrive(SATLINK_MSG_CMD_TRANSPORT, 3)); CHECK(commands == 3);
    CHECK(ring_head == ring_tail); CHECK(s_stats.rx_seq_gaps == 0);
    // The event of bytes that were already drained reads nothing, harmlessly.
    deliver(12); CHECK(commands == 3); CHECK(s_stats.rx_bad_frames == 0);
    // An event still queued (an overrun, say) gets the bytes past the announced
    // ones: they may sit after a hole its flush has to throw away.
    reset_case(); size_t first = arrive(SATLINK_MSG_PING, 0);
    arrive(SATLINK_MSG_CMD_TRANSPORT, 1); queued = 1; deliver(first);
    CHECK(pings == 1); CHECK(commands == 0); CHECK(ring_head == first);
    // A large burst in more than one chunk, announced short: all of it is read.
    reset_case(); size_t burst = 0;
    for (uint8_t seq = 0; seq < 40; seq++) burst += arrive(SATLINK_MSG_CMD_TRANSPORT, seq);
    CHECK(burst > 256); deliver(burst - 7);
    CHECK(commands == 40); CHECK(ring_head == ring_tail);
    printf("link RX: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
