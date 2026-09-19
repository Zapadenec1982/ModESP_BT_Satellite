#include "bt_av.h"

#include <inttypes.h>
#include <string.h>

#include "audio_out.h"
#include "link.h"

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "satlink/proto.h"

/* What the stack will actually register stream endpoints for, so the startup line
 * says what is true of this build rather than what was true of the first one. The
 * two Kconfig options come from patches/0001; see docs/BASE.md. */
#if defined(CONFIG_BT_A2DP_APTX_DECODER) && defined(CONFIG_BT_A2DP_AAC_DECODER)
#define BT_AV_CODEC_LIST    ", SBC, AAC, aptX, aptX HD"
#elif defined(CONFIG_BT_A2DP_APTX_DECODER)
#define BT_AV_CODEC_LIST    ", SBC, aptX, aptX HD"
#elif defined(CONFIG_BT_A2DP_AAC_DECODER)
#define BT_AV_CODEC_LIST    ", SBC, AAC"
#else
#define BT_AV_CODEC_LIST    ", SBC"
#endif

/*
 * Declared here rather than by including the Bluedroid stack header.
 *
 * A2DP_SinkAllow48kHz is ours -- it arrives with patches/0001 -- but it lives in
 * the stack's private include path, which the application component cannot see,
 * and it is not part of esp_a2dp_api.h. Adding it to the public API would mean
 * more patch surface in a header Espressif owns, for one call made once at
 * startup. See bt_overlay/host/bluedroid/stack/a2dp/a2dp_codec_config.c.
 */
extern void A2DP_SinkAllow48kHz(bool allow);

static const char *TAG = "sat.bt";

#define BT_TASK_STACK       3584
#define BT_TASK_PRIO        (configMAX_PRIORITIES - 4)
#define BT_QUEUE_DEPTH      12

/* Metadata we ask the phone for. PLAYING_TIME is not in the usual example masks
 * but TRACK_POS needs the track length. */
#define BT_META_ATTRS   (ESP_AVRC_MD_ATTR_TITLE   | \
                         ESP_AVRC_MD_ATTR_ARTIST  | \
                         ESP_AVRC_MD_ATTR_ALBUM   | \
                         ESP_AVRC_MD_ATTR_PLAYING_TIME | \
                         ESP_AVRC_MD_ATTR_COVER_ART)

/*
 * How large a cover this device will carry.
 *
 * The protocol's ceiling is 256 KB, which this board does not have: a WROOM has
 * no PSRAM and Bluedroid has already taken its share. What AVRCP calls the linked
 * thumbnail is a 200x200 image and runs to a few kilobytes, so 48 KB is generous
 * for what is actually asked for and affordable with what is actually here.
 * Anything larger is not sent at all, per the protocol: a truncated image is
 * worse than none, because the host cannot tell it from a lost tail.
 */
#define BT_COVER_MAX_BYTES  (48 * 1024)

/* Transaction labels. AVRCP allows sixteen; giving each conversation its own
 * makes a packet capture readable. */
#define TL_META         0
#define TL_RN_CAPS      1
#define TL_TRACK_CHANGE 2
#define TL_PLAY_STATUS  3
#define TL_VOLUME       4
#define TL_PASSTHROUGH  5
#define TL_PLAY_POS     6
#define TL_PLAY_STATE   7

#define META_SETTLE_US  (300 * 1000)    /* attributes arrive one per event */
#define POS_PERIOD_US   (1000 * 1000)   /* the spec's one per second, playing only */

/*
 * How often the phone is asked to notify about playback position, in seconds.
 *
 * One second was the obvious choice and the wrong one: every notification costs
 * an AVRCP packet inbound and a re-registration outbound, on the same radio link
 * that is carrying the music. Five seconds cuts that traffic fivefold, and the
 * progress bar does not suffer, because TRACK_POS still goes out once a second
 * with the position interpolated from the last notification.
 */
#define POS_NOTIFY_INTERVAL_S  5

/* Silence between announcing a format and clocking it, per PROTOCOL.md,
 * "Частота: одна на з'єднання". The host needs it to stop its channel and
 * bring it back up on the new width; the width is the part that fails
 * silently if it does not get the chance. */
#define BT_AV_FMT_SILENCE_MS   50

/* A2DP events arrive on the Bluetooth stack's own task. Anything that can block —
 * retuning the I2S clock, for one — is handed to this task instead. */
typedef struct {
    uint16_t             event;
    esp_a2d_cb_param_t   param;
} bt_a2d_msg_t;

static QueueHandle_t s_queue;

/* Everything the host is ever told about, in one place. */
static struct {
    uint8_t  state;                 /* satlink_bt_state_t */
    uint8_t  codec;                 /* satlink_codec_t */
    uint8_t  addr[6];
    char     name[64];

    uint8_t  play_state;            /* satlink_play_state_t */
    uint32_t elapsed_ms;
    uint32_t total_ms;
    int64_t  elapsed_at_us;         /* when elapsed_ms was true, for interpolation */

    char     title[128];
    char     artist[128];
    char     album[128];

    uint32_t rate_hz;               /* what STREAM_FMT last announced, 0 if never */
    uint8_t  bits;
    uint8_t  channels;

    uint8_t  volume;                /* absolute AVRCP volume, 0..127, ours to own */
    bool     volume_notify;         /* the phone asked to be told when it changes */
    bool     avrcp_up;
    bool     has_play_pos;          /* the phone will notify about position */

