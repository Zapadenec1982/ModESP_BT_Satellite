#include "bt_av.h"
#include "cover_stream.h"
#include "endpoint_policy.h"
#include "esp_heap_caps.h"
#include "satlink/cover_wire.h"

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
#include "esp_random.h"
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "satlink/proto.h"

/* What the stack will actually register stream endpoints for, so the startup line
 * says what is true of this build rather than what was true of the first one. The
 * two Kconfig options come from patches/0001; see docs/BASE.md. */
#if defined(CONFIG_BT_A2DP_APTX_DECODER) && defined(CONFIG_BT_A2DP_AAC_DECODER)
#define BT_AV_BASE_CODEC_LIST    ", SBC, AAC, aptX, aptX HD"
#elif defined(CONFIG_BT_A2DP_APTX_DECODER)
#define BT_AV_BASE_CODEC_LIST    ", SBC, aptX, aptX HD"
#elif defined(CONFIG_BT_A2DP_AAC_DECODER)
#define BT_AV_BASE_CODEC_LIST    ", SBC, AAC"
#else
#define BT_AV_BASE_CODEC_LIST    ", SBC"
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
#ifdef CONFIG_BT_A2DP_LHDCV5_DECODER
#define BT_AV_LHDC_NAME " LHDC-V5"
#else
#define BT_AV_LHDC_NAME ""
#endif
#define BT_AV_CODEC_LIST BT_AV_BASE_CODEC_LIST BT_AV_LHDC_NAME

extern void A2DP_SinkAllow48kHz(bool allow);
extern void A2DP_SinkAllow96kHz(bool allow);
extern void A2DP_SinkOfferAptxHd(bool offer);
extern void A2DP_SinkOfferAptx(bool offer);
extern void A2DP_SinkOfferAac(bool offer);

/*
 * Which stream endpoints to offer.
 *
 * A phone takes the best endpoint the sink advertises and cannot be asked for
 * less. Android 17 has no codec picker in developer options, and its HD audio
 * toggle drops all the way to SBC rather than one step down, so withholding the
 * endpoints above the one you want is the only way to reach it. The order of
 * preference on the phone is aptX HD, then aptX, then AAC, then SBC.
 *
 * Not only a test convenience: if a link ever turns out not to carry aptX HD
 * reliably, this is the knob that moves the phone down one step instead of all
 * the way to the floor.
 *
 * SBC has no switch. A2DP requires it of everyone, and it is that floor.
 *
 * All four are offered by default. Withholding is for reaching a specific codec:
 * SBC, plain aptX and AAC were each verified on hardware that way, and none of
 * them is reachable any other way on a current Android.
 */
#define BT_AV_OFFER_APTX_HD     true
#define BT_AV_OFFER_APTX        true
#define BT_AV_OFFER_AAC         true

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
 * Covers are streamed, never collected.
 *
 * This board has no PSRAM and about 47 KB of heap free while LHDC plays. The
 * 48 KB store-and-forward path that served protocol 1.5 hosts took that heap in
 * 1000-byte pieces, in competition with the decoder, and the bench showed
 * `ring full` bursts right after every "cover art ... queued" on it while the
 * 1.6 streaming path showed none (docs/BT_COVER_2026-09-20.md). The 1.6 path
 * copies each OBEX chunk straight into one bounded UART frame and keeps nothing.
 * A host that speaks less than 1.6 therefore gets no cover at all: a missing
 * image is a blank tile, a starved decoder is a click.
 */

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

/* A2DP events arrive on the Bluetooth stack's own task. Anything that can block —
 * retuning the I2S clock, for one — is handed to this task instead. */
typedef struct {
    uint16_t             event;
    esp_a2d_cb_param_t   param;
    fmt_ready_t ready;
    uint32_t codec_epoch;
    uint32_t host_caps;         /* BT_EVENT_ENDPOINTS_REGISTERED */
} bt_a2d_msg_t;

static QueueHandle_t s_queue;
#define BT_EVENT_FORMAT_READY         0xff00
#define BT_EVENT_ENDPOINTS_REGISTERED 0xff01
static ep_fsm_t s_endpoints;            /* bt_av task only */

