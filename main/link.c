#include "link.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "board.h"

#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_check.h"
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
#define LINK_TX_BUF          (SATLINK_WIRE_MAX * 2)
#define LINK_READ_CHUNK      256
#define LINK_EVENT_QUEUE     16

/* Symbol periods of idle before the hardware calls a burst finished. Ten of them
 * at 460800 is about 217 us, which is far inside the 30 ms the spec allows a GPIO
 * edge to reach the wire, and long enough not to split a frame in two. */
#define LINK_RX_TIMEOUT_SYM  10

/* Two queues, because the spec divides messages into those that may be thrown
 * away when the host stops reading and those that may not. Draining critical
 * first is what keeps BT_STATE ahead of a backlog of TRACK_POS. */
#define LINK_Q_CRITICAL      10
#define LINK_Q_BULK          14

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

/*
 * How much stack a sender is allowed to take.
 *
 * These were SATLINK_PAYLOAD_MAX, all 1036 bytes of it, and that overflowed the
 * main task on the first boot: the log bridge runs in whoever happens to be
 * logging, and app_main calling into a sender that allocates a kilobyte is the
 * end of a 3584-byte stack. A message gets room for what it can actually carry
 * and no more. Only COVER needs the full payload, and it has a static buffer.
 */
#define LINK_NAME_MAX      64        /* a Bluetooth device name */
#define LINK_META_MAX     128        /* one of title, artist, album */
#define LINK_LOG_MAX      224        /* one log line */
#define LINK_BUILD_MAX     80        /* "sat <version> <idf>" */

#define LINK_BUF_BT_STATE  (1 + 1 + 6 + 2 + LINK_NAME_MAX)
#define LINK_BUF_META      (3 * (2 + LINK_META_MAX))
#define LINK_BUF_LOG       (1 + 2 + LINK_LOG_MAX)
#define LINK_BUF_HELLO     (1 + 1 + 4 + 2 + LINK_BUILD_MAX)

/* The payload, not the frame. Sequence numbers are stamped at the moment of
 * transmission rather than of queueing, because there are two queues and the
 * sender drains one before the other: numbering at queue time puts frames on the
 * wire out of order, and the receiver's gap counter then reports losses that
 * never happened. Found exactly that way, in loopback, counting 256 phantom gaps
 * every ten seconds. */
typedef struct {
    uint8_t *payload;
    uint16_t len;
    uint8_t  type;
} tx_item_t;

static QueueHandle_t     s_uart_events;
static QueueHandle_t     s_q_critical;
static QueueHandle_t     s_q_bulk;
static link_handlers_t   s_handlers;
static satlink_rx_t      s_rx;
static link_stats_t      s_stats;
static uint8_t           s_tx_seq;
static bool              s_host_present;
static bool              s_peer_incompatible;
static uint32_t          s_host_caps;
static uint32_t          s_own_caps;
static uint32_t          s_ping_outstanding;
static int64_t           s_last_hello_us;
static esp_timer_handle_t s_ping_timer;

/* Guards the log bridge against feeding on itself: a failure inside a send would
 * otherwise log, which would send, which would fail. */
static volatile bool     s_in_log_bridge;
static vprintf_like_t    s_prev_vprintf;

/* ------------------------------------------------------------------ outbound */

/* TRACK_POS, LOG and COVER are the three the spec names as droppable. */
static bool type_is_droppable(uint8_t type)
{
    return type == SATLINK_MSG_TRACK_POS ||
           type == SATLINK_MSG_LOG ||
           type == SATLINK_MSG_COVER;
}

static void tx_item_free(tx_item_t *item)
{
    free(item->payload);
    item->payload = NULL;
}

