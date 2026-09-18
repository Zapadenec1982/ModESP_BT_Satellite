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
#include "satlink/frame.h"

static const char *TAG = "sat.link";

#define LINK_RX_BUF          (SATLINK_WIRE_MAX * 2)
#define LINK_TX_BUF          (SATLINK_WIRE_MAX * 2)
#define LINK_READ_CHUNK      256

/* Two queues, because the spec divides messages into those that may be thrown
 * away when the host stops reading and those that may not. Draining critical
 * first is what keeps BT_STATE ahead of a backlog of TRACK_POS. */
#define LINK_Q_CRITICAL      10
#define LINK_Q_BULK          14

#define LINK_TASK_STACK      3584
#define LINK_RX_PRIO         (configMAX_PRIORITIES - 6)
#define LINK_TX_PRIO         (configMAX_PRIORITIES - 7)

#define LINK_PING_PERIOD_US  1000000

typedef struct {
    uint8_t *data;
    uint16_t len;
    uint8_t  type;
} tx_item_t;

static QueueHandle_t     s_q_critical;
static QueueHandle_t     s_q_bulk;
static link_handlers_t   s_handlers;
static satlink_rx_t      s_rx;
static link_stats_t      s_stats;
static uint8_t           s_tx_seq;
static bool              s_host_present;
static bool              s_peer_incompatible;
static uint32_t          s_host_caps;
static uint32_t          s_ping_outstanding;
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
    free(item->data);
    item->data = NULL;
}

static void tx_push(uint8_t type, const uint8_t *payload, size_t payload_len)
{
    if (s_q_critical == NULL) {
        return;
    }
    /* An incompatible peer gets liveness and log lines and nothing else: it would
     * not understand the rest, and guessing is worse than silence. */
    if (s_peer_incompatible &&
        type != SATLINK_MSG_PING && type != SATLINK_MSG_PONG && type != SATLINK_MSG_LOG) {
        return;
    }

    uint8_t wire[SATLINK_WIRE_MAX];
    size_t wire_len = 0;
    if (!satlink_frame_encode(type, s_tx_seq, payload, payload_len,
                              wire, sizeof(wire), &wire_len)) {
        return;     /* the encoders already refuse oversize; nothing to say here */
    }
    s_tx_seq++;

    tx_item_t item = { .data = malloc(wire_len), .len = (uint16_t)wire_len, .type = type };
    if (item.data == NULL) {
        return;
    }
    memcpy(item.data, wire, wire_len);

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

        /* The UART driver's own buffer absorbs this; a full one means the host is
         * not draining, and waiting forever there would violate rule one. */
        uart_write_bytes(BOARD_LINK_UART_NUM, item.data, item.len);
        s_stats.tx_frames++;
        tx_item_free(&item);
    }
}

/* ------------------------------------------------------------------- senders */

void link_send_bt_state(uint8_t state, uint8_t codec, const uint8_t addr[6], const char *name)
{
    satlink_bt_state_t_msg m = { .state = state, .codec = codec, .name = satlink_str(name) };
    if (addr != NULL) { memcpy(m.addr, addr, sizeof(m.addr)); }

    uint8_t payload[SATLINK_PAYLOAD_MAX];
    size_t len = satlink_enc_bt_state(payload, sizeof(payload), &m);
    if (len != 0) { tx_push(SATLINK_MSG_BT_STATE, payload, len); }
}

void link_send_track_meta(const char *title, const char *artist, const char *album)
{
    satlink_track_meta_t m = {
        .title  = satlink_str(title),
        .artist = satlink_str(artist),
        .album  = satlink_str(album),
    };
    uint8_t payload[SATLINK_PAYLOAD_MAX];
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
    satlink_log_t m = { .level = level, .text = satlink_str(text) };
    uint8_t payload[SATLINK_PAYLOAD_MAX];
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
        .caps  = SATLINK_CAP_A2DP_SINK | SATLINK_CAP_AVRCP_META |
                 SATLINK_CAP_ABS_VOLUME | SATLINK_CAP_COVER_ART |
                 SATLINK_CAP_EXPANDER | SATLINK_CAP_ADC,
        .build = satlink_str(build),
    };
    uint8_t payload[SATLINK_PAYLOAD_MAX];
    size_t len = satlink_enc_hello(payload, sizeof(payload), &m);
    if (len != 0) { tx_push(SATLINK_MSG_HELLO, payload, len); }
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

    case SATLINK_MSG_PONG:
        s_ping_outstanding = 0;
        break;

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
         * may be newer within the same major. */
        ESP_LOGD(TAG, "ignoring message 0x%02x", type);
        break;
    }
}

static void link_rx_task(void *arg)
{
    (void)arg;
    uint8_t chunk[LINK_READ_CHUNK];

    for (;;) {
        int n = uart_read_bytes(BOARD_LINK_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }
        satlink_rx_feed(&s_rx, chunk, (size_t)n, on_frame, NULL);
        s_stats.rx_seq_gaps = s_rx.seq_gaps;
        s_stats.rx_bad = s_rx.err_cobs + s_rx.err_crc + s_rx.err_runt + s_rx.err_too_long;
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

esp_err_t link_start(const link_handlers_t *handlers)
{
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
                                            0, NULL, 0), TAG, "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(BOARD_LINK_UART_NUM, &cfg), TAG, "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(BOARD_LINK_UART_NUM, BOARD_LINK_TX_GPIO, BOARD_LINK_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart_set_pin");

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