/* How long a cold start waits for the host's HELLO before the endpoints are
 * registered from whatever has arrived. The P4 sends its first greeting from a
 * low-priority module after Wi-Fi, the display and the SD card have come up,
 * which is several seconds after this board is ready; the previous half a
 * second made a cold start of both boards a 44.1 kHz session, every time. A
 * host that is not there does not become there, and one that turns up later
 * still gets the endpoints registered again (endpoint_policy.h). */
#define BT_AV_HELLO_WAIT_MS 10000
static atomic_bool s_caps_pending;
static atomic_bool s_cover_refresh;
static atomic_bool s_cover_query;
static atomic_uint s_cover_epoch;
static uint32_t s_cover_nonce;
/* Repeats of ONE image already served, bounded by COVER_RETRY_MAX. Zeroed by a real
 * track change and by a transfer that completed -- both are evidence that the next
 * image starts from a clean link. Atomic because the link task raises it and the
 * Bluetooth callback task clears it. */
static atomic_uint s_cover_retries;
static atomic_uint s_latest_codec_epoch;
static uint32_t s_codec_epoch;
static fmt_barrier_t s_format;
static uint32_t s_nonce, s_generation, s_gate_token, s_retry_ms;
static uint8_t s_decoder_bits = 16;
static bool s_format_pending;


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
    cover_stream_t art_stream;      /* the image in flight: offset, CRC, JPEG magic */
    bool     art_connected;
    bool     art_connecting;
    bool     art_supported;
    uint32_t art_generation;
    uint32_t art_done_generation;
    char     art_request[8];
    bool     art_busy;
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

static void announce_format(void)
{
    if (s_now.rate_hz && fmt_valid(&s_format.request))
        link_send_format(&s_format.request, s_format.require_ack);
}

static void begin_format(uint32_t rate, uint8_t bits, uint8_t channels, uint8_t source_bits)
{
    s_decoder_bits = source_bits;
    s_now.rate_hz = rate; s_now.bits = bits; s_now.channels = channels;
    s_format_pending = false;
    esp_err_t err = audio_out_begin_format(&s_gate_token);
    if (err != ESP_OK) { ESP_LOGE(TAG, "cannot silence I2S: %s", esp_err_to_name(err)); return; }
    if (!s_nonce || ++s_generation == 0) {
        do { s_nonce = esp_random(); } while (!s_nonce);
        s_generation = 1;
    }
    if (rate == 96000 && !(link_host_caps() & SATLINK_HOST_CAP_96KHZ)) {
        ESP_LOGE(TAG, "host cannot accept 96 kHz; PCM stays gated");
        rate = 0;
    }
    fmt_request_t r = {s_nonce, s_generation, rate, bits, channels};
    fmt_begin(&s_format, r, (link_host_caps() & FMT_HOST_CAP) != 0);
    if (!fmt_valid(&r)) { ESP_LOGE(TAG, "unsupported stream format; PCM stays gated"); return; }
    s_format_pending = true;
    s_retry_ms = (uint32_t)(esp_timer_get_time() / 1000);
    announce_format();
}

void bt_av_apply_host_caps(void)
{
    atomic_store(&s_cover_refresh, true);
    atomic_store(&s_cover_query, true);
    atomic_store(&s_caps_pending, true);
}

