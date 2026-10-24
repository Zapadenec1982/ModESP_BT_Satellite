#include "link.h"
#include "link_policy.h"
#include "log_line.h"
#include "tx_slots.h"
#include "satlink/cover_wire.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "board.h"

#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "satlink/cobs.h"
#include "satlink/frame.h"

static const char *TAG = "sat.link";

/* Four frames rather than two: at 460800 baud this is about ninety milliseconds
 * of wire, which is the margin the receiver has if something delays it. */
#define LINK_RX_BUF          (SATLINK_WIRE_MAX * 4)
/* IDF 5.5.5 uart_tx_all spins when a buffered TX ring has no free bytes.
 * With no driver TX ring it waits on the FIFO semaphore instead. Only our
 * dedicated sender waits; Bluetooth callbacks still enqueue without blocking. */
#define LINK_TX_BUF          0
#define LINK_READ_CHUNK      256
#define LINK_EVENT_QUEUE     16

/* Symbol periods of idle before the hardware calls a burst finished. Ten of them
 * at 460800 is about 217 us: no delay a host command would notice, and long
 * enough not to split a frame in two. */
#define LINK_RX_TIMEOUT_SYM  10

/* PROTOCOL.md 13 divides the device's messages into those that may be thrown away
 * when the host stops reading and those that may not. What may not waits in slots
 * (main/tx_slots.h, below); what may, in two queues. The slots drain first, which
 * is what keeps BT_STATE ahead of a backlog of TRACK_POS. */
#define LINK_Q_TELEMETRY     10      /* TRACK_POS and LOG: when full, the oldest goes */
#define LINK_Q_COVER          6      /* cover parts: when full, the new one is refused */
/* With nothing to send the sender sleeps until a producer wakes it; this only
 * bounds the sleep, in case a wake-up is ever missed. */
#define LINK_TX_IDLE_MS     100

#define LINK_TASK_STACK      3584
/*
 * The receiver runs above the audio path, and that is deliberate.
 *
 * It used to sit at configMAX-6, below the I2S writer at -3 and the A2DP handler
 * at -4, so while music played it was the last thing the scheduler got to. Its
 * work is tiny -- drain a UART and reassemble -- but the driver's buffer holds
 * only about forty-five milliseconds of wire at 460800 baud, and forty-five
 * milliseconds is nothing when a codec is busy. The bytes went in the bin, the
 * host saw a quarter of its traffic vanish, and the counter that should have said
 * so was being overwritten.
 *
 * Short bounded work at high priority is the right shape for draining an
 * interface. It blocks on the driver's event queue and runs only when there is
 * something to take, so it cannot starve anything by spinning.
 *
 * The sender can wait: a late PONG costs latency, a late read costs data.
 */
#define LINK_RX_PRIO         (configMAX_PRIORITIES - 2)
#define LINK_TX_PRIO         (configMAX_PRIORITIES - 5)

#define LINK_PING_PERIOD_US  1000000

/* A HELLO answers a HELLO, but no faster than this. Two polite implementations
 * with no limit would greet each other until the wire melted. */
#define LINK_HELLO_MIN_GAP_US (1000 * 1000)

/* The retry timer ticks on a fixed 1 s grid (esp_timer re-arms with
 * alarm += period), but each tick is seen a few microseconds early or late.
 * Measured against the strict gap, every tick that came in a little earlier
 * than the last one was refused, and the once-a-second retry became 1 s or 2 s
 * at random. The timer path alone gets this much slack. */
#define LINK_HELLO_RETRY_SLACK_US (50 * 1000)

/*
 * How much stack a sender is allowed to take.
 *
 * These were SATLINK_PAYLOAD_MAX, all 1036 bytes of it, and that overflowed the
 * main task on the first boot: the log bridge runs in whoever happens to be
 * logging, and app_main calling into a sender that allocates a kilobyte is the
 * end of a 3584-byte stack. A message gets room for what it can actually carry
 * and no more. Cover parts are the one thing that needs the full payload, and
 * link_try_cover_stream builds those on the heap, one frame at a time.
 */
#define LINK_NAME_MAX      64        /* a Bluetooth device name */
#define LINK_META_MAX     128        /* one of title, artist, album */
#define LINK_LOG_MAX      224        /* one log line */
#define LINK_BUILD_MAX     80        /* "sat <version> <idf>" */

#define LINK_BUF_BT_STATE  (1 + 1 + 6 + 2 + LINK_NAME_MAX)
#define LINK_BUF_META      (3 * (2 + LINK_META_MAX))
#define LINK_BUF_LOG       (1 + 2 + LINK_LOG_MAX)
#define LINK_BUF_HELLO     (1 + 1 + 4 + 2 + LINK_BUILD_MAX)
#define LINK_BUF_PAIR_REQ  (6 + 1 + 4 + 2 + SATLINK_BT_NAME_MAX)
#define LINK_BUF_BT_CONFIG (5 + 2 + SATLINK_BT_NAME_MAX)
/* PING, PONG, STREAM_FMT, STREAM_PREPARE, VOLUME_STATE, COVER_RESET and
 * PAIR_RESULT: 14 bytes at most, STREAM_PREPARE. */
#define LINK_BUF_SMALL     16
/* The largest slot, which is what the sender copies a slot out into. */
#define LINK_SLOT_MAX      SATLINK_BOND_LIST_BYTES_MAX

/* A queued payload, not a frame. Sequence numbers are stamped at the moment of
 * transmission rather than of queueing, because there are several queues and the
 * sender drains one before another: numbering at queue time puts frames on the
 * wire out of order, and the receiver's gap counter then reports losses that
 * never happened. Found exactly that way, in loopback, counting 256 phantom gaps
 * every ten seconds. */
typedef struct {
    uint8_t *payload;
    uint16_t len;
    uint8_t  type;
} tx_item_t;

/*
 * The slots: one per message type that must not be lost. Static, so no state of
 * the heap can lose a BT_STATE: under three kilobytes with the sender's copy
 * buffer, the price of PROTOCOL.md 13's "the rest is not dropped".
 */