    char     art_handle[8];         /* seven characters and a terminator */
    uint8_t *art;                   /* grown while a transfer runs, freed after */
    size_t   art_len;
    bool     art_busy;
    bool     art_oversize;
} s_now;

static esp_timer_handle_t s_meta_timer;
static esp_timer_handle_t s_pos_timer;

static const char *conn_state_name(esp_a2d_connection_state_t state)
{
    switch (state) {
    case ESP_A2D_CONNECTION_STATE_DISCONNECTED:  return "disconnected";
    case ESP_A2D_CONNECTION_STATE_CONNECTING:    return "connecting";
    case ESP_A2D_CONNECTION_STATE_CONNECTED:     return "connected";
    case ESP_A2D_CONNECTION_STATE_DISCONNECTING: return "disconnecting";
    default:                                     return "?";
    }
}

/* The configured CIE carries exactly one frequency bit, unlike the capability
 * CIE we advertise. */
static uint32_t sbc_rate_hz(const esp_a2d_cie_sbc_t *sbc)
{
    if (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_48K) { return 48000; }
    if (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_44K) { return 44100; }
    if (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_32K) { return 32000; }
    if (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_16K) { return 16000; }
    return 0;
}

/*
 * The rest of the codec information elements, read as raw bytes.
 *
 * `cie` holds the codec specific information element with the AVDTP header
 * already removed, so byte 0 is the first codec byte. The bitfield structs in
 * esp_a2dp_api.h only cover SBC and the MPEG family, and the vendor codecs have
 * no struct at all, so these read the bytes directly. Layouts are from the A2DP
 * codec specifications and match what the stack's own parsers do.
 */

/* MPEG-2/4 AAC: [0] object type, [1..2] a twelve bit frequency mask,
 * [2] bits 3..2 channels, [3..5] bit rate. */
static uint32_t aac_rate_hz(const uint8_t *cie)
{
    uint16_t f = (uint16_t)cie[1] | (uint16_t)((cie[2] << 8) & 0xF000);
    if (f & 0x8000) { return 48000; }
    if (f & 0x0001) { return 44100; }
    if (f & 0x0002) { return 32000; }
    if (f & 0x0004) { return 24000; }
    if (f & 0x0008) { return 22050; }
    if (f & 0x0010) { return 16000; }
    return 0;
}

static uint8_t aac_channels(const uint8_t *cie)
{
    return (cie[2] & 0x0C) == 0x08 ? 1 : 2;     /* 0x08 mono, 0x04 stereo */
}

/* aptX and aptX HD: [0..3] vendor id and [4..5] codec id, both little endian,
 * then [6] with the frequency in the high nibble and the channel mode in the
 * low one. aptX HD adds four reserved bytes that carry nothing we need. */
#define APTX_CIE_FREQ_CHAN      6

static uint32_t aptx_vendor_id(const uint8_t *cie)
{
    return (uint32_t)cie[0] | ((uint32_t)cie[1] << 8) |
           ((uint32_t)cie[2] << 16) | ((uint32_t)cie[3] << 24);
}

static uint16_t aptx_codec_id(const uint8_t *cie)
{
    return (uint16_t)cie[4] | (uint16_t)((uint16_t)cie[5] << 8);
}

static uint32_t aptx_rate_hz(const uint8_t *cie)
{
    uint8_t f = cie[APTX_CIE_FREQ_CHAN];
    if (f & 0x10) { return 48000; }
    if (f & 0x20) { return 44100; }
    return 0;
}

static uint8_t aptx_channels(const uint8_t *cie)
{
    return (cie[APTX_CIE_FREQ_CHAN] & 0x01) ? 1 : 2;    /* 0x01 mono, 0x02 stereo */
}

void bt_av_report_state(void)
{
    link_send_bt_state(s_now.state, s_now.codec, s_now.addr, s_now.name);
}

void bt_av_report_stream_fmt(void)
{
    if (s_now.rate_hz == 0) {
        return;     /* nothing negotiated yet; there is no format to announce */
    }
    link_send_stream_fmt(s_now.rate_hz, s_now.bits, s_now.channels);
    ESP_LOGI(TAG, "announced %" PRIu32 " Hz, %u-bit slots, %u channels",
             s_now.rate_hz, s_now.bits, s_now.channels);
}

static void send_track_meta(void *arg)
{
    (void)arg;
    link_send_track_meta(s_now.title, s_now.artist, s_now.album);
    ESP_LOGI(TAG, "meta: \"%s\" / \"%s\" / \"%s\"", s_now.title, s_now.artist, s_now.album);
}

static void meta_settle_restart(void)
{
    /* Attributes come one per event and a phone may leave some out, so rather
     * than waiting for a set that might never complete, send shortly after the
     * last one that did arrive. */
    esp_timer_stop(s_meta_timer);
    esp_timer_start_once(s_meta_timer, META_SETTLE_US);
}

static void send_track_pos(void)
{
    uint32_t elapsed = s_now.elapsed_ms;
    if (s_now.play_state == SATLINK_PLAY_PLAYING && s_now.elapsed_at_us != 0) {
        int64_t since_ms = (esp_timer_get_time() - s_now.elapsed_at_us) / 1000;
        if (since_ms > 0) {
            elapsed += (uint32_t)since_ms;
        }
    }
    link_send_track_pos(s_now.play_state, elapsed, s_now.total_ms);
}