static void tx_push(uint8_t type, const uint8_t *payload, size_t payload_len)
{
    if (s_q_critical == NULL) {
        return;
    }
    /* An incompatible peer gets liveness and log lines and nothing else: it would
     * not understand the rest, and guessing is worse than silence. */
    if (s_peer_incompatible &&
        type != SATLINK_MSG_HELLO && type != SATLINK_MSG_PING &&
        type != SATLINK_MSG_PONG && type != SATLINK_MSG_LOG) {
        return;
    }

    if (payload_len > SATLINK_PAYLOAD_MAX) {
        return;     /* the encoders already refuse oversize; nothing to say here */
    }

    /* A copy of the payload and nothing more. Framing happens in the sender, so
     * nothing frame-sized ever lands on the caller's stack — and the caller can
     * be any task at all, since the log bridge runs in whoever happened to log. */
    uint8_t *buf = malloc(payload_len != 0 ? payload_len : 1);
    if (buf == NULL) {
        return;
    }
    if (payload_len != 0) {
        memcpy(buf, payload, payload_len);
    }

    tx_item_t item = { .payload = buf, .len = (uint16_t)payload_len, .type = type };

    bool bulk = type_is_droppable(type);
    QueueHandle_t q = bulk ? s_q_bulk : s_q_critical;

    if (xQueueSend(q, &item, 0) == pdTRUE) {
        return;
    }

    /* Full. Make room by losing the oldest, which for a state message is also the
     * least interesting one. */
    tx_item_t victim;
    if (xQueueReceive(q, &victim, 0) == pdTRUE) {
        tx_item_free(&victim);
        if (bulk) { s_stats.tx_dropped_bulk++; } else { s_stats.tx_dropped_critical++; }
    }
    if (xQueueSend(q, &item, 0) != pdTRUE) {
        tx_item_free(&item);
        if (bulk) { s_stats.tx_dropped_bulk++; } else { s_stats.tx_dropped_critical++; }
    }
}

static void link_tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        tx_item_t item;
        bool got = (xQueueReceive(s_q_critical, &item, 0) == pdTRUE);
        if (!got) {
            got = (xQueueReceive(s_q_bulk, &item, 0) == pdTRUE);
        }
        if (!got) {
            /* Nothing waiting. Block on the critical queue so a new message wakes
             * us at once rather than after a poll interval. */
            if (xQueueReceive(s_q_critical, &item, pdMS_TO_TICKS(20)) != pdTRUE) {
                continue;
            }
        }

        /* Framed here, in transmission order, and into a buffer only this task
         * touches. */
        static uint8_t wire[SATLINK_WIRE_MAX];
        size_t wire_len = 0;
        if (satlink_frame_encode(item.type, s_tx_seq, item.payload, item.len,
                                 wire, sizeof(wire), &wire_len)) {
            s_tx_seq++;
            /* There is no flow control on this link, so the transmitter always
             * drains and this wait is bounded by the baud rate. The queues in
             * front are what keep callers from ever waiting at all. */
            uart_write_bytes(BOARD_LINK_UART_NUM, wire, wire_len);
            s_stats.tx_frames++;
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

void link_send_volume_state(uint8_t volume, uint8_t source)
{
    satlink_volume_state_t m = { .volume = volume, .source = source };
    uint8_t payload[8];
    size_t len = satlink_enc_volume_state(payload, sizeof(payload), &m);
    if (len != 0) { tx_push(SATLINK_MSG_VOLUME_STATE, payload, len); }
}

void link_send_gpio_in(uint32_t values, uint32_t changed)
{
    satlink_gpio_in_t m = { .values = values, .changed = changed };
    uint8_t payload[16];
    size_t len = satlink_enc_gpio_in(payload, sizeof(payload), &m);
    if (len != 0) { tx_push(SATLINK_MSG_GPIO_IN, payload, len); }
}

void link_send_gpio_map(const satlink_gpio_map_t *map)
{
    uint8_t payload[1 + SATLINK_GPIO_MAX * 3];
    size_t len = satlink_enc_gpio_map(payload, sizeof(payload), map);
    if (len != 0) { tx_push(SATLINK_MSG_GPIO_MAP, payload, len); }
}

void link_send_adc_value(uint8_t index, uint16_t raw, uint16_t mv)
{
    satlink_adc_value_t m = { .index = index, .raw = raw, .mv = mv };
    uint8_t payload[8];
    size_t len = satlink_enc_adc_value(payload, sizeof(payload), &m);
    if (len != 0) { tx_push(SATLINK_MSG_ADC_VALUE, payload, len); }
}

void link_send_cover(uint16_t chunk, uint16_t total, uint8_t mime,
                     uint32_t total_len, const uint8_t *data, uint16_t len)
{
    satlink_cover_t m = { .chunk = chunk, .total = total, .mime = mime,
                          .total_len = total_len, .len = len, .data = data };
    static uint8_t payload[SATLINK_PAYLOAD_MAX];   /* too big for a task stack */
    size_t n = satlink_enc_cover(payload, sizeof(payload), &m);
    if (n != 0) { tx_push(SATLINK_MSG_COVER, payload, n); }
}

void link_send_log(uint8_t level, const char *text)
{
    satlink_log_t m = { .level = level, .text = satlink_str_clip(text, LINK_LOG_MAX) };
    uint8_t payload[LINK_BUF_LOG];
    size_t len = satlink_enc_log(payload, sizeof(payload), &m);
    if (len != 0) { tx_push(SATLINK_MSG_LOG, payload, len); }
}

static void send_hello(void)
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
        .caps  = s_own_caps,
        .build = satlink_str_clip(build, LINK_BUILD_MAX),
    };
    uint8_t payload[LINK_BUF_HELLO];
    size_t len = satlink_enc_hello(payload, sizeof(payload), &m);
    if (len == 0) {
        /* Never observed, but it would have been invisible if it happened: the
         * link would simply never come up and each side would blame the other. */
        ESP_LOGE(TAG, "HELLO will not encode into %u bytes", (unsigned)sizeof(payload));
        return;
    }
    s_last_hello_us = esp_timer_get_time();
    s_stats.hello_sent++;
    ESP_LOGI(TAG, "HELLO out: %u.%u, caps 0x%08" PRIx32 ", %u bytes",
             SATLINK_PROTO_MAJOR, SATLINK_PROTO_MINOR, s_own_caps, (unsigned)len);
    tx_push(SATLINK_MSG_HELLO, payload, len);
}