static uint8_t s_buf_hello[LINK_BUF_HELLO];
static uint8_t s_buf_bt_state[LINK_BUF_BT_STATE];
static uint8_t s_buf_meta[LINK_BUF_META];
static uint8_t s_buf_pair_req[LINK_BUF_PAIR_REQ];
static uint8_t s_buf_bt_config[LINK_BUF_BT_CONFIG];
static uint8_t s_buf_bond_list[SATLINK_BOND_LIST_BYTES_MAX];
static uint8_t s_buf_small[7][LINK_BUF_SMALL];

#define SLOT(t, b)  { .type = (t), .cap = (uint16_t)sizeof(b), .buf = (b) }
static tx_slot_t s_slots[] = {
    SLOT(SATLINK_MSG_HELLO,        s_buf_hello),
    SLOT(SATLINK_MSG_BT_STATE,     s_buf_bt_state),
    SLOT(SATLINK_MSG_TRACK_META,   s_buf_meta),
    SLOT(SATLINK_MSG_PAIR_REQUEST, s_buf_pair_req),
    SLOT(SATLINK_MSG_BT_CONFIG,    s_buf_bt_config),
    SLOT(SATLINK_MSG_BOND_LIST,    s_buf_bond_list),
    SLOT(SATLINK_MSG_PING,         s_buf_small[0]),
    SLOT(SATLINK_MSG_PONG,         s_buf_small[1]),
    SLOT(SATLINK_MSG_STREAM_FMT,   s_buf_small[2]),
    SLOT(FMT_PREPARE,              s_buf_small[3]),
    SLOT(SATLINK_MSG_VOLUME_STATE, s_buf_small[4]),
    SLOT(COVER_RESET,              s_buf_small[5]),
    SLOT(SATLINK_MSG_PAIR_RESULT,  s_buf_small[6]),
};
#undef SLOT
#define LINK_SLOTS (sizeof(s_slots) / sizeof(s_slots[0]))
_Static_assert(LINK_SLOTS == 6 + sizeof(s_buf_small) / sizeof(s_buf_small[0]),
               "the slot table and its storage disagree");
_Static_assert(LINK_SLOT_MAX >= LINK_BUF_META && LINK_SLOT_MAX >= LINK_BUF_HELLO,
               "LINK_SLOT_MAX is not the largest slot");

static portMUX_TYPE      s_slot_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t          s_slot_stamp;
static TaskHandle_t      s_tx_task;

static QueueHandle_t     s_uart_events;
static QueueHandle_t     s_q_telemetry;
static QueueHandle_t     s_q_cover;
static link_handlers_t   s_handlers;
static satlink_rx_t      s_rx;
static link_stats_t      s_stats;
static uint8_t           s_tx_seq;
static atomic_bool       s_host_present;
static atomic_bool       s_peer_incompatible;
static uint32_t          s_host_caps;
static uint8_t           s_host_minor;
static portMUX_TYPE s_fmt_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_fmt_nonce, s_fmt_generation, s_fmt_sent_ms;
static uint32_t          s_own_caps;
static uint32_t          s_ping_outstanding;
static int64_t           s_last_hello_us;
static portMUX_TYPE s_hello_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_ping_timer;

/* Guards the log bridge against feeding on itself: a failure inside a send would
 * otherwise log, which would send, which would fail. Per task: the hook runs
 * unlocked from every task on both cores, and a shared flag dropped another
 * task's line whenever two logged at once. */
static __thread bool     s_in_log_bridge;
static vprintf_like_t    s_prev_vprintf;

/* ------------------------------------------------------------------ outbound */

/* TRACK_POS, LOG and COVER are the three the spec names as droppable. The 1.6
 * cover parts and their END are COVER by another name. */
static bool type_is_droppable(uint8_t type)
{
    return type == SATLINK_MSG_TRACK_POS ||
           type == SATLINK_MSG_LOG ||
           type == SATLINK_MSG_COVER ||
           type == COVER_DATA || type == COVER_END;
}

static void tx_item_free(tx_item_t *item)
{
    free(item->payload);
    item->payload = NULL;
}

static void tx_wake(void)
{
    if (s_tx_task != NULL) {
        xTaskNotifyGive(s_tx_task);
    }
}

static void tx_slot_count(tx_slot_result_t r)
{
    switch (r) {
    case TX_SLOT_REPLACED:
        s_stats.tx_coalesced++;
        break;
    case TX_SLOT_NO_SLOT:
    case TX_SLOT_TOO_LONG:
        s_stats.tx_dropped_critical++;
        break;
    case TX_SLOT_QUEUED:
        break;
    }
}

/*
 * A droppable message. When the queue is full the OLDEST goes, DEVICE_SPEC rule 1:
 * a TRACK_POS is superseded within the second and a log line the console already
 * printed. Cover parts have their own queue and are never evicted for these --
 * that used to hole images for the sake of a log line, and was why this queue
 * dropped the newest instead.
 */
static void tx_queue_telemetry(uint8_t type, const uint8_t *payload, size_t len)
{
    uint8_t *buf = malloc(len != 0 ? len : 1);
    if (buf == NULL) {
        s_stats.tx_dropped_bulk++;      /* counted, which it used not to be */
        return;
    }
    if (len != 0) {
        memcpy(buf, payload, len);
    }
    tx_item_t item = { .payload = buf, .len = (uint16_t)len, .type = type };
    /* Bounded: another producer can refill the place this one freed. */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (xQueueSend(s_q_telemetry, &item, 0) == pdTRUE) {
            tx_wake();
            return;
        }
        tx_item_t oldest;
        if (xQueueReceive(s_q_telemetry, &oldest, 0) == pdTRUE) {
            tx_item_free(&oldest);
            s_stats.tx_dropped_bulk++;
        }
    }
    tx_item_free(&item);
    s_stats.tx_dropped_bulk++;
}