static void pos_tick(void *arg)
{
    (void)arg;

    /* In pause and in stop, nothing at all. The host holds the last value. */
    if (!s_now.avrcp_up || s_now.play_state != SATLINK_PLAY_PLAYING) {
        return;
    }

    if (s_now.has_play_pos) {
        /* The phone notifies on movement; this is only to keep the bar sliding
         * smoothly between notifications. */
        send_track_pos();
    } else {
        /* The fallback the bench forced on us: one phone advertised 0x1f26, which
         * has no PLAY_POS bit, so the position has to be asked for. */
        esp_avrc_ct_send_get_play_status_cmd(TL_PLAY_STATUS);
    }
}

/* ---------------------------------------------------------------- cover art */

static void cover_reset(void)
{
    free(s_now.art);
    s_now.art = NULL;
    s_now.art_len = 0;
    s_now.art_busy = false;
    s_now.art_oversize = false;
}

static uint8_t cover_mime(const uint8_t *data, size_t len)
{
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return SATLINK_MIME_JPEG;
    }
    if (len >= 4 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return SATLINK_MIME_PNG;
    }
    return SATLINK_MIME_UNKNOWN;
}

/* Only when the host asked for covers. With no host the bit is clear, so none of
 * this runs and the behaviour of a device sitting on its own is unchanged. */
static void cover_fetch(void)
{
    if (s_now.art_busy || s_now.art_handle[0] == '\0') {
        return;
    }
    if ((link_host_caps() & SATLINK_HOST_CAP_COVER) == 0) {
        return;
    }
    s_now.art_busy = true;
    s_now.art_oversize = false;
    if (esp_avrc_ct_cover_art_connect(ESP_AVRC_CA_MTU_MAX) != ESP_OK) {
        ESP_LOGW(TAG, "cover art: the phone would not open an OBEX channel");
        s_now.art_busy = false;
    }
}

static void cover_send(void)
{
    uint16_t total = (uint16_t)((s_now.art_len + SATLINK_COVER_CHUNK_MAX - 1) /
                                SATLINK_COVER_CHUNK_MAX);
    uint8_t mime = cover_mime(s_now.art, s_now.art_len);

    ESP_LOGI(TAG, "cover art: %u bytes, %u chunks, mime %u",
             (unsigned)s_now.art_len, total, mime);

    for (uint16_t i = 0; i < total; i++) {
        size_t offset = (size_t)i * SATLINK_COVER_CHUNK_MAX;
        size_t n = s_now.art_len - offset;
        if (n > SATLINK_COVER_CHUNK_MAX) { n = SATLINK_COVER_CHUNK_MAX; }
        link_send_cover(i, total, mime, (uint32_t)s_now.art_len,
                        s_now.art + offset, (uint16_t)n);
    }
}

static void cover_on_data(const esp_avrc_ct_cb_param_t *param)
{
    if (param->cover_art_data.status != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "cover art: transfer failed");
        cover_reset();
        esp_avrc_ct_cover_art_disconnect();
        return;
    }

    size_t len = param->cover_art_data.data_len;
    if (!s_now.art_oversize && len != 0) {
        if (s_now.art_len + len > BT_COVER_MAX_BYTES) {
            /* Stop collecting but keep draining: abandoning mid-transfer would
             * leave the OBEX channel in a state the phone has to time out of. */
            ESP_LOGW(TAG, "cover art: over %d bytes, not sending it at all",
                     BT_COVER_MAX_BYTES);
            s_now.art_oversize = true;
            free(s_now.art);
            s_now.art = NULL;
            s_now.art_len = 0;
        } else {
            uint8_t *grown = realloc(s_now.art, s_now.art_len + len);
            if (grown == NULL) {
                ESP_LOGW(TAG, "cover art: out of memory at %u bytes",
                         (unsigned)s_now.art_len);
                s_now.art_oversize = true;
                free(s_now.art);
                s_now.art = NULL;
                s_now.art_len = 0;
            } else {
                memcpy(grown + s_now.art_len, param->cover_art_data.p_data, len);
                s_now.art = grown;
                s_now.art_len += len;
            }
        }
    }

    if (!param->cover_art_data.final) {
        return;
    }

    if (!s_now.art_oversize && s_now.art_len > 0) {
        cover_send();
    }
    cover_reset();
    esp_avrc_ct_cover_art_disconnect();
}

static uint8_t play_state_from_avrc(esp_avrc_playback_stat_t st)
{
    switch (st) {
    case ESP_AVRC_PLAYBACK_STOPPED:     return SATLINK_PLAY_STOPPED;
    case ESP_AVRC_PLAYBACK_PLAYING:     return SATLINK_PLAY_PLAYING;
    case ESP_AVRC_PLAYBACK_PAUSED:      return SATLINK_PLAY_PAUSED;
    case ESP_AVRC_PLAYBACK_FWD_SEEK:    return SATLINK_PLAY_FWD_SEEK;
    case ESP_AVRC_PLAYBACK_REV_SEEK:    return SATLINK_PLAY_REV_SEEK;
    default:                            return SATLINK_PLAY_STOPPED;
    }
}