/* The reply half of the greeting. Without it a peer that started first never
 * learns our version or our capabilities, and is right to refuse to bring the
 * link up: it would be guessing at what we can do. */
static void answer_hello(void)
{
    int64_t now = esp_timer_get_time();
    if (s_last_hello_us != 0 && now - s_last_hello_us < LINK_HELLO_MIN_GAP_US) {
        return;
    }
    send_hello();
}

/* ------------------------------------------------------------------- inbound */

static void handle_hello(const uint8_t *payload, size_t len)
{
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
        answer_hello();
        return;
    }

    s_peer_incompatible = false;
    s_host_present = true;
    s_host_caps = h.caps;
    ESP_LOGI(TAG, "host %u.%u, caps 0x%08" PRIx32 ", build \"%.*s\"",
             h.major, h.minor, h.caps, (int)h.build.len, h.build.ptr);

    if ((h.caps & SATLINK_HOST_CAP_RELOCK) == 0) {
        ESP_LOGI(TAG, "host cannot relock: announcing 44.1 kHz only");
    }

    /* The spec asks for a HELLO after start and in answer to one. Only the first
     * half was here, so a host that came up second waited for ever. */
    answer_hello();

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

    case SATLINK_MSG_GPIO_CFG: {
        static satlink_gpio_cfg_t cfg;      /* 97 bytes; not worth a task stack */
        if (!satlink_dec_gpio_cfg(payload, len, &cfg)) { s_stats.rx_bad++; break; }
        if (s_handlers.on_gpio_cfg != NULL) { s_handlers.on_gpio_cfg(&cfg); }
        break;
    }

    case SATLINK_MSG_GPIO_OUT: {
        satlink_gpio_out_t m = {0};
        if (!satlink_dec_gpio_out(payload, len, &m)) { s_stats.rx_bad++; break; }
        if (s_handlers.on_gpio_out != NULL) { s_handlers.on_gpio_out(m.values, m.mask); }
        break;
    }

    case SATLINK_MSG_ADC_READ: {
        satlink_adc_read_t m = {0};
        if (!satlink_dec_adc_read(payload, len, &m)) { s_stats.rx_bad++; break; }
        if (s_handlers.on_adc_read != NULL) { s_handlers.on_adc_read(m.index); }
        break;
    }

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