static void tx_push_tagged(uint8_t type, const uint8_t *payload, size_t payload_len,
                           uint32_t nonce, uint32_t generation)
{
    if (s_q_telemetry == NULL) {
        return;
    }
    /* An incompatible peer gets liveness and log lines and nothing else: it would
     * not understand the rest, and guessing is worse than silence. */
    if (s_peer_incompatible && !link_incompatible_allows(type)) {
        return;
    }
    if (payload_len > SATLINK_PAYLOAD_MAX) {
        return;     /* the encoders already refuse oversize; nothing to say here */
    }

    if (type_is_droppable(type)) {
        tx_queue_telemetry(type, payload, payload_len);
        return;
    }

    /* A copy into the type's slot and nothing more. Framing happens in the sender,
     * so nothing frame-sized ever lands on the caller's stack -- and the caller can
     * be any task at all. A spinlock rather than a mutex: the copy is a few hundred
     * bytes at worst, and Bluetooth callbacks must not wait. */
    portENTER_CRITICAL(&s_slot_mux);
    tx_slot_result_t r = tx_slot_put(s_slots, LINK_SLOTS, type, payload, payload_len,
                                     nonce, generation, &s_slot_stamp);
    portEXIT_CRITICAL(&s_slot_mux);
    tx_slot_count(r);
    tx_wake();
}

static void tx_push(uint8_t type, const uint8_t *payload, size_t len)
{
    tx_push_tagged(type, payload, len, 0, 0);
}

static void link_tx_task(void *arg)
{
    (void)arg;
    /* Framed here, in transmission order, and into buffers only this task touches. */
    static uint8_t slot_out[LINK_SLOT_MAX];
    static uint8_t wire[SATLINK_WIRE_MAX];

    for (;;) {
        uint8_t type = 0;
        uint16_t len = 0;
        uint32_t nonce = 0, generation = 0;
        const uint8_t *payload = NULL;
        tx_item_t item = { 0 };

        portENTER_CRITICAL(&s_slot_mux);
        bool got = tx_slot_take(s_slots, LINK_SLOTS, slot_out, sizeof(slot_out),
                                &type, &len, &nonce, &generation);
        portEXIT_CRITICAL(&s_slot_mux);
        if (got) {
            payload = slot_out;
        } else if (xQueueReceive(s_q_telemetry, &item, 0) == pdTRUE ||
                   xQueueReceive(s_q_cover, &item, 0) == pdTRUE) {
            type = item.type;
            len = item.len;
            payload = item.payload;
        } else {
            /* Nothing waiting. Every producer gives a notification, so a new message
             * wakes us at once rather than after a poll interval. */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LINK_TX_IDLE_MS));
            continue;
        }

        size_t wire_len = 0;
        if (satlink_frame_encode(type, s_tx_seq, payload, len, wire, sizeof(wire), &wire_len)) {
            s_tx_seq++;
            /* There is no flow control on this link, so the transmitter always
             * drains and this wait is bounded by the baud rate. The slots and
             * queues in front are what keep callers from ever waiting at all. */
            int sent = uart_write_bytes(BOARD_LINK_UART_NUM, wire, wire_len);
            if (sent == (int)wire_len) {
                s_stats.tx_frames++;
                if (generation &&
                    uart_wait_tx_done(BOARD_LINK_UART_NUM, pdMS_TO_TICKS(100)) == ESP_OK) {
                    portENTER_CRITICAL(&s_fmt_mux);
                    if (s_fmt_nonce != nonce || s_fmt_generation != generation) {
                        s_fmt_nonce = nonce;
                        s_fmt_generation = generation;
                        s_fmt_sent_ms = (uint32_t)(esp_timer_get_time() / 1000);
                    }
                    portEXIT_CRITICAL(&s_fmt_mux);
                }
            }
        }
        tx_item_free(&item);
    }
}

/* ------------------------------------------------------------------- senders */

void link_send_bt_state(uint8_t state, uint8_t codec, const uint8_t addr[6], const char *name)
{
    satlink_bt_state_t_msg m = { .state = state, .codec = codec,
                                 .name = satlink_str_clip(name, LINK_NAME_MAX) };
    if (addr != NULL) { memcpy(m.addr, addr, sizeof(m.addr)); }

    uint8_t payload[LINK_BUF_BT_STATE];
    size_t len = satlink_enc_bt_state(payload, sizeof(payload), &m);
    if (len != 0) { tx_push(SATLINK_MSG_BT_STATE, payload, len); }
}

void link_send_track_meta(const char *title, const char *artist, const char *album)
{
    satlink_track_meta_t m = {
        .title  = satlink_str_clip(title, LINK_META_MAX),
        .artist = satlink_str_clip(artist, LINK_META_MAX),
        .album  = satlink_str_clip(album, LINK_META_MAX),
    };
    uint8_t payload[LINK_BUF_META];
    size_t len = satlink_enc_track_meta(payload, sizeof(payload), &m);
    if (len != 0) { tx_push(SATLINK_MSG_TRACK_META, payload, len); }
}

void link_send_track_pos(uint8_t state, uint32_t elapsed_ms, uint32_t total_ms)
{
    satlink_track_pos_t m = { .state = state, .elapsed_ms = elapsed_ms, .total_ms = total_ms };
    uint8_t payload[16];
    size_t len = satlink_enc_track_pos(payload, sizeof(payload), &m);
    if (len != 0) { tx_push(SATLINK_MSG_TRACK_POS, payload, len); }
}

void link_send_stream_fmt(uint32_t rate_hz, uint8_t bits, uint8_t channels)
{
    satlink_stream_fmt_t m = { .rate_hz = rate_hz, .bits = bits, .channels = channels };
    uint8_t payload[16];
    size_t len = satlink_enc_stream_fmt(payload, sizeof(payload), &m);
    if (len != 0) { tx_push(SATLINK_MSG_STREAM_FMT, payload, len); }
}

void link_send_format(const fmt_request_t *r, bool acknowledged)
{
    uint8_t payload[14];
    size_t len;
    uint8_t type;
    if (acknowledged) {
        fmt_encode(payload, r); len = sizeof payload; type = FMT_PREPARE;
    } else {
        satlink_stream_fmt_t legacy = { .rate_hz=r->rate_hz, .bits=r->bits, .channels=r->channels };
        len = satlink_enc_stream_fmt(payload, sizeof payload, &legacy); type = SATLINK_MSG_STREAM_FMT;
    }
    if (len) tx_push_tagged(type, payload, len, r->nonce, r->generation);
}