static void handle_a2d_event(const bt_a2d_msg_t *msg)
{
    const esp_a2d_cb_param_t *p = &msg->param;

    switch (msg->event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        const uint8_t *bda = p->conn_stat.remote_bda;
        ESP_LOGI(TAG, "a2dp %s, %02x:%02x:%02x:%02x:%02x:%02x",
                 conn_state_name(p->conn_stat.state),
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

        switch (p->conn_stat.state) {
        case ESP_A2D_CONNECTION_STATE_CONNECTED:
            memcpy(s_now.addr, bda, sizeof(s_now.addr));
            s_now.state = SATLINK_BT_CONNECTED;
            break;
        case ESP_A2D_CONNECTION_STATE_CONNECTING:
            s_now.state = SATLINK_BT_CONNECTING;
            break;
        default:
            memset(s_now.addr, 0, sizeof(s_now.addr));
            s_now.name[0] = '\0';
            s_now.codec = SATLINK_CODEC_NONE;
            s_now.rate_hz = 0;
            s_now.state = SATLINK_BT_DISCOVERABLE;
            s_now.play_state = SATLINK_PLAY_STOPPED;
            s_now.title[0] = s_now.artist[0] = s_now.album[0] = '\0';
            link_send_track_meta("", "", "");
            break;
        }
        bt_av_report_state();
        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT: {
        bool started = (p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED);
        ESP_LOGI(TAG, "stream %s (dropped %" PRIu32 " bytes so far)",
                 started ? "started" : "suspended", audio_out_dropped_bytes());
        audio_out_set_streaming(started);
        s_now.state = started ? SATLINK_BT_STREAMING : SATLINK_BT_CONNECTED;
        bt_av_report_state();
        break;
    }

    case ESP_A2D_AUDIO_CFG_EVT: {
        const esp_a2d_mcc_t *mcc = &p->audio_cfg.mcc;
        const uint8_t *cie = (const uint8_t *)&mcc->cie;
        uint32_t rate = 0;
        uint8_t  channels = 2;
        uint8_t  src_bits = 16;

        switch (mcc->type) {
        case ESP_A2D_MCT_SBC:
            rate = sbc_rate_hz(&mcc->cie.sbc_info);
            src_bits = 16;
            s_now.codec = SATLINK_CODEC_SBC;
            ESP_LOGI(TAG, "codec SBC, %" PRIu32 " Hz, bitpool %u..%u",
                     rate, mcc->cie.sbc_info.min_bitpool, mcc->cie.sbc_info.max_bitpool);
            break;

        case ESP_A2D_MCT_M24:
            rate = aac_rate_hz(cie);
            channels = aac_channels(cie);
            src_bits = 16;
            s_now.codec = SATLINK_CODEC_AAC;
            ESP_LOGI(TAG, "codec AAC, %" PRIu32 " Hz, %u channel(s)", rate, channels);
            break;

        case ESP_A2D_MCT_NON_A2DP: {
            uint32_t vendor = aptx_vendor_id(cie);
            uint16_t codec  = aptx_codec_id(cie);
            rate     = aptx_rate_hz(cie);
            channels = aptx_channels(cie);
            /* Both aptX variants come out of libfreeaptx at 24 bits, widened to
             * int32 by the decoder. Narrowing, if the wire needs it, happens in
             * audio_out with dither rather than here by truncation. */
            src_bits = 32;

            if (vendor == 0x0000004F && codec == 0x0001) {
                s_now.codec = SATLINK_CODEC_APTX;
                ESP_LOGI(TAG, "codec aptX, %" PRIu32 " Hz, %u channel(s)", rate, channels);
            } else if (vendor == 0x000000D7 && codec == 0x0024) {
                s_now.codec = SATLINK_CODEC_APTX_HD;
                /* aptX HD carries 24 bits. They reach the host intact in 32-bit
                 * slots; in 16-bit ones audio_out narrows them with dither. */
                ESP_LOGI(TAG, "codec aptX HD, %" PRIu32 " Hz, %u channel(s), 24-bit into %u-bit slots",
                         rate, channels, audio_out_slot_bits());
            } else {
                ESP_LOGE(TAG, "vendor codec %08" PRIx32 ":%04x negotiated, which this "
                              "build cannot name to the host", vendor, codec);
                s_now.codec = SATLINK_CODEC_NONE;
                rate = 0;
            }
            break;
        }

        default:
            /* An endpoint we never registered. Say so rather than stream noise. */
            ESP_LOGE(TAG, "unexpected codec type %d", mcc->type);
            s_now.codec = SATLINK_CODEC_NONE;
            break;
        }

        if (rate != 0) {
            /* A tripwire, not a policy. The stream endpoints advertise 44.1 kHz
             * and nothing else precisely so this cannot happen; if it ever fires,
             * some endpoint's advertised capability and this promise have drifted
             * apart, and the host is about to be handed a rate it cannot follow.
             * That is inaudible in every counter this firmware keeps, so it has
             * to be said out loud. */
            if (rate != 44100 && (link_host_caps() & SATLINK_HOST_CAP_RELOCK) == 0) {
                ESP_LOGE(TAG, "negotiated %" PRIu32 " Hz, but the host was promised 44.1 kHz "
                              "and cannot relock: an endpoint is advertising a rate this "
                              "device must not offer", rate);
            }
            /* The slot width is fixed at startup from what the host said it can
             * take, not per codec: the host locks to it once. STREAM_FMT reports
             * the slot, which is what a counter on the pins measures -- BCLK over
             * WS is twice this number. */
            s_now.rate_hz  = rate;
            s_now.bits     = audio_out_slot_bits();
            s_now.channels = channels;

            /*
             * Announce, then go quiet, then move the clock. In that order.
             *
             * PROTOCOL.md asks for STREAM_FMT, then at least 50 ms of silence on
             * I2S, then samples at the new format. This used to retune first and
             * announce afterwards, which worked only because nothing is streaming
             * yet when a codec is configured -- the ring is empty and the wire is
             * carrying zeros. On a renegotiation mid-connection it would not have
             * worked, and the failure is the silent kind: the host reframes to a
             * rate it has not been told about yet.
             *
             * The host counts slot width in BCLK ticks from its own configuration,
             * so a receiver still set to the old width against the new stream
             * gathers one frame out of two and welds the channels into one word.
             * Nothing downstream can detect that; only this ordering prevents it.
             *
             * Blocking here is fine and is why this task exists: A2DP events arrive
             * on the Bluetooth stack's task and are handed over precisely so that
             * the slow parts can be slow.
             */
            bt_av_report_stream_fmt();
            audio_out_flush();
            vTaskDelay(pdMS_TO_TICKS(BT_AV_FMT_SILENCE_MS));

            audio_out_set_rate(rate);
            audio_out_set_source_bits(src_bits);
        }
        bt_av_report_state();
        break;
    }

    default:
        ESP_LOGD(TAG, "a2dp event %u", msg->event);
        break;
    }
}

static void bt_av_task(void *arg)
{
    (void)arg;

    bt_a2d_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) == pdTRUE) {
            handle_a2d_event(&msg);
        }
    }
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    bt_a2d_msg_t msg = { .event = (uint16_t)event };
    if (param != NULL) {
        memcpy(&msg.param, param, sizeof(msg.param));
    }
    /* Zero wait: the stack's task is not ours to stall. */
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped a2dp event %d", event);
    }
}