void bt_av_cover_retry(uint32_t boot_nonce, uint32_t track_generation)
{
    /* s_cover_nonce is written once at start-up and never again, so reading it from
     * this task is safe; the epoch is atomic because it is not. */
    if (boot_nonce != s_cover_nonce ||
        track_generation != (uint32_t)atomic_load(&s_cover_epoch)) {
        return;     /* the phone has moved on; the next RESET brings its own image */
    }
    if (atomic_load(&s_cover_retries) >= COVER_RETRY_MAX) {
        ESP_LOGW(TAG, "cover art: %u repeats already for generation %" PRIu32 ", not asking the phone again",
                 (unsigned)COVER_RETRY_MAX, track_generation);
        return;
    }
    atomic_fetch_add(&s_cover_retries, 1);
    /* The same two flags a late HELLO raises. Re-reading the metadata rather than
     * only repeating the GET is deliberate: it refreshes the image handle too, so a
     * retry also recovers the case where the handle, and not the transfer, was the
     * problem. cover_new_track() then gives it a fresh generation, which the host
     * needs -- a repeated RESET pair is idempotent and would clear nothing. */
    atomic_store(&s_cover_refresh, true);
    atomic_store(&s_cover_query, true);
    ESP_LOGI(TAG, "cover art: host asked for generation %" PRIu32 " again", track_generation);
}

void bt_av_format_ready(const fmt_ready_t *ready)
{
    if (!s_queue) return;
    bt_a2d_msg_t msg = { .event = BT_EVENT_FORMAT_READY, .ready = *ready };
    // Losing an ACK is safe: PREPARE retries and the host repeats READY.
    xQueueSend(s_queue, &msg, 0);
}

/* Sets the stack's switches from the host's HELLO. Read while esp_a2d_sink_init()
 * registers the endpoints and never again, so it must run before every init. */
static void decide_endpoints(uint32_t host_caps)
{
    A2DP_SinkAllow48kHz((host_caps & SATLINK_HOST_CAP_RELOCK) != 0);
    A2DP_SinkAllow96kHz((host_caps & SATLINK_HOST_CAP_96KHZ) != 0);
    A2DP_SinkOfferAptxHd(BT_AV_OFFER_APTX_HD);
    A2DP_SinkOfferAptx(BT_AV_OFFER_APTX);
    A2DP_SinkOfferAac(BT_AV_OFFER_AAC);
    ESP_LOGI(TAG, "endpoints: SBC%s%s%s" BT_AV_LHDC_NAME ", %s, %u-bit slots",
             BT_AV_OFFER_AAC ? " AAC" : "",
             BT_AV_OFFER_APTX ? " aptX" : "",
             BT_AV_OFFER_APTX_HD ? " aptX-HD" : "",
             (host_caps & SATLINK_HOST_CAP_96KHZ) ? "44.1/48 kHz + LHDC 96 kHz" :
             (host_caps & SATLINK_HOST_CAP_RELOCK) ? "44.1 and 48 kHz" : "44.1 kHz only",
             audio_out_slot_bits());
}

/* No phone in any stage of attachment. Re-registering under one would drop it. */
static bool bt_av_idle(void)
{
    return s_now.state <= SATLINK_BT_DISCOVERABLE && !s_now.avrcp_up;
}

static void a2d_data_cb(const uint8_t *buf, uint32_t len);

/* Carries out what the endpoint machine asked for, and reports back to it. */
static void run_endpoint_action(ep_action_t action)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t caps = link_host_caps();
    esp_err_t err;

    switch (action) {
    case EP_ACT_DEINIT:
        ESP_LOGI(TAG, "host decides endpoints with 0x%02" PRIx32 " now, they were registered "
                      "from 0x%02" PRIx32 " and no phone is attached: registering again",
                 ep_deciding(caps), s_endpoints.decided);
        err = esp_a2d_sink_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "endpoints: deinit refused: %s", esp_err_to_name(err));
            ep_action_failed(&s_endpoints, now);
        }
        break;

    case EP_ACT_INIT:
        decide_endpoints(caps);
        err = esp_a2d_sink_init();
        if (err == ESP_OK) {
            /* The data callback survives a deinit in this stack, but a stack
             * is not a contract; registering it again costs one message. */
            err = esp_a2d_sink_register_data_callback(a2d_data_cb);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "endpoints: init refused: %s; nothing to connect to until it succeeds",
                     esp_err_to_name(err));
            ep_action_failed(&s_endpoints, now);
        } else {
            ep_init_sent(&s_endpoints, caps);
        }
        break;

    default:
        break;
    }
}