bool link_format_sent(const fmt_request_t *r, uint32_t *when_ms)
{
    portENTER_CRITICAL(&s_fmt_mux);
    bool match = r->nonce == s_fmt_nonce && r->generation == s_fmt_generation;
    *when_ms = s_fmt_sent_ms;
    portEXIT_CRITICAL(&s_fmt_mux);
    return match;
}

void link_send_volume_state(uint8_t volume, uint8_t source)
{
    satlink_volume_state_t m = { .volume = volume, .source = source };
    uint8_t payload[8];
    size_t len = satlink_enc_volume_state(payload, sizeof(payload), &m);
    if (len != 0) { tx_push(SATLINK_MSG_VOLUME_STATE, payload, len); }
}

void link_send_cover_reset(uint32_t nonce, uint32_t generation)
{
    /* Streaming hosts only. A 1.5 host would understand RESET, but this build
     * never follows it with an image, so it is told nothing about covers. */
    if (!(link_host_caps() & COVER_STREAM_CAP)) return;
    uint8_t p[8]; cover_put32(p, nonce); cover_put32(p+4, generation);
    tx_push(COVER_RESET, p, sizeof p);
}

// BTC callback only. Bounded, nonblocking; END shares the DATA FIFO so it cannot
// overtake data. Never evict another frame to make room for an image.
bool link_try_cover_stream(uint32_t nonce, uint32_t generation, uint32_t offset,
                           const uint8_t *data, size_t len, uint32_t crc)
{
    /* The incompatible check too: this skips tx_push and its filter, and the caps
     * are still those of the last compatible host. */
    if (s_peer_incompatible || !(link_host_caps() & COVER_STREAM_CAP) || !s_q_cover ||
        len>COVER_PART_MAX || (!data && len) ||
        link_bulk_pending()>LINK_BULK_COVER_HEADROOM) return false;
    size_t bytes=16+len;
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT)<bytes+16*1024) return false;
    uint8_t *p=malloc(bytes);
    if (!p) return false;
    cover_put32(p,nonce); cover_put32(p+4,generation);
    cover_put32(p+8,len?0:offset); cover_put32(p+12,len?offset:crc);
    if (len) memcpy(p+16,data,len);
    tx_item_t item={.payload=p,.len=(uint16_t)bytes,.type=len?COVER_DATA:COVER_END};
    if (xQueueSend(s_q_cover,&item,0)==pdTRUE) { tx_wake(); return true; }
    free(p); return false;
}

void link_send_log(uint8_t level, const char *text)
{
    satlink_log_t m = { .level = level, .text = satlink_str_clip(text, LINK_LOG_MAX) };
    uint8_t payload[LINK_BUF_LOG];
    size_t len = satlink_enc_log(payload, sizeof(payload), &m);
    if (len != 0) { tx_push(SATLINK_MSG_LOG, payload, len); }
}

/* Protocol 1.8. A host below minor 8 would count these as unknown and nothing
 * else, but it asked for none of them, so it is not sent them. */
static bool host_speaks_1_8(void) { return link_host_minor() >= 8; }

void link_send_pair_request(const satlink_pair_request_t *m)
{
    if (!host_speaks_1_8()) return;
    uint8_t payload[6 + 1 + 4 + 2 + SATLINK_BT_NAME_MAX];
    size_t len = satlink_enc_pair_request(payload, sizeof(payload), m);
    if (len != 0) { tx_push(SATLINK_MSG_PAIR_REQUEST, payload, len); }
}

void link_send_pair_result(const uint8_t addr[6], uint8_t status)
{
    if (!host_speaks_1_8()) return;
    satlink_pair_result_t m = { .status = status };
    memcpy(m.addr, addr, sizeof(m.addr));
    uint8_t payload[8];
    size_t len = satlink_enc_pair_result(payload, sizeof(payload), &m);
    if (len != 0) { tx_push(SATLINK_MSG_PAIR_RESULT, payload, len); }
}

void link_send_bt_config(const satlink_bt_config_t *m)
{
    if (!host_speaks_1_8()) return;
    uint8_t payload[5 + 2 + SATLINK_BT_NAME_MAX];
    size_t len = satlink_enc_bt_config(payload, sizeof(payload), m);
    if (len != 0) { tx_push(SATLINK_MSG_BT_CONFIG, payload, len); }
}

static size_t encode_bond_list(uint8_t *buf, size_t cap, const void *m)
{
    return satlink_enc_bond_list(buf, cap, (const satlink_bond_list_t *)m);
}

/* 658 bytes at worst, so not on the caller's stack -- and not on the heap either,
 * where a failed allocation used to lose it without a word. It is encoded
 * straight into its slot. */
void link_send_bond_list(const satlink_bond_list_t *m)
{
    if (!host_speaks_1_8() || s_q_telemetry == NULL || s_peer_incompatible) return;
    portENTER_CRITICAL(&s_slot_mux);
    tx_slot_result_t r = tx_slot_encode(s_slots, LINK_SLOTS, SATLINK_MSG_BOND_LIST,
                                        encode_bond_list, m, &s_slot_stamp);
    portEXIT_CRITICAL(&s_slot_mux);
    tx_slot_count(r);
    tx_wake();
}

/* Our greeting, byte for byte as it goes on the wire. 0 if it will not fit. */
static size_t encode_own_hello(uint8_t *payload, size_t cap)
{
    const esp_app_desc_t *app = esp_app_get_description();
    /* Both fields are 32 bytes in the descriptor, so this is sized to hold them
     * whole rather than to hold what they usually happen to contain. */
    char build[4 + sizeof(app->version) + 1 + sizeof(app->idf_ver)];
    snprintf(build, sizeof(build), "sat %.*s %.*s",
             (int)sizeof(app->version), app->version,
             (int)sizeof(app->idf_ver), app->idf_ver);

    satlink_hello_t m = {
        .major = SATLINK_PROTO_MAJOR,
        .minor = SATLINK_PROTO_MINOR,
        .caps  = __atomic_load_n(&s_own_caps, __ATOMIC_SEQ_CST),
        .build = satlink_str_clip(build, LINK_BUILD_MAX),
    };
    return satlink_enc_hello(payload, cap, &m);
}