/* PCM straight from the stack's SBC decoder. Runs on the A2DP sink task, so it
 * only ever hands the bytes over and returns. */
static void a2d_data_cb(const uint8_t *buf, uint32_t len)
{
    audio_out_write(buf, len);
}

static void copy_attr(char *dst, size_t cap, const uint8_t *text, int len)
{
    if (len < 0) { len = 0; }
    size_t n = ((size_t)len < cap - 1) ? (size_t)len : cap - 1;
    memcpy(dst, text, n);
    dst[n] = '\0';
}

/* AVRCP responses are handled in place. It is all cheap, and it sidesteps the
 * trap in meta_rsp: attr_text points at memory the stack reclaims when this
 * returns, so the struct must not be queued anywhere. */
static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        s_now.avrcp_up = param->conn_stat.connected;
        ESP_LOGI(TAG, "avrcp %s", s_now.avrcp_up ? "connected" : "disconnected");
        if (s_now.avrcp_up) {
            esp_avrc_ct_send_metadata_cmd(TL_META, BT_META_ATTRS);
            /* Ask what the phone is willing to notify us about before subscribing
             * to anything: registering for an event the target does not support
             * gets rejected, and some phones stop answering after that. */
            esp_avrc_ct_send_get_rn_capabilities_cmd(TL_RN_CAPS);
        } else {
            s_now.has_play_pos = false;
            s_now.play_state = SATLINK_PLAY_STOPPED;
            s_now.art_handle[0] = '\0';
            cover_reset();
        }
        break;

    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
        esp_avrc_rn_evt_cap_mask_t caps = param->get_rn_caps_rsp.evt_set;
        bool track  = esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &caps,
                                                         ESP_AVRC_RN_TRACK_CHANGE);
        bool status = esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &caps,
                                                         ESP_AVRC_RN_PLAY_STATUS_CHANGE);
        bool volume = esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &caps,
                                                         ESP_AVRC_RN_VOLUME_CHANGE);
        s_now.has_play_pos = esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &caps,
                                                                ESP_AVRC_RN_PLAY_POS_CHANGED);

        ESP_LOGI(TAG, "peer notifies 0x%04x: track %s, status %s, position %s, volume %s",
                 caps.bits, track ? "yes" : "no", status ? "yes" : "no",
                 s_now.has_play_pos ? "yes" : "no", volume ? "yes" : "no");

        if (track)  { esp_avrc_ct_send_register_notification_cmd(TL_TRACK_CHANGE,
                                                                 ESP_AVRC_RN_TRACK_CHANGE, 0); }
        if (status) { esp_avrc_ct_send_register_notification_cmd(TL_PLAY_STATE,
                                                                 ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0); }
        if (volume) { esp_avrc_ct_send_register_notification_cmd(TL_VOLUME,
                                                                 ESP_AVRC_RN_VOLUME_CHANGE, 0); }
        if (s_now.has_play_pos) {
            esp_avrc_ct_send_register_notification_cmd(TL_PLAY_POS,
                                                       ESP_AVRC_RN_PLAY_POS_CHANGED,
                                                       POS_NOTIFY_INTERVAL_S);
        } else {
            ESP_LOGI(TAG, "no PLAY_POS bit: falling back to polling GetPlayStatus");
        }
        /* Either way, ask once now so the host is not left with nothing. */
        esp_avrc_ct_send_get_play_status_cmd(TL_PLAY_STATUS);
        break;
    }

    case ESP_AVRC_CT_METADATA_RSP_EVT:
        switch (param->meta_rsp.attr_id) {
        case ESP_AVRC_MD_ATTR_TITLE:
            copy_attr(s_now.title, sizeof(s_now.title),
                      param->meta_rsp.attr_text, param->meta_rsp.attr_length);
            break;
        case ESP_AVRC_MD_ATTR_ARTIST:
            copy_attr(s_now.artist, sizeof(s_now.artist),
                      param->meta_rsp.attr_text, param->meta_rsp.attr_length);
            break;
        case ESP_AVRC_MD_ATTR_ALBUM:
            copy_attr(s_now.album, sizeof(s_now.album),
                      param->meta_rsp.attr_text, param->meta_rsp.attr_length);
            break;
        case ESP_AVRC_MD_ATTR_COVER_ART:
            copy_attr(s_now.art_handle, sizeof(s_now.art_handle),
                      param->meta_rsp.attr_text, param->meta_rsp.attr_length);
            cover_fetch();
            break;
        case ESP_AVRC_MD_ATTR_PLAYING_TIME: {
            char buf[24];
            copy_attr(buf, sizeof(buf), param->meta_rsp.attr_text, param->meta_rsp.attr_length);
            /* strtoll, not atoi: this is the field that came back as -1000. */
            s_now.total_ms = satlink_sanitize_total_ms(strtoll(buf, NULL, 10));
            break;
        }
        default:
            break;
        }
        meta_settle_restart();
        break;

    case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT:
        s_now.total_ms      = satlink_sanitize_total_ms((int64_t)param->play_status_rsp.song_length);
        s_now.elapsed_ms    = param->play_status_rsp.song_position;
        s_now.elapsed_at_us = esp_timer_get_time();
        s_now.play_state    = play_state_from_avrc(param->play_status_rsp.play_status);
        send_track_pos();
        break;

    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        switch (param->change_ntf.event_id) {
        case ESP_AVRC_RN_TRACK_CHANGE:
            /* Notifications fire once. Re-arm or this is the last one. */
            esp_avrc_ct_send_metadata_cmd(TL_META, BT_META_ATTRS);
            esp_avrc_ct_send_get_play_status_cmd(TL_PLAY_STATUS);
            esp_avrc_ct_send_register_notification_cmd(TL_TRACK_CHANGE,
                                                       ESP_AVRC_RN_TRACK_CHANGE, 0);
            break;

        case ESP_AVRC_RN_PLAY_STATUS_CHANGE:
            s_now.play_state = play_state_from_avrc(param->change_ntf.event_parameter.playback);
            s_now.elapsed_at_us = esp_timer_get_time();
            send_track_pos();
            esp_avrc_ct_send_register_notification_cmd(TL_PLAY_STATE,
                                                       ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
            break;

        case ESP_AVRC_RN_PLAY_POS_CHANGED:
            s_now.elapsed_ms = param->change_ntf.event_parameter.play_pos;
            s_now.elapsed_at_us = esp_timer_get_time();
            send_track_pos();
            esp_avrc_ct_send_register_notification_cmd(TL_PLAY_POS,
                                                       ESP_AVRC_RN_PLAY_POS_CHANGED,
                                                       POS_NOTIFY_INTERVAL_S);
            break;

        case ESP_AVRC_RN_VOLUME_CHANGE:
            /* This is the hole the host pointed out: the phone moved the slider,
             * and without VOLUME_STATE nobody downstream would ever know. */
            s_now.volume = param->change_ntf.event_parameter.volume;
            ESP_LOGI(TAG, "volume %u, set on the phone", s_now.volume);
            link_send_volume_state(s_now.volume, 1);
            esp_avrc_ct_send_register_notification_cmd(TL_VOLUME, ESP_AVRC_RN_VOLUME_CHANGE, 0);
            break;

        default:
            ESP_LOGD(TAG, "avrcp notify 0x%02x", param->change_ntf.event_id);
            break;
        }
        break;

    case ESP_AVRC_CT_COVER_ART_STATE_EVT:
        if (param->cover_art_state.state == ESP_AVRC_COVER_ART_CONNECTED) {
            /* The linked thumbnail rather than the full image: it is the one size
             * BIP guarantees exists, and the only one this board has room for. */
            esp_avrc_ct_cover_art_get_linked_thumbnail((uint8_t *)s_now.art_handle);
        } else {
            if (s_now.art_busy) {
                ESP_LOGW(TAG, "cover art: channel closed mid-transfer");
            }
            cover_reset();
        }
        break;

    case ESP_AVRC_CT_COVER_ART_DATA_EVT:
        cover_on_data(param);
        break;

    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
        s_now.volume = param->set_volume_rsp.volume;
        ESP_LOGI(TAG, "volume %u, set from the host", s_now.volume);
        link_send_volume_state(s_now.volume, 0);
        break;

    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        ESP_LOGI(TAG, "peer avrcp features 0x%" PRIx32 ", tg flag 0x%x",
                 param->rmt_feats.feat_mask, param->rmt_feats.tg_feat_flag);
        break;

    default:
        ESP_LOGD(TAG, "avrcp event %d", event);
        break;
    }
}

