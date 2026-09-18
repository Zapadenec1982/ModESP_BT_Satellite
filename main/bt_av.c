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

static const char *TAG = "sat.bt";

#define BT_TASK_STACK       3584
#define BT_TASK_PRIO        (configMAX_PRIORITIES - 4)
#define BT_QUEUE_DEPTH      12

/* Metadata we ask the phone for. PLAYING_TIME is not in the usual example masks
 * but TRACK_POS needs the track length. */
#define BT_META_ATTRS   (ESP_AVRC_MD_ATTR_TITLE   | \
                         ESP_AVRC_MD_ATTR_ARTIST  | \
                         ESP_AVRC_MD_ATTR_ALBUM   | \
                         ESP_AVRC_MD_ATTR_PLAYING_TIME)

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

    uint8_t  volume;
    bool     avrcp_up;
    bool     has_play_pos;          /* the phone will notify about position */
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

void bt_av_report_state(void)
{
    link_send_bt_state(s_now.state, s_now.codec, s_now.addr, s_now.name);
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
        s_now.state = started ? SATLINK_BT_STREAMING : SATLINK_BT_CONNECTED;
        bt_av_report_state();
        break;
    }

    case ESP_A2D_AUDIO_CFG_EVT: {
        const esp_a2d_mcc_t *mcc = &p->audio_cfg.mcc;
        if (mcc->type != ESP_A2D_MCT_SBC) {
            /* Cannot happen on stock Bluedroid, which registers an SBC endpoint
             * and nothing else. Worth a shout if it ever does. */
            ESP_LOGE(TAG, "unexpected codec type %d", mcc->type);
            break;
        }
        uint32_t rate = sbc_rate_hz(&mcc->cie.sbc_info);
        ESP_LOGI(TAG, "codec SBC, %" PRIu32 " Hz, bitpool %u..%u",
                 rate, mcc->cie.sbc_info.min_bitpool, mcc->cie.sbc_info.max_bitpool);

        s_now.codec = SATLINK_CODEC_SBC;
        if (rate != 0) {
            audio_out_set_rate(rate);
            /* Before the first sample, which is the one hard promise this message
             * makes to the host. */
            link_send_stream_fmt(rate, 16, 2);
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
                                                       ESP_AVRC_RN_PLAY_POS_CHANGED, 1);
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
                                                       ESP_AVRC_RN_PLAY_POS_CHANGED, 1);
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
    if (!s_now.avrcp_up) {
        ESP_LOGW(TAG, "volume %u with no phone connected", volume);
        return;
    }
    esp_avrc_ct_send_set_absolute_volume_cmd(TL_VOLUME, volume);
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

    ESP_LOGI(TAG, "\"%s\" is discoverable, A2DP sink + AVRCP controller, SBC", device_name);
    return ESP_OK;
}