static void send_hello(void)
{
    uint8_t payload[LINK_BUF_HELLO];
    size_t len = encode_own_hello(payload, sizeof(payload));
    if (len == 0) {
        /* Never observed, but it would have been invisible if it happened: the
         * link would simply never come up and each side would blame the other. */
        ESP_LOGE(TAG, "HELLO will not encode into %u bytes", (unsigned)sizeof(payload));
        return;
    }
    s_stats.hello_sent++;
    ESP_LOGI(TAG, "HELLO out: %u.%u, caps 0x%08" PRIx32 ", %u bytes",
             SATLINK_PROTO_MAJOR, SATLINK_PROTO_MINOR, __atomic_load_n(&s_own_caps, __ATOMIC_SEQ_CST), (unsigned)len);
    tx_push(SATLINK_MSG_HELLO, payload, len);
}

/* Every HELLO this device sends goes through here. RX, startup and retry timer
 * share one reservation (the time of the last HELLO), but not one gap: the
 * reply and startup paths ask for the full LINK_HELLO_MIN_GAP_US, the retry
 * timer for LINK_HELLO_RETRY_SLACK_US less. */
static void reserve_hello(bool force, int64_t min_gap_us)
{
    int64_t now = esp_timer_get_time();
    // Do not hold the lock while encoding, logging or queueing the message.
    portENTER_CRITICAL(&s_hello_mux);
    // `force`: our capabilities changed, and a host that keeps the old ones would be
    // driving a line we now drive too, or not driving one we now need.
    if (!force && s_last_hello_us != 0 && now - s_last_hello_us < min_gap_us) {
        portEXIT_CRITICAL(&s_hello_mux);
        return;
    }
    s_last_hello_us = now;
    portEXIT_CRITICAL(&s_hello_mux);
    send_hello();
}

/* The reply half of the greeting, also used for the startup HELLO. Without it a
 * peer that started first never learns our version or our capabilities, and is
 * right to refuse to bring the link up: it would be guessing at what we can do. */
static void answer_hello(bool force)
{
    reserve_hello(force, LINK_HELLO_MIN_GAP_US);
}

/* ------------------------------------------------------------------- inbound */

static void handle_hello(const uint8_t *payload, size_t len)
{
    /*
     * Our own greeting, come back off the wire. A receive line that nothing
     * drives picks up the transmit line beside it; on the bench, before the
     * panel's first greeting, one came back whole and passed its CRC. Taken for
     * the host's, it had this device trusting its own capabilities as the
     * host's -- and our BT_SETTINGS is the host's PAIR_CONFIRM, bit for bit, so
     * pairing went to a confirmer that was not there. A host never sends our
     * build string, so a payload identical to ours is ours.
     */
    uint8_t own[LINK_BUF_HELLO];
    size_t own_len = encode_own_hello(own, sizeof(own));
    if (own_len != 0 && own_len == len && memcmp(own, payload, len) == 0) {
        if (s_stats.rx_echoes++ == 0) {
            ESP_LOGW(TAG, "our own HELLO came back: the receive line hears the transmit line");
        }
        return;
    }

    satlink_hello_t h = {0};
    if (!satlink_dec_hello(payload, len, &h)) {
        s_stats.rx_bad++;
        return;
    }

    if (h.major != SATLINK_PROTO_MAJOR) {
        /* Different major means incompatible. The link stops carrying anything
         * but liveness and logs — and the audio keeps playing, because I2S does
         * not depend on any of this. */
        s_peer_incompatible = true;
        s_host_present = false;
        ESP_LOGE(TAG, "host speaks protocol %u.%u, we speak %u.%u — link disabled, audio unaffected",
                 h.major, h.minor, SATLINK_PROTO_MAJOR, SATLINK_PROTO_MINOR);
        /* Still answer. Two sides that both fall silent on a version mismatch
         * leave nobody able to say what the mismatch was. */
        answer_hello(false);
        return;
    }

    uint32_t caps = fmt_ack_capable(h.minor, h.caps) ? h.caps : (h.caps & ~FMT_HOST_CAP);
    if (h.minor < 4 || !(caps & FMT_HOST_CAP) || !(caps & SATLINK_HOST_CAP_RELOCK))
        caps &= ~SATLINK_HOST_CAP_96KHZ;
    if (h.minor < 5) caps &= ~SATLINK_HOST_CAP_COVER;
    if (h.minor < 6 || !(caps & SATLINK_HOST_CAP_COVER)) caps &= ~COVER_STREAM_CAP;
    if (h.minor < 8) caps &= ~SATLINK_HOST_CAP_PAIR_CONFIRM;
    /* 1.9: the host clocks the link only in 32-bit slots and only if it can relock. */
    if (h.minor < 9 || !(caps & SATLINK_HOST_CAP_RELOCK) || !(caps & SATLINK_HOST_CAP_WIDE))
        caps &= ~SATLINK_HOST_CAP_I2S_MASTER;
    /* Minor first: a sender that sees the new caps must not see the old minor. */
    __atomic_store_n(&s_host_minor, h.minor, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_host_caps, caps, __ATOMIC_SEQ_CST);
    /* Presence last: a reader that sees the host present (link_wait_for_host)
     * must also see its minor and caps. */
    s_peer_incompatible = false;
    s_host_present = true;
    ESP_LOGI(TAG, "host %u.%u, caps 0x%08" PRIx32 ", build \"%.*s\"",
             h.major, h.minor, h.caps, (int)h.build.len, h.build.ptr);

    if ((h.caps & SATLINK_HOST_CAP_RELOCK) == 0) {
        ESP_LOGI(TAG, "host cannot relock: announcing 44.1 kHz only");
    }

    /* 1.9: whatever the host's caps change about us (the I2S role) is made true
     * before we answer, and a change goes out now rather than after the rate limit. */
    bool changed = false;
    if (s_handlers.on_host_caps != NULL) {
        const uint32_t own = s_handlers.on_host_caps(caps);
        changed = own != __atomic_load_n(&s_own_caps, __ATOMIC_SEQ_CST);
        __atomic_store_n(&s_own_caps, own, __ATOMIC_SEQ_CST);
    }

    /* The spec asks for a HELLO after start and in answer to one. Only the first
     * half was here, so a host that came up second waited for ever. */
    answer_hello(changed);

    if (s_handlers.on_hello != NULL) {
        s_handlers.on_hello(&h);
    }
}