/* ------------------------------------------------------- avrcp target role */

/*
 * Absolute volume, and why this device is the target for it.
 *
 * Without a target on this side the phone has nowhere to send a volume, so it
 * applies its own before encoding — which throws away resolution before the
 * audio has even left the handset, and is why Bluetooth arrived quieter than
 * everything else. With a target, the phone sends the stream at full scale and
 * tells us the number instead.
 *
 * The number is then forwarded to the host and not applied here. Attenuating in
 * this firmware would recreate the same loss one step further along; the host has
 * a DSP with headroom and is the right place to scale.
 */
static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "avrcp target %s",
                 param->conn_stat.connected ? "connected" : "disconnected");
        if (!param->conn_stat.connected) {
            s_now.volume_notify = false;
        }
        break;

    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
        s_now.volume = param->set_abs_vol.volume;
        ESP_LOGI(TAG, "volume %u, set on the phone", s_now.volume);
        /* Source 1: the peer moved it. The host scales; we do not. */
        link_send_volume_state(s_now.volume, 1);
        break;

    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            s_now.volume_notify = true;
            esp_avrc_rn_param_t rn = { .volume = s_now.volume };
            /* Interim now, changed later — that is the shape the profile wants:
             * an immediate acknowledgement carrying the current value, and a
             * second response only when it actually moves. */
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn);
        }
        break;

    case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
        ESP_LOGI(TAG, "peer avrcp ct features 0x%" PRIx32 ", flag 0x%x",
                 param->rmt_feats.feat_mask, param->rmt_feats.ct_feat_flag);
        break;

    default:
        ESP_LOGD(TAG, "avrcp tg event %d", event);
        break;
    }
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "paired with %s", param->auth_cmpl.device_name);
            strncpy(s_now.name, (const char *)param->auth_cmpl.device_name, sizeof(s_now.name) - 1);
            s_now.name[sizeof(s_now.name) - 1] = '\0';
            bt_av_report_state();
        } else {
            ESP_LOGE(TAG, "pairing failed, status %d", param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_CFM_REQ_EVT:
        /* Just works / numeric comparison. There are no buttons on this device
         * yet, so the only sane answer is yes. */
        ESP_LOGI(TAG, "confirm request, value %" PRIu32, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "power mode %d", param->mode_chg.mode);
        break;

    default:
        ESP_LOGD(TAG, "gap event %d", event);
        break;
    }
}