static void poll_format(void)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (atomic_exchange(&s_cover_query, false)) {
        // This task requests metadata; all cover state stays on the BTC callback task.
        if (esp_avrc_ct_send_metadata_cmd(TL_META, BT_META_ATTRS) != ESP_OK)
            atomic_store(&s_cover_refresh, false);
    }
    if (atomic_exchange(&s_caps_pending, false) && s_now.rate_hz) {
        uint32_t caps = link_host_caps();
        bool ack = (caps & FMT_HOST_CAP) != 0;
        uint8_t bits = fmt_slot_bits(s_decoder_bits, (caps & SATLINK_HOST_CAP_WIDE) != 0, ack);
        if (!fmt_valid(&s_format.request) || bits != s_now.bits || ack != s_format.require_ack ||
            (s_now.rate_hz == 96000 && !(caps & SATLINK_HOST_CAP_96KHZ)))
            begin_format(s_now.rate_hz, bits, s_now.channels, s_decoder_bits);
        else announce_format(); // a restarted host must relearn the current transaction
    }
    if (!s_format_pending) return;
    uint32_t sent_ms;
    if (!s_format.transmitted && link_format_sent(&s_format.request, &sent_ms))
        fmt_sent(&s_format, sent_ms);
    if (s_format.transmitted && !s_format.configured && !s_format.refused) {
        esp_err_t err = audio_out_configure(s_format.request.rate_hz, s_format.request.bits, s_decoder_bits);
        if (err != ESP_OK) {
            s_format.refused = true;
            ESP_LOGE(TAG, "I2S format failed; PCM stays gated: %s", esp_err_to_name(err));
        } else s_format.configured = true;
    }
    if (fmt_can_resume_source(&s_format, now, s_codec_epoch, atomic_load(&s_latest_codec_epoch)) &&
        audio_out_resume(s_gate_token)) {
        s_format_pending = false;
        ESP_LOGI(TAG, "format ready: %" PRIu32 " Hz / %u-bit, generation %" PRIu32,
                 s_now.rate_hz, s_now.bits, s_generation);
    } else if (!s_format.refused && now - s_retry_ms >= FMT_RETRY_MS) {
        s_retry_ms = now;
        announce_format();
    }
}

void bt_av_report_state(void)
{
    link_send_bt_state(s_now.state, s_now.codec, s_now.addr, s_now.name);
}

void bt_av_report_stream_fmt(void)
{
    atomic_store(&s_cover_refresh, true);
    atomic_store(&s_cover_query, true);
    atomic_store(&s_caps_pending, true);
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
    s_now.art_stream = (cover_stream_t){0};
    s_now.art_busy = false;
}

static void cover_new_track(void)
{
    // Keep draining an old OBEX response, but it cannot be sent under the new ID.
    uint32_t gen = atomic_fetch_add(&s_cover_epoch, 1) + 1;
    if (!gen) gen = atomic_fetch_add(&s_cover_epoch, 1) + 1;
    s_now.art_handle[0] = 0;
    link_send_cover_reset(s_cover_nonce, gen);
}

static void cover_fetch(void)
{
    /* COVER_STREAM_CAP rather than the bare cover bit: a 1.5 host would need
     * the collected image this build no longer keeps, so it is offered none. */
    if (s_now.art_busy || !s_now.avrcp_up || !s_now.art_supported ||
        !(link_host_caps() & COVER_STREAM_CAP)) return;
    // Android may withhold the handle until OBEX is connected. Match IDF's example:
    // connect on remote features, then request metadata again on OBEX connected.
    if (!s_now.art_connected) {
        if (!s_now.art_connecting) {
            const esp_err_t err = esp_avrc_ct_cover_art_connect(512);
            s_now.art_connecting = err == ESP_OK;
            if (err != ESP_OK) ESP_LOGW(TAG, "cover art: connect: %s", esp_err_to_name(err));
        }
        return;
    }
    if (!s_now.art_handle[0] || s_now.art_done_generation == atomic_load(&s_cover_epoch)) return;
    s_now.art_busy = true;
    s_now.art_generation = atomic_load(&s_cover_epoch);
    memcpy(s_now.art_request, s_now.art_handle, sizeof s_now.art_request);
    esp_err_t err = esp_avrc_ct_cover_art_get_linked_thumbnail((uint8_t *)s_now.art_request);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cover art: request rejected: %s", esp_err_to_name(err));
        cover_reset();
    }
}