static void on_frame(void *ctx, uint8_t type, uint8_t seq,
                     const uint8_t *payload, size_t len)
{
    (void)ctx;
    (void)seq;
    s_stats.rx_frames++;

    /* One gate for every command, present and future. It used to be one check per
     * handler, and transport, volume, radio and the expander had none, so a host
     * the link refused to come up for could still drive them. */
    if (s_peer_incompatible && !link_incompatible_allows(type)) {
        s_stats.rx_refused++;
        return;
    }

    switch (type) {
    case SATLINK_MSG_HELLO:
        handle_hello(payload, len);
        break;

    case SATLINK_MSG_PING: {
        satlink_ping_t ping = {0};
        if (!satlink_dec_ping(payload, len, &ping)) { s_stats.rx_bad++; break; }
        satlink_pong_t pong = {
            .echo_ms = ping.tick_ms,
            .own_ms  = (uint32_t)(esp_timer_get_time() / 1000),
        };
        uint8_t out[16];
        size_t n = satlink_enc_pong(out, sizeof(out), &pong);
        if (n != 0) { tx_push(SATLINK_MSG_PONG, out, n); }
        break;
    }

    case SATLINK_MSG_PONG: {
        satlink_pong_t pong = {0};
        if (!satlink_dec_pong(payload, len, &pong)) { s_stats.rx_bad++; break; }
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_stats.rtt_ms = now_ms - pong.echo_ms;   /* wraps cleanly in unsigned */
        s_ping_outstanding = 0;
        break;
    }

    case FMT_READY: {
        fmt_ready_t ready;
        if (!fmt_decode_ready(payload, len, &ready)) { s_stats.rx_bad++; break; }
        if ((link_host_caps() & FMT_HOST_CAP) && s_handlers.on_format_ready)
            s_handlers.on_format_ready(&ready);
        break;
    }
    case SATLINK_MSG_CMD_TRANSPORT: {
        satlink_cmd_transport_t m = {0};
        if (!satlink_dec_cmd_transport(payload, len, &m)) { s_stats.rx_bad++; break; }
        if (s_handlers.on_transport != NULL) { s_handlers.on_transport(m.cmd); }
        break;
    }

    case SATLINK_MSG_CMD_VOLUME: {
        satlink_cmd_volume_t m = {0};
        if (!satlink_dec_cmd_volume(payload, len, &m)) { s_stats.rx_bad++; break; }
        if (s_handlers.on_volume != NULL) { s_handlers.on_volume(m.volume); }
        break;
    }

    case SATLINK_MSG_CMD_RADIO: {
        satlink_cmd_radio_t m = {0};
        if (!satlink_dec_cmd_radio(payload, len, &m)) { s_stats.rx_bad++; break; }
        if (s_handlers.on_radio != NULL) {
            s_handlers.on_radio(m.radio != 0, m.discoverable != 0, m.disconnect != 0);
        }
        break;
    }

    case SATLINK_MSG_COVER_RETRY: {
        satlink_cover_retry_t m = {0};
        if (!satlink_dec_cover_retry(payload, len, &m)) { s_stats.rx_bad++; break; }
        /* Gated on the host's own cover capability, not only on ours. A host that
         * is sent no covers at all has nothing that could have failed, so a retry
         * from it names a transfer that never happened -- and serving it would
         * cost the phone an OBEX round trip for an image nobody will receive. */
        if ((link_host_caps() & COVER_STREAM_CAP) && s_handlers.on_cover_retry)
            s_handlers.on_cover_retry(m.boot_nonce, m.track_generation);
        break;
    }

    /* Protocol 1.8. The host sends these only to a device that claims
     * BT_SETTINGS, and this one does; nothing else gates them here. */
    case SATLINK_MSG_PAIR_REPLY: {
        satlink_pair_reply_t m = {0};
        if (!satlink_dec_pair_reply(payload, len, &m)) { s_stats.rx_bad++; break; }
        if (s_handlers.on_pair_reply != NULL)
            s_handlers.on_pair_reply(m.addr, m.accept != 0);
        break;
    }

    case SATLINK_MSG_BT_CONFIG_SET: {
        satlink_bt_config_set_t m = {0};
        if (!satlink_dec_bt_config_set(payload, len, &m)) { s_stats.rx_bad++; break; }
        if (s_handlers.on_bt_config_set != NULL)
            s_handlers.on_bt_config_set(&m);
        break;
    }

    case SATLINK_MSG_BOND_REMOVE: {
        satlink_bond_remove_t m = {0};
        if (!satlink_dec_bond_remove(payload, len, &m)) { s_stats.rx_bad++; break; }
        if (s_handlers.on_bond_remove != NULL)
            s_handlers.on_bond_remove(m.addr);
        break;
    }

    case SATLINK_MSG_BT_REFRESH:
        if (!satlink_dec_bt_refresh(payload, len)) { s_stats.rx_bad++; break; }
        if (s_handlers.on_bt_refresh != NULL)
            s_handlers.on_bt_refresh();
        break;

    default:
        /* A message we do not implement is not a protocol error: the other side
         * may be newer within the same major. Worth counting, though -- "the peer
         * sends something we ignore" and "the peer sends nothing" are different
         * faults that used to look identical from here. */
        s_stats.rx_unknown++;
        ESP_LOGD(TAG, "ignoring message 0x%02x", type);
        break;
    }
}

