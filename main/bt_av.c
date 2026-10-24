#include "bt_av.h"
#include "avrc_volume.h"
#include "bond_table.h"
#include "bt_settings.h"
#include "cover_stream.h"
#include "endpoint_policy.h"
#include "esp_heap_caps.h"
#include "format_policy.h"
#include "pair_policy.h"
#include "pause_policy.h"
#include "reconnect_policy.h"
#include "satlink/cover_wire.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "audio_out.h"
#include "link.h"
#include "nvs.h"

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
#include "freertos/semphr.h"
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
extern void A2DP_SinkOfferLhdcV5(bool offer);

/*
 * Stock Bluedroid, declared here for the same reason as the switches above: the
 * stack's own header is not on the application's include path, and ESP-IDF has no
 * API for it. esp_bt_gap_set_scan_mode() leaves it alone (it passes BTA_DM_IGNORE),
 * so what is set here stays set.
 *
 * `connect_only_paired` stays false. It admits only link keys made with MITM
 * protection, and every bond made before protocol 1.8 was made without.
 */
extern void BTM_SetPairableMode(bool allow_pairing, bool connect_only_paired);

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

/*
 * The codecs this build can offer, as BT_CONFIG counts them (protocol 1.8): a
 * decoder that is built in and not withheld above. The host switches these off
 * and on; the rest it is never shown.
 */
#if defined(CONFIG_BT_A2DP_AAC_DECODER)
#define BT_AV_BUILD_AAC     (BT_AV_OFFER_AAC ? SATLINK_CODEC_BIT_AAC : 0u)
#else
#define BT_AV_BUILD_AAC     0u
#endif
#if defined(CONFIG_BT_A2DP_APTX_DECODER)
#define BT_AV_BUILD_APTX    ((BT_AV_OFFER_APTX ? SATLINK_CODEC_BIT_APTX : 0u) | \
                             (BT_AV_OFFER_APTX_HD ? SATLINK_CODEC_BIT_APTX_HD : 0u))
#else
#define BT_AV_BUILD_APTX    0u
#endif
#if defined(CONFIG_BT_A2DP_LHDCV5_DECODER)
#define BT_AV_BUILD_LHDC    SATLINK_CODEC_BIT_LHDC_V5
#else
#define BT_AV_BUILD_LHDC    0u
#endif
#define BT_AV_BUILD_CODECS  ((uint8_t)(SATLINK_CODEC_BIT_SBC | BT_AV_BUILD_AAC | \
                                       BT_AV_BUILD_APTX | BT_AV_BUILD_LHDC))

static const char *TAG = "sat.bt";

/* 3584 until protocol 1.8, which added the settings, the bonds and their NVS
 * writes to this task. */
#define BT_TASK_STACK       4096
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
 * 1.6 streaming path showed none (bench, 20.09.2026). The 1.6 path
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
    uint8_t  codecs;            /*   ... and the codecs it offered */
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
static bool s_fmt_slave;                /* the I2S role the current transaction was begun in */
static atomic_bool s_cover_refresh;
static atomic_bool s_cover_query;
static atomic_uint s_cover_epoch;
static uint32_t s_cover_nonce;
/* Repeats of ONE image already served, bounded by COVER_RETRY_MAX (cover_stream.h).
 * Refilled by a real track change and by a transfer that completed and that the host
 * did not then ask for again. Under a lock because the link task raises it and the
 * Bluetooth callback task refills it. */
static cover_retry_t s_cover_retries;
static portMUX_TYPE s_cover_retry_mux = portMUX_INITIALIZER_UNLOCKED;
static atomic_uint s_latest_codec_epoch;
static uint32_t s_codec_epoch;
static fmt_barrier_t s_format;
static uint32_t s_nonce, s_generation, s_gate_token, s_retry_ms;
static uint8_t s_decoder_bits = 16;
static bool s_format_pending;

/*
 * Protocol 1.8: the settings the host shows, the bonds it lists, the pairing it is
 * asked about, and the call to the last phone.
 *
 * Loaded by bt_av_start before this task exists, and from then on this task's
 * alone. The link task and the stack's callback task hand it events through
 * s_cfg_queue and touch none of it; nothing is taken from the queue until the
 * stack is up (s_stack_up), so a host that greets during the start waits there.
 */
#define BT_NVS_NAMESPACE    "sat_bt"
#define BT_NVS_SETTINGS     "settings"
#define BT_NVS_BONDS        "bonds"
#define BT_CFG_QUEUE_DEPTH  8

static bt_settings_t s_settings;
static bond_table_t  s_bonds;
static bool          s_bonds_dirty;     /* saved when no stream is playing */
static pair_fsm_t    s_pair;
static rc_fsm_t      s_reconnect;
static uint8_t       s_io_cap = 0xFF;   /* what the stack was last told, 0xFF = nothing */
/* What the radio is doing now. The settings, except after a CMD_RADIO, which acts
 * as it did in 1.7 and is not saved. */
static bool          s_live_radio, s_live_disc;
static QueueHandle_t s_cfg_queue;
static atomic_bool   s_stack_up;


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
    bool     elapsed_unknown;       /* the phone gave no position: send 0, never count up */

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
    bool     art_drain_failed;      /* the old track's drain failed: refetch on close */
} s_now;

static esp_timer_handle_t s_meta_timer;

/* Title, artist and album are written on the BTC task, read by the meta timer on the
 * esp_timer task and cleared by the bt_av task. s_meta_mux guards the three strings and
 * s_meta_live (A2DP is up, so TRACK_META may go out): a spinlock, because a Bluetooth
 * callback must not wait. s_meta_send keeps the timer's send and the empty one on
 * disconnect in order, so a send already under way cannot land after the empty one. */
static portMUX_TYPE s_meta_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_meta_live;
static SemaphoreHandle_t s_meta_send;
static esp_timer_handle_t s_pos_timer;

/* How long a phone gets to act on the host's PAUSE before the stream itself is suspended.
 *
 * The host pauses the phone whenever it listens to another source. Some phones show the
 * pause and keep streaming anyway: on 05.10.2026 a Pixel 6 Pro streamed LHDC on for
 * minutes while the host re-sent PAUSE every 3 s, and that live stream next to the panel's
 * Wi-Fi cost AirPlay 24 gaps of up to 280 ms in 90 s, none once the phone was gone. A
 * stream still running this long after a PAUSE is suspended at AVDTP level (customer's
 * decision, 05.10.2026): the radio goes quiet and the phone stays connected.
 *
 * Five seconds, not the 1.5 this started with. A phone that obeys a pause still keeps its
 * stream open for a few seconds of silence before closing it itself (Android's output goes
 * to standby after 3 s), and suspending it in that window is not harmless: Android then
 * holds the stream as suspended by the remote and will not start it again from its own play
 * button. On 05.10.2026 that was a phone playing, its clock running, and no sound until
 * Bluetooth was switched off and on. */
#define PAUSE_GRACE_US  (5000 * 1000)
static esp_timer_handle_t s_pause_timer;