static bool cover_emit(void *ctx, uint32_t offset, const uint8_t *data,
                       size_t len, uint32_t crc)
{
    (void)ctx;
    return s_now.art_generation==atomic_load(&s_cover_epoch) &&
           link_try_cover_stream(s_cover_nonce,s_now.art_generation,offset,data,len,crc);
}

/* Runs on the Bluetooth stack's callback task: the bytes are borrowed, so each
 * chunk is copied into its UART frame here and nothing waits. */
static void cover_on_data(const esp_avrc_ct_cb_param_t *param)
{
    if (!s_now.art_busy) return;
    if (param->cover_art_data.status != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "cover art: transfer failed");
        cover_reset(); esp_avrc_ct_cover_art_disconnect(); return;
    }
    bool stale = s_now.art_generation != atomic_load(&s_cover_epoch);
    size_t len = param->cover_art_data.data_len;
    if (!stale && !s_now.art_stream.failed &&
        !cover_stream_feed(&s_now.art_stream, param->cover_art_data.p_data, len,
                           param->cover_art_data.final, cover_emit, NULL)) {
        ESP_LOGW(TAG, "cover art: stream cancelled at %lu bytes (limit/queue/invalid JPEG)",
                 (unsigned long)s_now.art_stream.used);
    }
    if (!param->cover_art_data.final) return;
    if (!stale) {
        ESP_LOGI(TAG, "cover art: streamed %lu bytes, generation %lu, %s, heap %u",
                 (unsigned long)s_now.art_stream.used, (unsigned long)s_now.art_generation,
                 s_now.art_stream.finished ? "queued" : "discarded",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        s_now.art_done_generation = s_now.art_generation;
        /* Whatever this image cost in repeats, it arrived: the next one starts with
         * its full allowance. */
        if (s_now.art_stream.finished) atomic_store(&s_cover_retries, 0);
    }
    cover_reset();
    // Preserve the OBEX session and its image handles between tracks.
    if (stale) cover_fetch();
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
    case BT_EVENT_ENDPOINTS_REGISTERED:
        ep_registered(&s_endpoints, msg->host_caps);
        break;

    case ESP_A2D_PROF_STATE_EVT: {
        const bool deinit = p->a2d_prof_stat.init_state == ESP_A2D_DEINIT_SUCCESS;
        ESP_LOGI(TAG, "a2dp sink %s", deinit ? "deinitialised" : "initialised");
        run_endpoint_action(ep_on_prof_state(&s_endpoints, deinit));
        break;
    }

    case BT_EVENT_FORMAT_READY: {
        const bool refused_before = s_format.refused;
        fmt_ack(&s_format, &msg->ready);
        if (!refused_before && s_format.refused) {
            uint32_t ignored_token;
            audio_out_begin_format(&ignored_token);
            ESP_LOGE(TAG, "host refused format generation %" PRIu32 "; PCM stays gated", s_generation);
        }
        break;
    }
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
            s_format_pending = false;
            memset(&s_format, 0, sizeof s_format);
            uint32_t ignored_token;
            audio_out_begin_format(&ignored_token);
            audio_out_set_streaming(false);
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
        s_codec_epoch = msg->codec_epoch;
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
            audio_out_set_codec_name("SBC");
            ESP_LOGI(TAG, "codec SBC, %" PRIu32 " Hz, bitpool %u..%u",
                     rate, mcc->cie.sbc_info.min_bitpool, mcc->cie.sbc_info.max_bitpool);
            break;

        case ESP_A2D_MCT_M24:
            rate = aac_rate_hz(cie);
            channels = aac_channels(cie);
            src_bits = 16;
            s_now.codec = SATLINK_CODEC_AAC;
            audio_out_set_codec_name("AAC");
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

            if (vendor == 0x0000053a && codec == 0x4c35) {
                _Static_assert(sizeof(mcc->cie) >= 11, "LHDC V5 CIE must fit");
                const uint8_t frequency = cie[6] & 0x35;
                rate = frequency == 0x04 ? 96000 : frequency == 0x10 ? 48000 :
                       frequency == 0x20 ? 44100 : 0;
                src_bits = (cie[7] & 7) == 2 ? 32 : 16;
                channels = 2;
                s_now.codec = SATLINK_CODEC_LHDC_V5;
                audio_out_set_codec_name("LHDC V5");
                ESP_LOGI(TAG, "codec LHDC V5, %" PRIu32 " Hz, %u-bit PCM", rate, src_bits);
            } else if (vendor == 0x0000004F && codec == 0x0001) {
                s_now.codec = SATLINK_CODEC_APTX;
                audio_out_set_codec_name("aptX");
                ESP_LOGI(TAG, "codec aptX, %" PRIu32 " Hz, %u channel(s)", rate, channels);
            } else if (vendor == 0x000000D7 && codec == 0x0024) {
                s_now.codec = SATLINK_CODEC_APTX_HD;
                audio_out_set_codec_name("aptX HD");
                /* aptX HD carries 24 bits. They reach the host intact in 32-bit
                 * slots; in 16-bit ones audio_out narrows them with dither. */
                ESP_LOGI(TAG, "codec aptX HD, %" PRIu32 " Hz, %u channel(s), 24-bit into %u-bit slots",
                         rate, channels, audio_out_slot_bits());
            } else {
                ESP_LOGE(TAG, "vendor codec %08" PRIx32 ":%04x negotiated, which this "
                              "build cannot name to the host", vendor, codec);
                s_now.codec = SATLINK_CODEC_NONE;
                audio_out_set_codec_name("unknown vendor codec");
                rate = 0;
            }
            break;
        }

        default:
            /* An endpoint we never registered. Say so rather than stream noise. */
            ESP_LOGE(TAG, "unexpected codec type %d", mcc->type);
            s_now.codec = SATLINK_CODEC_NONE;
            audio_out_set_codec_name("unsupported");
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
            uint32_t caps = link_host_caps();
            bool acknowledged = (caps & FMT_HOST_CAP) != 0;
            uint8_t bits = fmt_slot_bits(src_bits, (caps & SATLINK_HOST_CAP_WIDE) != 0, acknowledged);
            begin_format(rate, bits, channels, src_bits);
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
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(10)) == pdTRUE) {
            handle_a2d_event(&msg);
        }
        poll_format();
        run_endpoint_action(ep_poll(&s_endpoints, link_host_caps(), bt_av_idle(),
                                    (uint32_t)(esp_timer_get_time() / 1000)));
    }
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    // Invalidate old control work before the next decoder callback. The epoch
    // also covers a newer CFG queued while an older event is still being handled.
    bt_a2d_msg_t msg = { .event = (uint16_t)event };
    if (event == ESP_A2D_AUDIO_CFG_EVT ||
        (event == ESP_A2D_CONNECTION_STATE_EVT && param &&
         param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED)) {
        msg.codec_epoch = atomic_fetch_add(&s_latest_codec_epoch, 1) + 1;
        audio_out_gate();
    }
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
        cover_new_track();
        if (s_now.avrcp_up) {
            esp_avrc_ct_send_metadata_cmd(TL_META, BT_META_ATTRS);
            /* Ask what the phone is willing to notify us about before subscribing
             * to anything: registering for an event the target does not support
             * gets rejected, and some phones stop answering after that. */
            esp_avrc_ct_send_get_rn_capabilities_cmd(TL_RN_CAPS);
        } else {
            s_now.has_play_pos = false;
            s_now.play_state = SATLINK_PLAY_STOPPED;
            s_now.art_connected = false;
            s_now.art_connecting = false;
            s_now.art_supported = false;
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
        if (atomic_exchange(&s_cover_refresh, false)) {
            cover_new_track();
            cover_fetch();
        }
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
            if (param->meta_rsp.attr_length != 7 || !param->meta_rsp.attr_text) {
                if (s_now.art_handle[0]) cover_new_track();
                ESP_LOGI(TAG, "cover art: no image handle for this track");
                break;
            }
            if (s_now.art_handle[0] && memcmp(s_now.art_handle, param->meta_rsp.attr_text, 7))
                cover_new_track();
            copy_attr(s_now.art_handle, sizeof(s_now.art_handle),
                      param->meta_rsp.attr_text, param->meta_rsp.attr_length);
            ESP_LOGI(TAG, "cover art: handle %.7s", s_now.art_handle);
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
            /* A new track is a new image, and its allowance is its own. Only here,
             * not in cover_new_track(): a retry goes through that function too, and
             * refilling there would make the bound mean nothing. */
            atomic_store(&s_cover_retries, 0);
            cover_new_track();
            s_now.title[0] = s_now.artist[0] = s_now.album[0] = 0;
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
        s_now.art_connecting = false;
        s_now.art_connected = param->cover_art_state.state == ESP_AVRC_COVER_ART_CONNECTED;
        if (s_now.art_connected) {
            ESP_LOGI(TAG, "cover art: OBEX connected");
            esp_avrc_ct_send_metadata_cmd(TL_META, BT_META_ATTRS);
        } else if (!s_now.art_connected) {
            if (s_now.art_busy) ESP_LOGW(TAG, "cover art: channel closed, reason %d", param->cover_art_state.reason);
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
        s_now.art_supported = (param->rmt_feats.tg_feat_flag & ESP_AVRC_FEAT_FLAG_TG_COVER_ART) != 0;
        ESP_LOGI(TAG, "cover art: peer support %s", s_now.art_supported ? "yes" : "no");
        cover_fetch();
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
    do { s_cover_nonce = esp_random(); } while (!s_cover_nonce);
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
     * before the endpoints are registered, because their advertised capabilities
     * are read at registration and never again.
     *
     * If HELLO has not arrived when the wait runs out, caps read as zero and both
     * answers are the conservative one. That is the right way round to be wrong:
     * 44.1 kHz in 16-bit slots is what every host can take. It is no longer a
     * sentence for the session, though: a greeting that arrives later, with
     * wider capabilities, has the endpoints registered again as soon as no
     * phone is attached (endpoint_policy.h).
     */
    link_wait_for_host(BT_AV_HELLO_WAIT_MS);
    uint32_t host_caps = link_host_caps();
    decide_endpoints(host_caps);
    bt_av_apply_host_caps();

    ESP_RETURN_ON_ERROR(esp_a2d_register_callback(a2d_cb), TAG, "a2d_register");
    ESP_RETURN_ON_ERROR(esp_a2d_sink_init(), TAG, "a2d_sink_init");
    /* Deprecated in 5.5.5 in favour of the external-codec path, and still the only
     * stock route to decoded PCM: the external path hands over raw frames and has
     * no decoder behind it (docs/BASE.md). */
    ESP_RETURN_ON_ERROR(esp_a2d_sink_register_data_callback(a2d_data_cb), TAG, "a2d_data_cb");

    /* Hand the endpoint machine its starting point on its own task, so that it
     * never races the registration above. */
    bt_a2d_msg_t registered = { .event = BT_EVENT_ENDPOINTS_REGISTERED, .host_caps = host_caps };
    ESP_RETURN_ON_FALSE(xQueueSend(s_queue, &registered, pdMS_TO_TICKS(100)) == pdTRUE,
                        ESP_ERR_NO_MEM, TAG, "event queue full at start");

    ESP_RETURN_ON_ERROR(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE),
                        TAG, "set_scan_mode");
    s_now.state = SATLINK_BT_DISCOVERABLE;
    bt_av_report_state();

    ESP_LOGI(TAG, "\"%s\" is discoverable, A2DP sink + AVRCP controller%s",
             device_name, BT_AV_CODEC_LIST);
    return ESP_OK;
}