/* One read's worth of bytes, into the reassembler and the counters. */
static void rx_feed_chunk(const uint8_t *chunk, size_t n)
{
    if (s_stats.rx_bytes == 0) {
        /* The first byte ever. Worth its own line: it is the difference
         * between a wire that is not connected and a peer that is not
         * understood. */
        ESP_LOGI(TAG, "something is transmitting on GPIO%d", BOARD_LINK_RX_GPIO);
    }
    s_stats.rx_bytes += (uint32_t)n;

    satlink_rx_feed(&s_rx, chunk, n, on_frame, NULL);
    s_stats.rx_seq_gaps = s_rx.seq_gaps;
    /* Its own field. This one used to be rx_bad, and assigning it here
     * erased every payload that on_frame had just counted as bad. */
    s_stats.rx_bad_frames = s_rx.err_cobs + s_rx.err_crc + s_rx.err_runt + s_rx.err_too_long;
}

/*
 * Whatever is still buffered once no event is waiting to claim it.
 *
 * The driver can hold bytes that no event announces: it pushes them into the
 * ring before it posts the event, and when the event queue is full it drops the
 * event and keeps the bytes; the xQueueReset after an overrun flush can erase
 * one as well. Reading only what each event announces would then leave the
 * newest bytes of every later burst behind -- the tail of each host frame, its
 * delimiter included, waiting for the next PING -- until the next overrun.
 *
 * It stops as soon as an event is queued, so an overrun still being reported
 * gets to flush the bytes after its hole instead of having them joined to the
 * bytes before it. A data event whose bytes were taken here reads nothing.
 */
static void rx_drain_residue(uint8_t *chunk, size_t cap)
{
    while (uxQueueMessagesWaiting(s_uart_events) == 0) {
        int n = uart_read_bytes(BOARD_LINK_UART_NUM, chunk, cap, 0);
        if (n <= 0) {
            break;
        }
        rx_feed_chunk(chunk, (size_t)n);
    }
}

/* One UART_DATA event. Its size is a hint, not a count of what is buffered. */
static void rx_data_event(size_t announced, uint8_t *chunk, size_t cap)
{
    size_t remaining = announced;
    while (remaining > 0) {
        size_t want = (remaining < cap) ? remaining : cap;
        int n = uart_read_bytes(BOARD_LINK_UART_NUM, chunk, want, 0);
        if (n <= 0) {
            break;
        }
        remaining -= (size_t)n;
        rx_feed_chunk(chunk, (size_t)n);
    }
    rx_drain_residue(chunk, cap);
}

/*
 * Driven by the driver's event queue rather than by polling.
 *
 * The obvious version asks uart_read_bytes for a bufferful with a timeout, and
 * that is a trap: it returns when the buffer fills OR the timeout expires, and
 * our frames are a dozen bytes, so it waits out the whole timeout every single
 * time. Measured on the bench, that put 100 ms of latency on every message in
 * this direction — survivable for PING, a tenth of a second on every command.
 *
 * The event queue wakes us when the hardware says a burst has ended, which at
 * this baud rate is a few hundred microseconds after the last byte.
 */