/* ------------------------------------------------------------ host commands */

void bt_av_transport(uint8_t cmd)
{
    static const uint8_t key[] = {
        [SATLINK_XPORT_PLAY]       = ESP_AVRC_PT_CMD_PLAY,
        [SATLINK_XPORT_PAUSE]      = ESP_AVRC_PT_CMD_PAUSE,
        [SATLINK_XPORT_STOP]       = ESP_AVRC_PT_CMD_STOP,
        [SATLINK_XPORT_NEXT]       = ESP_AVRC_PT_CMD_FORWARD,
        [SATLINK_XPORT_PREV]       = ESP_AVRC_PT_CMD_BACKWARD,
        [SATLINK_XPORT_FFWD_START] = ESP_AVRC_PT_CMD_FAST_FORWARD,
        [SATLINK_XPORT_FFWD_STOP]  = ESP_AVRC_PT_CMD_FAST_FORWARD,
        [SATLINK_XPORT_REW_START]  = ESP_AVRC_PT_CMD_REWIND,
        [SATLINK_XPORT_REW_STOP]   = ESP_AVRC_PT_CMD_REWIND,
    };

    if (cmd > SATLINK_XPORT_REW_STOP) {
        ESP_LOGW(TAG, "host sent transport command %u, which does not exist", cmd);
        return;
    }
    if (!s_now.avrcp_up) {
        ESP_LOGW(TAG, "transport command %u with no phone connected", cmd);
        return;
    }

    /* Seek is a held key: start presses, stop releases. Everything else is a
     * press followed at once by a release, which is what a tap looks like on the
     * wire. */
    bool hold_start = (cmd == SATLINK_XPORT_FFWD_START || cmd == SATLINK_XPORT_REW_START);
    bool hold_stop  = (cmd == SATLINK_XPORT_FFWD_STOP  || cmd == SATLINK_XPORT_REW_STOP);

    if (hold_stop) {
        esp_avrc_ct_send_passthrough_cmd(TL_PASSTHROUGH, key[cmd], ESP_AVRC_PT_CMD_STATE_RELEASED);
        return;
    }
    esp_avrc_ct_send_passthrough_cmd(TL_PASSTHROUGH, key[cmd], ESP_AVRC_PT_CMD_STATE_PRESSED);
    if (!hold_start) {
        esp_avrc_ct_send_passthrough_cmd(TL_PASSTHROUGH, key[cmd], ESP_AVRC_PT_CMD_STATE_RELEASED);
    }
}

void bt_av_set_volume(uint8_t volume)
{
    if (volume > 127) { volume = 127; }
    s_now.volume = volume;

    /* The host turned the knob, so this device now holds a different volume than
     * the phone last heard about. Telling it keeps the handset's own slider in
     * step, which is the whole point of absolute volume. */
    if (s_now.volume_notify) {
        esp_avrc_rn_param_t rn = { .volume = volume };
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn);
        s_now.volume_notify = false;   /* one response per registration */
    }

    ESP_LOGI(TAG, "volume %u, set from the host", volume);
    link_send_volume_state(volume, 0);
}