/* Set when pause_check() suspended the stream, so that this device, which stopped it, is
 * the one that starts it again (resume_own_suspend()). Cleared when the stream starts and
 * when the phone connects or goes. */
static atomic_bool s_own_suspend;
static void resume_own_suspend(const char *why);

static void pause_timer_apply(pause_timer_op_t op)
{
    if (op == PAUSE_TIMER_START) {
        esp_timer_start_once(s_pause_timer, PAUSE_GRACE_US);
    } else if (op == PAUSE_TIMER_STOP) {
        esp_timer_stop(s_pause_timer);  /* ESP_ERR_INVALID_STATE when none is pending */
    }
}

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
    s_fmt_slave = audio_out_is_slave();   /* before the gate: a later role change is seen */
    s_format_pending = false;
    esp_err_t err = audio_out_begin_format(&s_gate_token);
    if (err != ESP_OK) { ESP_LOGE(TAG, "cannot silence I2S: %s", esp_err_to_name(err)); return; }
    if (!s_nonce || ++s_generation == 0) {
        do { s_nonce = esp_random(); } while (!s_nonce);
        s_generation = 1;
    }
    if (!ep_host_can_follow(rate, link_host_caps())) {
        /* 48 kHz used to play on to a host without RELOCK, announced or not; only
         * 96 kHz was held. Silence until a host that can follow greets, or the
         * phone reconnects to endpoints that offer only what this host takes. */
        ESP_LOGE(TAG, "host cannot follow %" PRIu32 " Hz; PCM stays gated", rate);
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
    portENTER_CRITICAL(&s_cover_retry_mux);
    const bool take = cover_retry_take(&s_cover_retries, track_generation);
    portEXIT_CRITICAL(&s_cover_retry_mux);
    if (!take) {
        ESP_LOGW(TAG, "cover art: %u repeats already for generation %" PRIu32 ", not asking the phone again",
                 (unsigned)COVER_RETRY_MAX, track_generation);
        return;
    }
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

/* Sets the stack's switches from the host's HELLO and the codecs the settings
 * leave on. Read while esp_a2d_sink_init() registers the endpoints and never
 * again, so it must run before every init. */
static void decide_endpoints(uint32_t host_caps, uint8_t codecs)
{
    A2DP_SinkAllow48kHz((host_caps & SATLINK_HOST_CAP_RELOCK) != 0);
    A2DP_SinkAllow96kHz((host_caps & SATLINK_HOST_CAP_96KHZ) != 0);
    A2DP_SinkOfferAptxHd((codecs & SATLINK_CODEC_BIT_APTX_HD) != 0);
    A2DP_SinkOfferAptx((codecs & SATLINK_CODEC_BIT_APTX) != 0);
    A2DP_SinkOfferAac((codecs & SATLINK_CODEC_BIT_AAC) != 0);
    A2DP_SinkOfferLhdcV5((codecs & SATLINK_CODEC_BIT_LHDC_V5) != 0);
    ESP_LOGI(TAG, "endpoints: SBC%s%s%s%s, %s, %u-bit slots",
             (codecs & SATLINK_CODEC_BIT_AAC) ? " AAC" : "",
             (codecs & SATLINK_CODEC_BIT_APTX) ? " aptX" : "",
             (codecs & SATLINK_CODEC_BIT_APTX_HD) ? " aptX-HD" : "",
             (codecs & SATLINK_CODEC_BIT_LHDC_V5) ? " LHDC-V5" : "",
             (host_caps & SATLINK_HOST_CAP_96KHZ) ? "44.1/48 kHz + LHDC 96 kHz" :
             (host_caps & SATLINK_HOST_CAP_RELOCK) ? "44.1 and 48 kHz" : "44.1 kHz only",
             audio_out_slot_bits());
}

/* The codecs to offer a phone: the build's, less what the host switched off. */
static uint8_t offered_codecs(void)
{
    return bt_codecs_enabled(&s_settings, BT_AV_BUILD_CODECS);
}

/* No phone in any stage of attachment. Re-registering under one would drop it. */
static bool bt_av_idle(void)
{
    return s_now.state <= SATLINK_BT_DISCOVERABLE && !s_now.avrcp_up;
}

/* What BT_STATE says with no phone: the radio as it is right now. */
static uint8_t idle_state(void)
{
    return !s_live_radio ? SATLINK_BT_OFF :
           s_live_disc   ? SATLINK_BT_DISCOVERABLE : SATLINK_BT_IDLE;
}

/* Protocol 1.8, further down: the bonds hear about phones coming and going. */
static void cfg_phone_connected(const uint8_t addr[6]);
static void cfg_phone_gone(void);

static void a2d_data_cb(const uint8_t *buf, uint32_t len);

/* Carries out what the endpoint machine asked for, and reports back to it. */
static void run_endpoint_action(ep_action_t action)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t caps = link_host_caps();
    const uint8_t codecs = offered_codecs();
    esp_err_t err;

    switch (action) {
    case EP_ACT_DEINIT:
        ESP_LOGI(TAG, "endpoints want caps 0x%02" PRIx32 " and codecs 0x%02x now, were registered "
                      "from 0x%02" PRIx32 " and 0x%02x, and no phone is attached: registering again",
                 ep_deciding(caps), codecs, s_endpoints.decided, s_endpoints.codecs);
        err = esp_a2d_sink_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "endpoints: deinit refused: %s", esp_err_to_name(err));
            ep_action_failed(&s_endpoints, now);
        }
        break;

    case EP_ACT_INIT:
        decide_endpoints(caps, codecs);
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
            ep_init_sent(&s_endpoints, caps, codecs);
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
    /* audio_out_set_role runs on the link task and gates PCM; the new role is followed
     * here with a new transaction, even when the host's greeting has not been seen yet. */
    const bool caps_pending = atomic_exchange(&s_caps_pending, false);
    const bool slave = audio_out_is_slave();
    if ((caps_pending || slave != s_fmt_slave) && s_now.rate_hz) {
        uint32_t caps = link_host_caps();
        bool ack = (caps & FMT_HOST_CAP) != 0;
        /* 1.9: a host that clocks the link does it in 32-bit slots, whatever the decoder. */
        uint8_t bits = slave ? 32
                     : fmt_slot_bits(s_decoder_bits, (caps & SATLINK_HOST_CAP_WIDE) != 0, ack);
        if (fmt_needs_restart(fmt_valid(&s_format.request), s_now.bits, bits,
                              s_format.require_ack, ack, slave != s_fmt_slave,
                              ep_host_can_follow(s_now.rate_hz, caps)))
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

/* On the bt_av task only, which writes every field it reads: the link's task asks for
 * one through bt_av_request_state(). */
static void bt_av_report_state(void)
{
    link_send_bt_state(s_now.state, s_now.codec, s_now.addr, s_now.name);
}

static void send_track_meta(void *arg)
{
    (void)arg;
    /* A snapshot, so an attribute copied in meanwhile cannot tear what is encoded. Static:
     * only the esp_timer task runs this, and its stack is small. */
    static char snap[3][sizeof s_now.title];
    xSemaphoreTake(s_meta_send, portMAX_DELAY);
    portENTER_CRITICAL(&s_meta_mux);
    const bool live = s_meta_live;
    memcpy(snap[0], s_now.title, sizeof snap[0]);
    memcpy(snap[1], s_now.artist, sizeof snap[1]);
    memcpy(snap[2], s_now.album, sizeof snap[2]);
    portEXIT_CRITICAL(&s_meta_mux);
    if (live) {
        link_send_track_meta(snap[0], snap[1], snap[2]);
    }
    xSemaphoreGive(s_meta_send);
    if (live) {
        ESP_LOGI(TAG, "meta: \"%s\" / \"%s\" / \"%s\"", snap[0], snap[1], snap[2]);
    }
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
    if (s_now.play_state == SATLINK_PLAY_PLAYING && s_now.elapsed_at_us != 0
        && !s_now.elapsed_unknown) {
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
    bool stale = s_now.art_generation != atomic_load(&s_cover_epoch);
    if (param->cover_art_data.status != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "cover art: transfer failed");
        // The new track's fetch waits for the close: the channel is still up here.
        s_now.art_drain_failed = stale;
        cover_reset(); esp_avrc_ct_cover_art_disconnect(); return;
    }
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
        /* Whatever this image cost in repeats, it was sent whole: the next one starts
         * with its full allowance -- unless the host names this generation in a
         * retry, which takes the refill back. */
        if (s_now.art_stream.finished) {
            portENTER_CRITICAL(&s_cover_retry_mux);
            cover_retry_completed(&s_cover_retries, s_now.art_generation);
            portEXIT_CRITICAL(&s_cover_retry_mux);
        }
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
        ep_registered(&s_endpoints, msg->host_caps, msg->codecs);
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
        atomic_store(&s_own_suspend, false);    /* a suspend belongs to the link it was on */

        switch (p->conn_stat.state) {
        case ESP_A2D_CONNECTION_STATE_CONNECTED: {
            memcpy(s_now.addr, bda, sizeof(s_now.addr));
            s_now.state = SATLINK_BT_CONNECTED;
            cfg_phone_connected(bda);
            /* Metadata from a phone whose AVRCP came up first was kept, not sent. */
            portENTER_CRITICAL(&s_meta_mux);
            s_meta_live = true;
            const bool have_meta = s_now.title[0] || s_now.artist[0] || s_now.album[0];
            portEXIT_CRITICAL(&s_meta_mux);
            if (have_meta) {
                meta_settle_restart();
            }
            break;
        }
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
            s_now.state = idle_state();
            s_now.play_state = SATLINK_PLAY_STOPPED;
            /* Nothing non-empty may follow the empty TRACK_META: not a settle already
             * pending, nor one armed by an attribute that arrives after this. */
            esp_timer_stop(s_meta_timer);
            xSemaphoreTake(s_meta_send, portMAX_DELAY);
            portENTER_CRITICAL(&s_meta_mux);
            s_meta_live = false;
            s_now.title[0] = s_now.artist[0] = s_now.album[0] = '\0';
            portEXIT_CRITICAL(&s_meta_mux);
            link_send_track_meta("", "", "");
            xSemaphoreGive(s_meta_send);
            break;
        }
        bt_av_report_state();
        if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            cfg_phone_gone();
        }
        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT: {
        bool started = (p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED);
        ESP_LOGI(TAG, "stream %s (dropped %" PRIu32 " bytes so far)",
                 started ? "started" : "suspended", audio_out_dropped_bytes());
        audio_out_set_streaming(started);
        s_now.state = started ? SATLINK_BT_STREAMING : SATLINK_BT_CONNECTED;
        pause_timer_apply(pause_on_stream(started));
        if (started) {
            atomic_store(&s_own_suspend, false);
        }
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
            /* A rate the host cannot follow -- endpoints decided for an earlier
             * host, or an endpoint advertising what it must not -- is held gated
             * by begin_format, which says so. */
            uint32_t caps = link_host_caps();
            bool acknowledged = (caps & FMT_HOST_CAP) != 0;
            uint8_t bits = audio_out_is_slave() ? 32
                         : fmt_slot_bits(src_bits, (caps & SATLINK_HOST_CAP_WIDE) != 0, acknowledged);
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

static void cfg_poll(void);

static void bt_av_task(void *arg)
{
    (void)arg;

    bt_a2d_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(10)) == pdTRUE) {
            handle_a2d_event(&msg);
        }
        poll_format();
        if (atomic_load(&s_stack_up)) {
            cfg_poll();
        }
        run_endpoint_action(ep_poll(&s_endpoints, link_host_caps(), offered_codecs(), bt_av_idle(),
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

/* Cut on a character boundary: the phone's text is UTF-8, and a title cut through
 * its last character went to the host as invalid UTF-8 in TRACK_META. */
static size_t attr_fit(size_t cap, const uint8_t *text, int len)
{
    if (len < 0) { len = 0; }
    size_t n = ((size_t)len < cap - 1) ? (size_t)len : cap - 1;
    if (n < (size_t)len) {
        n = satlink_utf8_prefix((const char *)text, n);
    }
    return n;
}

static void copy_attr(char *dst, size_t cap, const uint8_t *text, int len)
{
    size_t n = attr_fit(cap, text, len);
    memcpy(dst, text, n);
    dst[n] = '\0';
}

/* Title, artist and album: the meta timer reads them on its own task. */
static void copy_meta(char *dst, size_t cap, const uint8_t *text, int len)
{
    size_t n = attr_fit(cap, text, len);
    portENTER_CRITICAL(&s_meta_mux);
    memcpy(dst, text, n);
    dst[n] = '\0';
    portEXIT_CRITICAL(&s_meta_mux);
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
            s_now.art_drain_failed = false;
            cover_reset();
            /* So that a late refill cannot carry over into the next connection. */
            esp_timer_stop(s_meta_timer);
            portENTER_CRITICAL(&s_meta_mux);
            s_now.title[0] = s_now.artist[0] = s_now.album[0] = '\0';
            portEXIT_CRITICAL(&s_meta_mux);
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
            copy_meta(s_now.title, sizeof(s_now.title),
                      param->meta_rsp.attr_text, param->meta_rsp.attr_length);
            break;
        case ESP_AVRC_MD_ATTR_ARTIST:
            copy_meta(s_now.artist, sizeof(s_now.artist),
                      param->meta_rsp.attr_text, param->meta_rsp.attr_length);
            break;
        case ESP_AVRC_MD_ATTR_ALBUM:
            copy_meta(s_now.album, sizeof(s_now.album),
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
        s_now.elapsed_unknown = !satlink_elapsed_known(param->play_status_rsp.song_position);
        s_now.elapsed_ms    = satlink_sanitize_elapsed_ms(param->play_status_rsp.song_position);
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
            portENTER_CRITICAL(&s_cover_retry_mux);
            cover_retry_track_change(&s_cover_retries);
            portEXIT_CRITICAL(&s_cover_retry_mux);
            cover_new_track();
            portENTER_CRITICAL(&s_meta_mux);
            s_now.title[0] = s_now.artist[0] = s_now.album[0] = 0;
            portEXIT_CRITICAL(&s_meta_mux);
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
            /* A phone that plays again after the host's PAUSE was resumed by its user:
             * suspending it when the wait runs out would leave it playing with no sound. */
            pause_timer_apply(pause_on_play_status(s_now.play_state));
            /* Only a change, never a polled status: a phone that ignored the PAUSE answers
             * "playing" to every poll, and starting on that would undo the suspend at once. */
            if (s_now.play_state == SATLINK_PLAY_PLAYING) {
                resume_own_suspend("the phone plays again");
            }
            esp_avrc_ct_send_register_notification_cmd(TL_PLAY_STATE,
                                                       ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
            break;

        case ESP_AVRC_RN_PLAY_POS_CHANGED:
            s_now.elapsed_unknown = !satlink_elapsed_known(param->change_ntf.event_parameter.play_pos);
            s_now.elapsed_ms = satlink_sanitize_elapsed_ms(param->change_ntf.event_parameter.play_pos);
            s_now.elapsed_at_us = esp_timer_get_time();
            send_track_pos();
            esp_avrc_ct_send_register_notification_cmd(TL_PLAY_POS,
                                                       ESP_AVRC_RN_PLAY_POS_CHANGED,
                                                       POS_NOTIFY_INTERVAL_S);
            break;

        case ESP_AVRC_RN_VOLUME_CHANGE:
            /* This is the hole the host pointed out: the phone moved the slider,
             * and without VOLUME_STATE nobody downstream would ever know. */
            s_now.volume = avrc_abs_volume(param->change_ntf.event_parameter.volume);
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
            /* One go per failure, so a connect that fails as well ends it. cover_fetch()
             * connects again, and OBEX connected re-reads the metadata for the GET. */
            const bool refetch = cover_refetch_on_close(
                s_now.art_drain_failed, s_now.art_busy,
                s_now.art_generation != atomic_load(&s_cover_epoch), s_now.art_handle[0] != 0);
            s_now.art_drain_failed = false;
            cover_reset();
            if (refetch) cover_fetch();
        }
        break;

    case ESP_AVRC_CT_COVER_ART_DATA_EVT:
        cover_on_data(param);
        break;

    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
        s_now.volume = avrc_abs_volume(param->set_volume_rsp.volume);
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
        s_now.volume = avrc_abs_volume(param->set_abs_vol.volume);
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

/* ------------------------------------------- protocol 1.8: settings and bonds */

#define BDA_FMT         "%02x:%02x:%02x:%02x:%02x:%02x"
#define BDA_ARGS(a)     (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]

typedef enum {
    CFG_HELLO = 1,      /* the host greeted: pairing mode, and the 1.8 state for a 1.8 host */
    CFG_REFRESH,        /* BT_REFRESH */
    CFG_SET,            /* BT_CONFIG_SET */
    CFG_REMOVE,         /* BOND_REMOVE */
    CFG_REPLY,          /* PAIR_REPLY */
    CFG_RADIO,          /* CMD_RADIO, protocol 1.0 */
    CFG_CONFIRM,        /* ESP_BT_GAP_CFM_REQ_EVT */
    CFG_PASSKEY,        /* ESP_BT_GAP_KEY_NOTIF_EVT */
    CFG_AUTH,           /* ESP_BT_GAP_AUTH_CMPL_EVT */
    CFG_NAME,           /* ESP_BT_GAP_READ_REMOTE_NAME_EVT */
    CFG_UNBONDED,       /* ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT */
    CFG_STATE,          /* BT_STATE after a host HELLO */
} cfg_event_t;

typedef struct {
    uint8_t  type;              /* cfg_event_t */
    uint8_t  addr[6];
    bool     ok;                /* AUTH, NAME, UNBONDED: success; REPLY: accept;
                                   CONFIRM: Just Works */
    uint8_t  set[4];            /* SET: radio, discoverable, auto_reconnect, codecs_enabled;
                                   RADIO: on, discoverable, disconnect */
    uint32_t value;             /* CONFIRM: the number; PASSKEY: the passkey */
    bool     name_refused;      /* SET: longer than any name the device takes */
    uint8_t  name_len;
    char     name[SATLINK_BT_NAME_MAX + 1];
} cfg_msg_t;

/* False before bt_av_start made the queue, which is not worth a word: what the
 * host said then is answered when the task starts (cfg_poll). */
static bool cfg_post(const cfg_msg_t *m)
{
    if (s_cfg_queue == NULL) {
        return false;
    }
    if (xQueueSend(s_cfg_queue, m, 0) == pdTRUE) {
        return true;
    }
    ESP_LOGW(TAG, "bt settings: queue full, dropped event %u", m->type);
    return false;
}

/* A name from the stack: up to 248 bytes, meant to be UTF-8. Kept to 32 bytes on
 * a character boundary, and dropped when it is not text: the host shows it. */
static void cfg_take_name(cfg_msg_t *m, const uint8_t *name)
{
    size_t len = strnlen((const char *)name, ESP_BT_GAP_MAX_BDNAME_LEN);
    len = bt_utf8_clip((const char *)name, len, SATLINK_BT_NAME_MAX);
    if (!bt_utf8_valid((const char *)name, len)) {
        len = 0;
    }
    memcpy(m->name, name, len);
    m->name[len] = '\0';
    m->name_len = (uint8_t)len;
}

static bool host_confirms(void)
{
    return (link_host_caps() & SATLINK_HOST_CAP_PAIR_CONFIRM) != 0;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- NVS. Two records, each written whole, each read back only if it is
 * exactly what this build writes; anything else is the factory state. */

static bool nvs_save(const char *key, const void *blob, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BT_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, key, blob, len);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bt settings: cannot save %s: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static size_t nvs_load(const char *key, void *blob, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(BT_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;       /* first boot: the namespace does not exist yet */
    }
    size_t len = cap;
    esp_err_t err = nvs_get_blob(h, key, blob, &len);
    nvs_close(h);
    return err == ESP_OK ? len : 0;
}

static void save_settings(void)
{
    uint8_t blob[BT_SETTINGS_BLOB_MAX];
    size_t len = bt_settings_pack(&s_settings, blob, sizeof(blob));
    if (len != 0) {
        nvs_save(BT_NVS_SETTINGS, blob, len);
    }
}

/* Deferred while a stream plays: a flash write stalls both cores' caches, and the
 * bonds' order and names can wait for a pause. The bonds themselves are the
 * stack's and are saved by it. */
static bool save_bonds(void)
{
    uint8_t *blob = malloc(BOND_TABLE_BLOB_MAX);
    if (blob == NULL) {
        return false;
    }
    size_t len = bond_table_pack(&s_bonds, blob, BOND_TABLE_BLOB_MAX);
    if (len != 0 && nvs_save(BT_NVS_BONDS, blob, len)) {
        s_bonds_dirty = false;
        /* The deepest this task goes is here or in a BOND_LIST, and its stack
         * grew for protocol 1.8: say what is left of it. */
        ESP_LOGI(TAG, "bond names saved, %u of them; bt task stack headroom %u bytes",
                 s_bonds.count, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    free(blob);
    return !s_bonds_dirty;
}

/* Before the task exists: the settings decide the name, the endpoints and the
 * scan mode the stack starts with. */
static void load_settings(const char *factory_name)
{
    bt_settings_defaults(&s_settings, factory_name);
    uint8_t blob[BT_SETTINGS_BLOB_MAX];
    size_t len = nvs_load(BT_NVS_SETTINGS, blob, sizeof(blob));
    if (len != 0 && !bt_settings_unpack(&s_settings, blob, len)) {
        ESP_LOGW(TAG, "bt settings: stored record not understood, factory state");
    }

    uint8_t *bonds = malloc(BOND_TABLE_BLOB_MAX);
    if (bonds != NULL) {
        len = nvs_load(BT_NVS_BONDS, bonds, BOND_TABLE_BLOB_MAX);
        if (len != 0 && !bond_table_unpack(&s_bonds, bonds, len)) {
            ESP_LOGW(TAG, "bt settings: stored bond names not understood, starting without");
        }
        free(bonds);
    }
    s_live_radio = s_settings.radio != 0;
    s_live_disc = s_settings.discoverable != 0;
    ESP_LOGI(TAG, "bt settings: radio %u, discoverable %u, reconnect %u, codecs 0x%02x of 0x%02x, "
                  "name \"%s\", %u bond name(s)",
             s_settings.radio, s_settings.discoverable, s_settings.auto_reconnect,
             offered_codecs(), BT_AV_BUILD_CODECS, s_settings.name, s_bonds.count);
}

/* ---- The stack's side: its bonds, scan mode, pairing mode. */

/* The stack's bonds, at most BOND_TABLE_MAX of them: past that the table cannot
 * follow anyway. -1 when the stack will not say. */
static int stack_bonds(esp_bd_addr_t *list)
{
    int n = BOND_TABLE_MAX;
    if (esp_bt_gap_get_bond_device_list(&n, list) != ESP_OK) {
        return -1;
    }
    return n;
}

static void sync_bonds(void)
{
    esp_bd_addr_t list[BOND_TABLE_MAX];
    int n = stack_bonds(list);
    if (n >= 0 && bond_sync(&s_bonds, (const uint8_t (*)[6])list, n)) {
        s_bonds_dirty = true;
    }
}

/* Scan mode from the radio as it is now; pairing from the saved setting, which a
 * 1.7 host's CMD_RADIO never touched and still does not. */
static void apply_radio(void)
{
    esp_err_t err = esp_bt_gap_set_scan_mode(
        s_live_radio ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
        s_live_radio && s_live_disc ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan mode refused: %s", esp_err_to_name(err));
    }
    BTM_SetPairableMode(s_settings.discoverable != 0, false);
    if (s_now.state <= SATLINK_BT_DISCOVERABLE) {
        s_now.state = idle_state();
    }
}

/* DisplayYesNo when the host can show a question, NoInputNoOutput when nobody
 * can. The stack reads it at the start of each pairing, so a change takes effect
 * from the next one. */
static bool apply_io_cap(void)
{
    esp_bt_io_cap_t cap = host_confirms() ? ESP_BT_IO_CAP_IO : ESP_BT_IO_CAP_NONE;
    if (cap == s_io_cap) {
        return true;
    }
    esp_err_t err = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &cap, sizeof(cap));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pairing mode refused: %s", esp_err_to_name(err));
        return false;
    }
    s_io_cap = cap;
    ESP_LOGI(TAG, "pairing: %s", cap == ESP_BT_IO_CAP_IO ? "DisplayYesNo, the host confirms"
                                                         : "NoInputNoOutput, nobody is asked");
    return true;
}

/* ---- What the host is told. */

static void send_bt_config(void)
{
    satlink_bt_config_t m = {
        .radio = s_settings.radio,
        .discoverable = s_settings.discoverable,
        .auto_reconnect = s_settings.auto_reconnect,
        .codecs_supported = BT_AV_BUILD_CODECS,
        .codecs_enabled = offered_codecs(),
        .name = satlink_str(s_settings.name),
    };
    if (link_host_minor() >= 8) {
        ESP_LOGI(TAG, "bt config to the host: radio %u, discoverable %u, reconnect %u, "
                      "codecs 0x%02x of 0x%02x", m.radio, m.discoverable, m.auto_reconnect,
                 m.codecs_enabled, m.codecs_supported);
    }
    link_send_bt_config(&m);
}

static void send_bond_list(void)
{
    if (link_host_minor() < 8) {
        return;
    }
    static satlink_bond_list_t list;        /* this task only; 300 bytes off its stack */
    uint8_t order[BOND_TABLE_MAX];
    int n = bond_order(&s_bonds, order);
    int last = bond_last(&s_bonds);
    int total = esp_bt_gap_get_bond_device_num();
    if (total < n) {
        total = n;
    }
    list.total = (uint8_t)(total > 255 ? 255 : total);
    list.count = (uint8_t)(n > SATLINK_BOND_LIST_MAX ? SATLINK_BOND_LIST_MAX : n);
    const bool attached = s_now.state >= SATLINK_BT_CONNECTED;
    for (int k = 0; k < list.count; k++) {
        const bond_entry_t *e = &s_bonds.e[order[k]];
        satlink_bond_t *b = &list.bonds[k];
        memcpy(b->addr, e->addr, sizeof(b->addr));
        b->flags = (uint8_t)(((attached && memcmp(e->addr, s_now.addr, 6) == 0) ? SATLINK_BOND_CONNECTED : 0) |
                             (order[k] == last ? SATLINK_BOND_LAST : 0));
        b->name = satlink_str(e->name);
    }
    ESP_LOGI(TAG, "bond list to the host: %u of %u", list.count, list.total);
    link_send_bond_list(&list);
}

static void send_pair_request(void)
{
    satlink_pair_request_t m = { .kind = s_pair.kind, .passkey = s_pair.passkey,
                                 .name = satlink_str(s_pair.name) };
    memcpy(m.addr, s_pair.addr, sizeof(m.addr));
    link_send_pair_request(&m);
}

static const char *pair_status_name(uint8_t status)
{
    switch (status) {
    case SATLINK_PAIR_OK:       return "paired";
    case SATLINK_PAIR_REJECTED: return "refused by the host";
    case SATLINK_PAIR_TIMEOUT:  return "no answer from the host, refused";
    default:                    return "failed";
    }
}

/* What the pairing machine asked for, in its order: the stack first, then the
 * host. */
static void carry_out(const pair_out_t *out)
{
    if (out->reply) {
        esp_bt_gap_ssp_confirm_reply((uint8_t *)out->addr, out->accept);
    }
    if (out->ask) {
        ESP_LOGI(TAG, "pairing " BDA_FMT " \"%s\": asking the host (kind %u)",
                 BDA_ARGS(s_pair.addr), s_pair.name, s_pair.kind);
        send_pair_request();
    }
    if (out->result) {
        ESP_LOGI(TAG, "pairing " BDA_FMT ": %s", BDA_ARGS(out->addr), pair_status_name(out->status));
        link_send_pair_result(out->addr, out->status);
    }
}

/* ---- Phones coming and going, from handle_a2d_event on this task. */

static void cfg_phone_connected(const uint8_t addr[6])
{
    s_now.name[0] = '\0';                   /* whatever was here named another phone */
    rc_stop(&s_reconnect);                  /* a phone is here: nobody else is called */
    sync_bonds();
    int i = bond_index(&s_bonds, addr);
    if (i >= 0) {
        bond_touch(&s_bonds, addr);
        s_bonds_dirty = true;
        if (s_bonds.e[i].name[0] != '\0') {
            memcpy(s_now.name, s_bonds.e[i].name, sizeof(s_bonds.e[i].name));
        } else {
            /* A bond from before 1.8, or one whose name never came: ask the phone. */
            esp_bt_gap_read_remote_name((uint8_t *)addr);
        }
    }
    if (!s_live_radio) {
        /* Radio switched off while this phone was on its way in, by BT_CONFIG_SET
         * or by CMD_RADIO. The live state, not the saved one: CMD_RADIO may have
         * switched the radio on without saving it. */
        ESP_LOGI(TAG, "radio is off: letting " BDA_FMT " go", BDA_ARGS(addr));
        esp_a2d_sink_disconnect((uint8_t *)addr);
    }
    send_bond_list();
}

static void cfg_phone_gone(void)
{
    send_bond_list();
}

/* ---- Events, one at a time, on this task. */

static void cfg_on_set(const cfg_msg_t *m, uint32_t now)
{
    const bool was_on = s_live_radio;
    unsigned changed = bt_settings_apply(&s_settings, BT_AV_BUILD_CODECS, m->set[0], m->set[1],
                                         m->set[2], m->set[3], m->name, m->name_len);
    if (m->name_refused || (m->name_len != 0 && !bt_name_ok(m->name, m->name_len))) {
        ESP_LOGW(TAG, "bt settings: name refused, keeping \"%s\"", s_settings.name);
    }
    if (changed != 0) {
        save_settings();
    }
    if (changed & BT_CHANGED_NAME) {
        esp_err_t err = esp_bt_gap_set_device_name(s_settings.name);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "device name refused by the stack: %s", esp_err_to_name(err));
        }
    }

    /* The whole state, so the radio follows it even when only a 1.7 CMD_RADIO
     * had moved it. */
    s_live_radio = s_settings.radio != 0;
    s_live_disc = s_settings.discoverable != 0;
    apply_radio();
    if (!s_live_radio) {
        rc_stop(&s_reconnect);
        if (s_now.state >= SATLINK_BT_CONNECTED) {
            esp_a2d_sink_disconnect(s_now.addr);
        }
    } else if (!was_on && s_settings.auto_reconnect) {
        rc_arm(&s_reconnect, now);
    }
    if ((changed & BT_CHANGED_RECONNECT) && !s_settings.auto_reconnect) {
        rc_stop(&s_reconnect);
    }

    ESP_LOGI(TAG, "bt settings from the host: radio %u, discoverable %u, reconnect %u, "
                  "codecs 0x%02x%s, name \"%s\"",
             s_settings.radio, s_settings.discoverable, s_settings.auto_reconnect, offered_codecs(),
             (changed & BT_CHANGED_CODECS) ? " from the next connection" : "", s_settings.name);
    send_bt_config();
    if (changed & (BT_CHANGED_RADIO | BT_CHANGED_DISCOVERABLE)) {
        bt_av_report_state();
    }
}

static void cfg_on_remove(const cfg_msg_t *m)
{
    static const uint8_t all[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    const bool every = memcmp(m->addr, all, sizeof(all)) == 0;
    esp_bd_addr_t list[BOND_TABLE_MAX];
    int n = stack_bonds(list);
    int issued = 0;
    for (int i = 0; i < n; i++) {
        if (every || memcmp(m->addr, list[i], 6) == 0) {
            /* A connected phone is let go first by the stack itself. */
            if (esp_bt_gap_remove_bond_device(list[i]) == ESP_OK) {
                issued++;
            }
        }
    }
    ESP_LOGI(TAG, "bond removal for %s" BDA_FMT ": %d issued",
             every ? "all, " : "", BDA_ARGS(m->addr), issued);
    if (issued == 0) {
        /* Nothing to wait for; the host still gets its answer. */
        sync_bonds();
        send_bond_list();
    }
}

static void cfg_handle(const cfg_msg_t *m, uint32_t now)
{
    pair_out_t out;

    switch (m->type) {
    case CFG_HELLO:
        apply_io_cap();
        if (link_host_minor() >= 8) {
            sync_bonds();
            send_bt_config();
            send_bond_list();
            /* The host may have restarted with a question on screen. */
            if (pair_should_repeat(&s_pair) && host_confirms()) {
                send_pair_request();
            }
        }
        break;

    case CFG_STATE:
        bt_av_report_state();
        break;

    case CFG_REFRESH:
        sync_bonds();
        send_bt_config();
        send_bond_list();
        break;

    case CFG_SET:
        cfg_on_set(m, now);
        break;

    case CFG_REMOVE:
        cfg_on_remove(m);
        break;

    case CFG_REPLY:
        out = pair_on_reply(&s_pair, m->addr, m->ok, now);
        if (!out.reply) {
            ESP_LOGW(TAG, "PAIR_REPLY for " BDA_FMT " answers nothing open, ignored", BDA_ARGS(m->addr));
        }
        carry_out(&out);
        break;

    case CFG_RADIO: {
        const bool on = m->set[0], disc = m->set[1], disconnect = m->set[2];
        const bool was_on = s_live_radio;
        if (disconnect && s_now.state >= SATLINK_BT_CONNECTED) {
            esp_a2d_sink_disconnect(s_now.addr);
        }
        s_live_radio = on;
        s_live_disc = disc;
        apply_radio();
        if (!on) {
            rc_stop(&s_reconnect);
        } else if (!was_on && s_settings.auto_reconnect) {
            rc_arm(&s_reconnect, now);
        }
        ESP_LOGI(TAG, "radio %s at the host's request", !on ? "off" :
                 disc ? "on, discoverable" : "on, connectable only");
        bt_av_report_state();
        break;
    }

    case CFG_CONFIRM:
        out = pair_on_confirm(&s_pair, m->addr, m->value, m->ok, m->name, host_confirms(), now);
        carry_out(&out);
        break;

    case CFG_PASSKEY:
        out = pair_on_passkey(&s_pair, m->addr, m->value, m->name, host_confirms(), now);
        carry_out(&out);
        break;

    case CFG_AUTH:
        if (m->ok) {
            sync_bonds();
            if (bond_index(&s_bonds, m->addr) >= 0 && m->name_len != 0 &&
                bond_set_name(&s_bonds, m->addr, m->name, m->name_len)) {
                s_bonds_dirty = true;
            }
        }
        out = pair_on_done(&s_pair, m->addr, m->ok);
        carry_out(&out);
        if (m->ok) {
            send_bond_list();
            if (m->name_len != 0 && bond_names_attached(s_now.state, s_now.addr, m->addr)) {
                memcpy(s_now.name, m->name, (size_t)m->name_len + 1);
            }
            bt_av_report_state();
        }
        break;

    case CFG_NAME:
        if (!m->ok || m->name_len == 0) {
            break;
        }
        if (bond_index(&s_bonds, m->addr) >= 0 &&
            bond_set_name(&s_bonds, m->addr, m->name, m->name_len)) {
            s_bonds_dirty = true;
            send_bond_list();
        }
        if (bond_names_attached(s_now.state, s_now.addr, m->addr) && s_now.name[0] == '\0') {
            memcpy(s_now.name, m->name, (size_t)m->name_len + 1);
            bt_av_report_state();
        }
        break;

    case CFG_UNBONDED:
        ESP_LOGI(TAG, "bond " BDA_FMT " %s", BDA_ARGS(m->addr), m->ok ? "removed" : "not removed");
        sync_bonds();
        send_bond_list();
        break;

    default:
        break;
    }
}

/* Calls the last phone back while `auto_reconnect` holds, and only into a quiet
 * moment: radio on, nobody attached or pairing, endpoints registered and not about
 * to be registered again. */
static void poll_reconnect(uint32_t now)
{
    int last = bond_last(&s_bonds);
    bool can_call = s_live_radio && s_settings.auto_reconnect && bt_av_idle() &&
                    s_endpoints.state == EP_STATE_IDLE &&
                    !ep_stale(&s_endpoints, link_host_caps(), offered_codecs()) &&
                    s_pair.phase == PAIR_IDLE && last >= 0;
    if (!rc_due(&s_reconnect, now, can_call)) {
        return;
    }
    esp_err_t err = esp_a2d_sink_connect(s_bonds.e[last].addr);
    ESP_LOGI(TAG, "auto-reconnect: calling " BDA_FMT " \"%s\", %u more after this: %s",
             BDA_ARGS(s_bonds.e[last].addr), s_bonds.e[last].name, s_reconnect.left,
             esp_err_to_name(err));
}

static void cfg_poll(void)
{
    static bool started;
    const uint32_t now = now_ms();

    if (!started) {
        started = true;
        /* A HELLO that came before the queue existed went nowhere; answer it as
         * if it had just arrived. Twice is harmless, never is not. */
        const cfg_msg_t hello = { .type = CFG_HELLO };
        cfg_handle(&hello, now);
        sync_bonds();
        if (s_settings.radio && s_settings.auto_reconnect) {
            rc_arm(&s_reconnect, now);
        }
    }

    cfg_msg_t m;
    while (xQueueReceive(s_cfg_queue, &m, 0) == pdTRUE) {
        cfg_handle(&m, now);
    }

    pair_out_t out = pair_on_tick(&s_pair, now);
    carry_out(&out);
    poll_reconnect(now);

    /* A failed write waits before the next: this loop turns every 10 ms, and
     * each failure is a log line on the host's link. */
    static bool save_backoff;
    static uint32_t save_retry_ms;
    if (s_bonds_dirty && s_now.state != SATLINK_BT_STREAMING &&
        (!save_backoff || (int32_t)(now - save_retry_ms) >= 0)) {
        save_backoff = !save_bonds();
        save_retry_ms = now + 10000;
    }
}

/* ---- From the link's task. Queued, never waited on. */

void bt_av_host_hello(void)
{
    cfg_msg_t m = { .type = CFG_HELLO };
    cfg_post(&m);
}

bool bt_av_request_state(void)
{
    cfg_msg_t m = { .type = CFG_STATE };
    return cfg_post(&m);
}

void bt_av_bt_refresh(void)
{
    cfg_msg_t m = { .type = CFG_REFRESH };
    cfg_post(&m);
}

void bt_av_pair_reply(const uint8_t addr[6], bool accept)
{
    cfg_msg_t m = { .type = CFG_REPLY, .ok = accept };
    memcpy(m.addr, addr, sizeof(m.addr));
    cfg_post(&m);
}

void bt_av_config_set(const satlink_bt_config_set_t *set)
{
    cfg_msg_t m = { .type = CFG_SET,
                    .set = { set->radio, set->discoverable, set->auto_reconnect, set->codecs_enabled } };
    if (set->name.len > SATLINK_BT_NAME_MAX) {
        m.name_refused = true;          /* refused whole, never clipped: the contract */
    } else {
        memcpy(m.name, set->name.ptr, set->name.len);
        m.name[set->name.len] = '\0';
        m.name_len = (uint8_t)set->name.len;
    }
    cfg_post(&m);
}

void bt_av_bond_remove(const uint8_t addr[6])
{
    cfg_msg_t m = { .type = CFG_REMOVE };
    memcpy(m.addr, addr, sizeof(m.addr));
    cfg_post(&m);
}

/* ------------------------------------------------------------------- GAP */

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    cfg_msg_t m = {0};

    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            /* The name reaches BT_STATE from CFG_AUTH, on the bt_av task, clipped. */
            ESP_LOGI(TAG, "paired with %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG, "pairing failed, status %d", param->auth_cmpl.stat);
        }
        m.type = CFG_AUTH;
        m.ok = param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS;
        memcpy(m.addr, param->auth_cmpl.bda, sizeof(m.addr));
        cfg_take_name(&m, param->auth_cmpl.device_name);
        cfg_post(&m);
        break;

    case ESP_BT_GAP_CFM_REQ_EVT:
        /* Numeric comparison or Just Works; since patches/0007 the stack confirms
         * neither by itself. Who answers is the pairing machine's call. */
        if (param->cfm_req.just_works) {
            ESP_LOGI(TAG, "confirm request from " BDA_FMT ", Just Works", BDA_ARGS(param->cfm_req.bda));
        } else {
            ESP_LOGI(TAG, "confirm request from " BDA_FMT ", value %06" PRIu32,
                     BDA_ARGS(param->cfm_req.bda), param->cfm_req.num_val);
        }
        m.type = CFG_CONFIRM;
        m.ok = param->cfm_req.just_works;
        m.value = param->cfm_req.num_val;
        memcpy(m.addr, param->cfm_req.bda, sizeof(m.addr));
        cfg_take_name(&m, param->cfm_req.device_name);
        if (!cfg_post(&m)) {
            /* Unanswered, the phone would wait out the stack's 35 s. Nobody can be
             * asked now, so answer as 1.7 would, or refuse if a host should have. */
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, !host_confirms());
        }
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "passkey for " BDA_FMT ": %06" PRIu32,
                 BDA_ARGS(param->key_notif.bda), param->key_notif.passkey);
        m.type = CFG_PASSKEY;
        m.value = param->key_notif.passkey;
        memcpy(m.addr, param->key_notif.bda, sizeof(m.addr));
        cfg_take_name(&m, param->key_notif.device_name);
        cfg_post(&m);
        break;

    case ESP_BT_GAP_KEY_REQ_EVT:
        /* Only a device with a keyboard is asked to type a passkey, and this one
         * never says it has one. */
        ESP_LOGW(TAG, "passkey entry asked of " BDA_FMT ", refused", BDA_ARGS(param->key_req.bda));
        esp_bt_gap_ssp_passkey_reply(param->key_req.bda, false, 0);
        break;

    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        m.type = CFG_NAME;
        m.ok = param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS;
        memcpy(m.addr, param->read_rmt_name.bda, sizeof(m.addr));
        if (m.ok) {
            cfg_take_name(&m, param->read_rmt_name.rmt_name);
        }
        cfg_post(&m);
        break;

    case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT:
        m.type = CFG_UNBONDED;
        m.ok = param->remove_bond_dev_cmpl.status == ESP_BT_STATUS_SUCCESS;
        memcpy(m.addr, param->remove_bond_dev_cmpl.bda, sizeof(m.addr));
        cfg_post(&m);
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

/* Runs PAUSE_GRACE_US after a PAUSE or STOP from the host, unless another transport
 * command, the phone's stream stopping or the phone reporting that it plays came first
 * (pause_policy.h). s_now.state is written on the BT task; a stale read costs at most
 * one suspend of a stream that had just stopped, which the stack answers as a no-op. */
static void pause_check(void *arg)
{
    (void)arg;
    if (!pause_suspends(s_now.state)) {
        return;
    }
    esp_err_t err = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
    if (err == ESP_OK) {
        atomic_store(&s_own_suspend, true);
    }
    ESP_LOGI(TAG, "phone still streaming %u ms after PAUSE: suspending the stream (%s)",
             (unsigned)(PAUSE_GRACE_US / 1000), esp_err_to_name(err));
}

/* Start a stream that pause_check() suspended, on the first sign that music is wanted
 * again: a PLAY from the host, or the phone saying it plays. A phone holds a stream the sink
 * suspended until the sink starts it, and Android takes a START from the remote in exactly
 * that state. */
static void resume_own_suspend(const char *why)
{
    if (!atomic_exchange(&s_own_suspend, false)) {
        return;
    }
    esp_err_t err = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
    if (err != ESP_OK) {
        atomic_store(&s_own_suspend, true);     /* the next sign tries again */
    }
    ESP_LOGI(TAG, "%s: starting the stream this device suspended (%s)", why, esp_err_to_name(err));
}

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
    /* Armed before the AVRCP check on purpose: a stream with no AVRCP to pause it is the
     * plainest case of a pause the phone will not act on. Any other command supersedes
     * a pause still waiting to be checked. A repeated PAUSE does not restart the wait: the
     * host repeats it every 3 s while the phone keeps playing, which is shorter than the
     * wait, so restarting it would mean it never ran out. */
    pause_timer_apply(pause_on_transport(cmd, esp_timer_is_active(s_pause_timer)));
    if (cmd == SATLINK_XPORT_PLAY) {
        resume_own_suspend("PLAY from the host");
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

/* Carried out on the Bluetooth task, which owns the radio state since protocol
 * 1.8 (CFG_RADIO). Not saved: that is what BT_CONFIG_SET is for. */
void bt_av_set_radio(bool on, bool discoverable, bool disconnect)
{
    cfg_msg_t m = { .type = CFG_RADIO, .set = { on, discoverable, disconnect } };
    cfg_post(&m);
}

/* ------------------------------------------------------------------- startup */

esp_err_t bt_av_start(const char *device_name)
{
    do { s_cover_nonce = esp_random(); } while (!s_cover_nonce);
    s_now.state = SATLINK_BT_OFF;
    s_now.codec = SATLINK_CODEC_NONE;
    s_now.play_state = SATLINK_PLAY_STOPPED;

    /* Before the task: from here on the settings are its alone. `device_name` is
     * the factory one; the host may since have given the device another. */
    load_settings(device_name);

    s_queue = xQueueCreate(BT_QUEUE_DEPTH, sizeof(bt_a2d_msg_t));
    ESP_RETURN_ON_FALSE(s_queue != NULL, ESP_ERR_NO_MEM, TAG, "no memory for the event queue");
    s_cfg_queue = xQueueCreate(BT_CFG_QUEUE_DEPTH, sizeof(cfg_msg_t));
    ESP_RETURN_ON_FALSE(s_cfg_queue != NULL, ESP_ERR_NO_MEM, TAG, "no memory for the settings queue");
    s_meta_send = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_meta_send != NULL, ESP_ERR_NO_MEM, TAG, "no memory for the meta lock");
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

    const esp_timer_create_args_t pause_args = {
        .callback = pause_check, .name = "bt_pause", .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&pause_args, &s_pause_timer), TAG, "pause timer");

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
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_device_name(s_settings.name), TAG, "set_device_name");

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
    const uint8_t codecs = offered_codecs();
    decide_endpoints(host_caps, codecs);
    bt_av_apply_host_caps();

    /* Secure Simple Pairing either way. With a host that confirms, DisplayYesNo
     * and a question on its screen; without one, no keyboard and no display, and
     * a phone pairs without anyone having to answer anything. Each HELLO sets it
     * again (apply_io_cap). */
    ESP_RETURN_ON_FALSE(apply_io_cap(), ESP_FAIL, TAG, "set_security_param");

    ESP_RETURN_ON_ERROR(esp_a2d_register_callback(a2d_cb), TAG, "a2d_register");
    ESP_RETURN_ON_ERROR(esp_a2d_sink_init(), TAG, "a2d_sink_init");
    /* Deprecated in 5.5.5 in favour of the external-codec path, and still the only
     * stock route to decoded PCM: the external path hands over raw frames and has
     * no decoder behind it (docs/BASE.md). */
    ESP_RETURN_ON_ERROR(esp_a2d_sink_register_data_callback(a2d_data_cb), TAG, "a2d_data_cb");

    /* Hand the endpoint machine its starting point on its own task, so that it
     * never races the registration above. */
    bt_a2d_msg_t registered = { .event = BT_EVENT_ENDPOINTS_REGISTERED, .host_caps = host_caps,
                                .codecs = codecs };
    ESP_RETURN_ON_FALSE(xQueueSend(s_queue, &registered, pdMS_TO_TICKS(100)) == pdTRUE,
                        ESP_ERR_NO_MEM, TAG, "event queue full at start");

    /* Scan mode and pairing as the saved settings say: factory state is
     * connectable, discoverable and pairable, as before protocol 1.8. */
    apply_radio();
    bt_av_report_state();

    ESP_LOGI(TAG, "\"%s\" is %s, A2DP sink + AVRCP controller%s", s_settings.name,
             s_now.state == SATLINK_BT_OFF ? "off" :
             s_now.state == SATLINK_BT_IDLE ? "connectable only" : "discoverable",
             BT_AV_CODEC_LIST);

    /* The settings, the bonds and the pairing are the task's from here. */
    atomic_store(&s_stack_up, true);
    return ESP_OK;
}