static void link_rx_task(void *arg)
{
    (void)arg;
    uint8_t chunk[LINK_READ_CHUNK];

    for (;;) {
        uart_event_t event;
        if (xQueueReceive(s_uart_events, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (event.type) {
        case UART_DATA:
            rx_data_event(event.size, chunk, sizeof(chunk));
            break;

        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            /* We stopped reading for long enough to lose bytes. Throw away what
             * is left rather than feed the reassembler a hole in the middle of a
             * frame: it would resynchronise anyway, but this says so in the log.
             * The counter moves because a lost frame is a lost frame. */
            ESP_LOGW(TAG, "receiver overran, %s",
                     event.type == UART_FIFO_OVF ? "hardware fifo" : "ring buffer");
            uart_flush_input(BOARD_LINK_UART_NUM);
            xQueueReset(s_uart_events);
            satlink_rx_discard(&s_rx);
            /* Bytes that landed between the flush and the reset lost their
             * event with it. They came after the hole, so they are fine to
             * keep. */
            rx_drain_residue(chunk, sizeof(chunk));
            /* Its own counter. This used to increment rx_bad, which the next data
             * event overwrote from the reassembler's totals -- so the one number
             * that said "we were too slow" was reliably erased by the one that
             * says "the wire was noisy". */
            s_stats.rx_overruns++;
            break;

        default:
            ESP_LOGD(TAG, "uart event %d", event.type);
            break;
        }
    }
}

/* Every tenth beat, and only once there is something worth saying. A link that
 * is working says one line every ten seconds; a link that is not says why. */
static void report_stats(void)
{
    static uint32_t beat;
    if (++beat % 10 != 0) {
        return;
    }
    /* Nothing has ever arrived on the wire: no host, which is a normal state and
     * not worth a line every ten seconds. Note that this asks about bytes rather
     * than frames — a peer at the wrong baud rate produces plenty of the first and
     * none of the second, and that is exactly the case worth shouting about. */
    bool quiet = s_stats.rx_bytes == 0 &&
                 s_stats.tx_dropped_bulk == 0 && s_stats.tx_dropped_critical == 0;
    if (quiet) {
        return;
    }

    /* Two lines, so that each fits one LOG to the host. */
    ESP_LOGI(TAG, "tx %" PRIu32 " (dropped %" PRIu32 "/%" PRIu32 ", coalesced %" PRIu32
                  ", hello %" PRIu32 "), pings unanswered %" PRIu32,
             s_stats.tx_frames, s_stats.tx_dropped_bulk, s_stats.tx_dropped_critical,
             s_stats.tx_coalesced, s_stats.hello_sent, s_stats.pings_unanswered);
    ESP_LOGI(TAG, "rx %" PRIu32 " bytes %" PRIu32 " frames "
                  "(bad frames %" PRIu32 ", bad payloads %" PRIu32 ", gaps %" PRIu32
                  ", unknown %" PRIu32 ", refused %" PRIu32 ", overrun %" PRIu32
                  ", own hello %" PRIu32 ")",
             s_stats.rx_bytes, s_stats.rx_frames, s_stats.rx_bad_frames, s_stats.rx_bad,
             s_stats.rx_seq_gaps, s_stats.rx_unknown, s_stats.rx_refused, s_stats.rx_overruns,
             s_stats.rx_echoes);
    if (s_host_present) {
        ESP_LOGI(TAG, "round trip %" PRIu32 " ms", s_stats.rtt_ms);
    }
}

static void ping_tick(void *arg)
{
    (void)arg;

    // PONG proves liveness, not capability negotiation. A lost startup HELLO
    // or reply must not leave a restarted satellite permanently in legacy mode.
    // Only the startup HELLO and these retries go out while no host has greeted,
    // so the grid already keeps them a second apart; the slack absorbs jitter.
    if (!s_host_present && !s_peer_incompatible)
        reserve_hello(false, LINK_HELLO_MIN_GAP_US - LINK_HELLO_RETRY_SLACK_US);

    if (s_ping_outstanding > 0) {
        s_stats.pings_unanswered++;
        /* Not an error. A host that is not answering is a host that is busy or
         * absent, and this device is expected to keep playing either way. */
    }

    report_stats();

    satlink_ping_t m = { .tick_ms = (uint32_t)(esp_timer_get_time() / 1000) };
    uint8_t payload[8];
    size_t len = satlink_enc_ping(payload, sizeof(payload), &m);
    if (len != 0) {
        tx_push(SATLINK_MSG_PING, payload, len);
        s_ping_outstanding++;
    }
}

/* -------------------------------------------------------------- log bridging */

/* The spec asks for the device's logs to reach the host, not just the console.
 * This tees them: the original handler still runs, so a serial console keeps
 * working exactly as before. */
static int log_bridge(const char *fmt, va_list args)
{
    int written = 0;
    if (s_prev_vprintf != NULL) {
        va_list copy;
        va_copy(copy, args);
        written = s_prev_vprintf(fmt, copy);
        va_end(copy);
    }

    if (s_in_log_bridge) {
        return written;    /* a send failed and logged; do not chase our own tail */
    }
    s_in_log_bridge = true;

    /* Exactly what LOG carries, so nothing is cut twice. */
    char line[LINK_LOG_MAX + 1];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n > 0) {
        if ((size_t)n >= sizeof(line)) {
            /* vsnprintf cuts by bytes. A character that lost its tail would reach
             * the host as invalid UTF-8 in a field the protocol says is UTF-8. */
            line[satlink_utf8_prefix(line, sizeof(line) - 1)] = '\0';
        }
        /* The level was always 2, whatever the line said. The letter IDF puts in
         * front of it is the only place the level survives to this hook. */
        const uint8_t level = log_line_level(line);
        const char *text = log_line_tidy(line);
        if (text[0] != '\0') {
            link_send_log(level, text);
        }
    }

    s_in_log_bridge = false;
    return written;
}

/* ------------------------------------------------------------------- startup */

bool link_host_present(void) { return s_host_present; }
bool link_wait_for_host(uint32_t timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (!s_host_present && !s_peer_incompatible) {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            ESP_LOGW(TAG, "no host greeting in %" PRIu32 " ms; coming up with the "
                          "conservative answer to everything it would have told us",
                     timeout_ms);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

uint32_t link_host_caps(void) { return __atomic_load_n(&s_host_caps, __ATOMIC_SEQ_CST); }
uint8_t link_host_minor(void) { return __atomic_load_n(&s_host_minor, __ATOMIC_SEQ_CST); }

uint32_t link_bulk_pending(void)
{
    return (s_q_cover != NULL) ? (uint32_t)uxQueueMessagesWaiting(s_q_cover) : 0;
}

void link_get_stats(link_stats_t *out)
{
    if (out != NULL) { *out = s_stats; }
}

esp_err_t link_start(const link_handlers_t *handlers, uint32_t caps)
{
    s_own_caps = caps;
    if (handlers != NULL) {
        s_handlers = *handlers;
    }
    satlink_rx_init(&s_rx);

    s_q_telemetry = xQueueCreate(LINK_Q_TELEMETRY, sizeof(tx_item_t));
    s_q_cover     = xQueueCreate(LINK_Q_COVER, sizeof(tx_item_t));
    ESP_RETURN_ON_FALSE(s_q_telemetry != NULL && s_q_cover != NULL,
                        ESP_ERR_NO_MEM, TAG, "no memory for the tx queues");

    const uart_config_t cfg = {
        .baud_rate = BOARD_LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(BOARD_LINK_UART_NUM, LINK_RX_BUF, LINK_TX_BUF,
                                            LINK_EVENT_QUEUE, &s_uart_events, 0),
                        TAG, "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(BOARD_LINK_UART_NUM, &cfg), TAG, "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(BOARD_LINK_UART_NUM, BOARD_LINK_TX_GPIO, BOARD_LINK_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart_set_pin");
    ESP_RETURN_ON_ERROR(uart_set_rx_timeout(BOARD_LINK_UART_NUM, LINK_RX_TIMEOUT_SYM),
                        TAG, "uart_set_rx_timeout");

#if CONFIG_SAT_LINK_LOOPBACK
    /* Wires TX to RX inside the chip so the whole link can be exercised with
     * nothing attached. Every frame we send comes straight back at us. */
    ESP_RETURN_ON_ERROR(uart_set_loop_back(BOARD_LINK_UART_NUM, true), TAG, "loop_back");
    ESP_LOGW(TAG, "loopback enabled: this build talks to itself, not to a host");
#endif

    ESP_RETURN_ON_FALSE(
        xTaskCreate(link_rx_task, "link_rx", LINK_TASK_STACK, NULL, LINK_RX_PRIO, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "no memory for the rx task");
    ESP_RETURN_ON_FALSE(
        xTaskCreate(link_tx_task, "link_tx", LINK_TASK_STACK, NULL, LINK_TX_PRIO, &s_tx_task) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "no memory for the tx task");

    const esp_timer_create_args_t timer_args = {
        .callback = ping_tick,
        .name = "link_ping",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_ping_timer), TAG, "timer_create");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_ping_timer, LINK_PING_PERIOD_US),
                        TAG, "timer_start");

    answer_hello(false);

    ESP_LOGI(TAG, "UART%d at %d baud, TX %d, RX %d",
             BOARD_LINK_UART_NUM, BOARD_LINK_BAUD, BOARD_LINK_TX_GPIO, BOARD_LINK_RX_GPIO);

    s_prev_vprintf = esp_log_set_vprintf(log_bridge);
    return ESP_OK;
}