/*
 * Driven by the driver's event queue rather than by polling.
 *
 * The obvious version asks uart_read_bytes for a bufferful with a timeout, and
 * that is a trap: it returns when the buffer fills OR the timeout expires, and
 * our frames are a dozen bytes, so it waits out the whole timeout every single
 * time. Measured on the bench, that put 100 ms of latency on every message in
 * this direction — survivable for PING, fatal for GPIO_IN, which the spec gives
 * 30 ms from edge to wire.
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
        case UART_DATA: {
            size_t remaining = event.size;
            while (remaining > 0) {
                size_t want = (remaining < sizeof(chunk)) ? remaining : sizeof(chunk);
                int n = uart_read_bytes(BOARD_LINK_UART_NUM, chunk, want, 0);
                if (n <= 0) {
                    break;
                }
                remaining -= (size_t)n;

                if (s_stats.rx_bytes == 0) {
                    /* The first byte ever. Worth its own line: it is the
                     * difference between a wire that is not connected and a peer
                     * that is not understood. */
                    ESP_LOGI(TAG, "something is transmitting on GPIO%d", BOARD_LINK_RX_GPIO);
                }
                s_stats.rx_bytes += (uint32_t)n;

                satlink_rx_feed(&s_rx, chunk, (size_t)n, on_frame, NULL);
                s_stats.rx_seq_gaps = s_rx.seq_gaps;
                s_stats.rx_bad = s_rx.err_cobs + s_rx.err_crc + s_rx.err_runt + s_rx.err_too_long;
            }
            break;
        }

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
            satlink_rx_init(&s_rx);
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

    ESP_LOGI(TAG, "tx %" PRIu32 " (dropped %" PRIu32 "/%" PRIu32 ", hello %" PRIu32 "), "
                  "rx %" PRIu32 " bytes %" PRIu32 " frames "
                  "(bad %" PRIu32 ", gaps %" PRIu32 ", unknown %" PRIu32 ", overrun %" PRIu32 "), "
                  "pings unanswered %" PRIu32,
             s_stats.tx_frames, s_stats.tx_dropped_bulk, s_stats.tx_dropped_critical,
             s_stats.hello_sent,
             s_stats.rx_bytes, s_stats.rx_frames, s_stats.rx_bad, s_stats.rx_seq_gaps,
             s_stats.rx_unknown, s_stats.rx_overruns, s_stats.pings_unanswered);
    if (s_host_present) {
        ESP_LOGI(TAG, "round trip %" PRIu32 " ms", s_stats.rtt_ms);
    }
}

static void ping_tick(void *arg)
{
    (void)arg;

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

    char line[200];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n > 0) {
        /* Strip the trailing newline and the colour escape the console adds: the
         * host wants text, not terminal control. */
        size_t len = strnlen(line, sizeof(line));
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                           line[len - 1] == 'm'  || line[len - 1] == '\033')) {
            if (line[len - 1] == 'm' && (len < 2 || line[len - 2] != '[')) { break; }
            line[--len] = '\0';
        }
        if (len > 0) {
            link_send_log(SATLINK_LOG_INFO, line);
        }
    }

    s_in_log_bridge = false;
    return written;
}

/* ------------------------------------------------------------------- startup */

bool link_host_present(void) { return s_host_present; }
uint32_t link_host_caps(void) { return s_host_caps; }

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

    s_q_critical = xQueueCreate(LINK_Q_CRITICAL, sizeof(tx_item_t));
    s_q_bulk     = xQueueCreate(LINK_Q_BULK, sizeof(tx_item_t));
    ESP_RETURN_ON_FALSE(s_q_critical != NULL && s_q_bulk != NULL,
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
        xTaskCreate(link_tx_task, "link_tx", LINK_TASK_STACK, NULL, LINK_TX_PRIO, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "no memory for the tx task");

    const esp_timer_create_args_t timer_args = {
        .callback = ping_tick,
        .name = "link_ping",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_ping_timer), TAG, "timer_create");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_ping_timer, LINK_PING_PERIOD_US),
                        TAG, "timer_start");

    /* HELLO first, then GPIO_MAP, always and in that order — the host reads the
     * map as the answer to "what lines does this thing have", and an empty one is
     * a legitimate answer. */
    send_hello();

    ESP_LOGI(TAG, "UART%d at %d baud, TX %d, RX %d",
             BOARD_LINK_UART_NUM, BOARD_LINK_BAUD, BOARD_LINK_TX_GPIO, BOARD_LINK_RX_GPIO);

    s_prev_vprintf = esp_log_set_vprintf(log_bridge);
    return ESP_OK;
}