void bt_av_set_radio(bool on, bool discoverable, bool disconnect)
{
    if (disconnect && s_now.state >= SATLINK_BT_CONNECTED) {
        esp_a2d_sink_disconnect(s_now.addr);
    }

    if (!on) {
        esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        s_now.state = SATLINK_BT_OFF;
        ESP_LOGI(TAG, "radio off at the host's request");
    } else {
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                 discoverable ? ESP_BT_GENERAL_DISCOVERABLE
                                              : ESP_BT_NON_DISCOVERABLE);
        s_now.state = discoverable ? SATLINK_BT_DISCOVERABLE : SATLINK_BT_IDLE;
        ESP_LOGI(TAG, "radio on, %s", discoverable ? "discoverable" : "connectable only");
    }
    bt_av_report_state();
}

/* ------------------------------------------------------------------- startup */

esp_err_t bt_av_start(const char *device_name)
{
    s_now.state = SATLINK_BT_OFF;
    s_now.codec = SATLINK_CODEC_NONE;
    s_now.play_state = SATLINK_PLAY_STOPPED;

    s_queue = xQueueCreate(BT_QUEUE_DEPTH, sizeof(bt_a2d_msg_t));
    ESP_RETURN_ON_FALSE(s_queue != NULL, ESP_ERR_NO_MEM, TAG, "no memory for the event queue");
    ESP_RETURN_ON_FALSE(
        xTaskCreate(bt_av_task, "bt_av", BT_TASK_STACK, NULL, BT_TASK_PRIO, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "no memory for the bt task");

    const esp_timer_create_args_t meta_args = {
        .callback = send_track_meta, .name = "bt_meta", .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&meta_args, &s_meta_timer), TAG, "meta timer");

    const esp_timer_create_args_t pos_args = {
        .callback = pos_tick, .name = "bt_pos", .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&pos_args, &s_pos_timer), TAG, "pos timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_pos_timer, POS_PERIOD_US), TAG, "pos start");

    /* BLE is not used. Hand its controller memory back before the controller
     * starts, which is the only moment this is allowed. */
    ESP_RETURN_ON_ERROR(esp_bt_controller_mem_release(ESP_BT_MODE_BLE), TAG, "mem_release");

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    cfg.mode = ESP_BT_MODE_CLASSIC_BT;
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&cfg), TAG, "controller_init");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT), TAG, "controller_enable");

    ESP_RETURN_ON_ERROR(esp_bluedroid_init(), TAG, "bluedroid_init");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid_enable");

    ESP_RETURN_ON_ERROR(esp_bt_gap_register_callback(gap_cb), TAG, "gap_register");
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_device_name(device_name), TAG, "set_device_name");

    /* Secure Simple Pairing, no keyboard and no display: a phone pairs without
     * anyone having to type anything. */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_security_param(param_type, &iocap, sizeof(iocap)),
                        TAG, "set_security_param");

    ESP_RETURN_ON_ERROR(esp_avrc_ct_init(), TAG, "avrc_ct_init");
    ESP_RETURN_ON_ERROR(esp_avrc_ct_register_callback(avrc_ct_cb), TAG, "avrc_ct_register");

    /* The target half, which exists for exactly one reason: absolute volume. */
    ESP_RETURN_ON_ERROR(esp_avrc_tg_init(), TAG, "avrc_tg_init");
    ESP_RETURN_ON_ERROR(esp_avrc_tg_register_callback(avrc_tg_cb), TAG, "avrc_tg_register");

    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set,
                                       ESP_AVRC_RN_VOLUME_CHANGE);
    ESP_RETURN_ON_ERROR(esp_avrc_tg_set_rn_evt_cap(&evt_set), TAG, "avrc_tg_rn_cap");
    s_now.volume = 127;    /* full scale until the phone says otherwise */

    /*
     * Two decisions the host makes for us, both read from HELLO and both taken
     * here because the endpoints are about to be registered and their advertised
     * capabilities are never looked at again.
     *
     * If HELLO has not arrived yet -- the host booting later than us, say -- caps
     * read as zero and both answers are the conservative one. That is the right
     * way round to be wrong: 44.1 kHz in 16-bit slots is what every host can take,
     * and a host that wanted more will get it after the next restart.
     */
    uint32_t host_caps = link_host_caps();

    A2DP_SinkAllow48kHz((host_caps & SATLINK_HOST_CAP_RELOCK) != 0);
    if (host_caps & SATLINK_HOST_CAP_WIDE) {
        audio_out_set_slot_bits(32);
    }
    ESP_LOGI(TAG, "endpoints offer %s, %u-bit slots",
             (host_caps & SATLINK_HOST_CAP_RELOCK) ? "44.1 and 48 kHz" : "44.1 kHz only",
             audio_out_slot_bits());

    ESP_RETURN_ON_ERROR(esp_a2d_register_callback(a2d_cb), TAG, "a2d_register");
    ESP_RETURN_ON_ERROR(esp_a2d_sink_init(), TAG, "a2d_sink_init");
    /* Deprecated in 5.5.5 in favour of the external-codec path, and still the only
     * stock route to decoded PCM: the external path hands over raw frames and has
     * no decoder behind it (docs/BASE.md). */
    ESP_RETURN_ON_ERROR(esp_a2d_sink_register_data_callback(a2d_data_cb), TAG, "a2d_data_cb");

    ESP_RETURN_ON_ERROR(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE),
                        TAG, "set_scan_mode");
    s_now.state = SATLINK_BT_DISCOVERABLE;
    bt_av_report_state();

    ESP_LOGI(TAG, "\"%s\" is discoverable, A2DP sink + AVRCP controller%s",
             device_name, BT_AV_CODEC_LIST);
    return ESP_OK;
}
