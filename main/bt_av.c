#include "bt_av.h"

#include <inttypes.h>
#include <string.h>

#include "audio_out.h"

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "sat.bt";

#define BT_TASK_STACK       3072
#define BT_TASK_PRIO        (configMAX_PRIORITIES - 4)
#define BT_QUEUE_DEPTH      10

/* Metadata we ask the phone for. PLAYING_TIME is not in the usual example masks
 * but the spec's TRACK_POS needs the track length. */
#define BT_META_ATTRS   (ESP_AVRC_MD_ATTR_TITLE   | \
                         ESP_AVRC_MD_ATTR_ARTIST  | \
                         ESP_AVRC_MD_ATTR_ALBUM   | \
                         ESP_AVRC_MD_ATTR_PLAYING_TIME)

/* A2DP events arrive on the Bluetooth stack's own task. Anything that can block —
 * retuning the I2S clock, for one — is handed to this task instead. */
typedef struct {
    uint16_t             event;
    esp_a2d_cb_param_t   param;
} bt_a2d_msg_t;

static QueueHandle_t s_queue;

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

static void handle_a2d_event(const bt_a2d_msg_t *msg)
{
    const esp_a2d_cb_param_t *p = &msg->param;

    switch (msg->event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        const uint8_t *bda = p->conn_stat.remote_bda;
        ESP_LOGI(TAG, "a2dp %s, %02x:%02x:%02x:%02x:%02x:%02x",
                 conn_state_name(p->conn_stat.state),
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "stream %s (dropped %" PRIu32 " bytes so far)",
                 p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED ? "started" : "suspended",
                 audio_out_dropped_bytes());
        break;

    case ESP_A2D_AUDIO_CFG_EVT: {
        const esp_a2d_mcc_t *mcc = &p->audio_cfg.mcc;
        if (mcc->type != ESP_A2D_MCT_SBC) {
            /* Cannot happen on stock Bluedroid, which registers an SBC endpoint and
             * nothing else. Worth a shout if it ever does. */
            ESP_LOGE(TAG, "unexpected codec type %d", mcc->type);
            break;
        }
        uint32_t rate = sbc_rate_hz(&mcc->cie.sbc_info);
        ESP_LOGI(TAG, "codec SBC, %" PRIu32 " Hz, bitpool %u..%u",
                 rate, mcc->cie.sbc_info.min_bitpool, mcc->cie.sbc_info.max_bitpool);
        if (rate != 0) {
            /* Before the first sample, which is what STREAM_FMT will promise the
             * host once the link exists. */
            audio_out_set_rate(rate);
        }
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

/* AVRCP responses are only logged for now, which is cheap enough to do in place.
 * It also sidesteps the trap in meta_rsp: attr_text points at memory the stack
 * reclaims when this returns, so the struct must not be queued anywhere. */
static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "avrcp %s", param->conn_stat.connected ? "connected" : "disconnected");
        if (param->conn_stat.connected) {
            esp_avrc_ct_send_metadata_cmd(0, BT_META_ATTRS);
            /* Ask what the phone is willing to notify us about before subscribing
             * to anything: registering for an event the target does not support
             * gets rejected, and some phones stop answering after that. */
            esp_avrc_ct_send_get_rn_capabilities_cmd(1);
        }
        break;

    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
        esp_avrc_rn_evt_cap_mask_t caps = param->get_rn_caps_rsp.evt_set;
        bool track_change = esp_avrc_rn_evt_bit_mask_operation(
            ESP_AVRC_BIT_MASK_OP_TEST, &caps, ESP_AVRC_RN_TRACK_CHANGE);
        ESP_LOGI(TAG, "peer notifies 0x%04x, track change %s",
                 caps.bits, track_change ? "yes" : "no");
        if (track_change) {
            esp_avrc_ct_send_register_notification_cmd(2, ESP_AVRC_RN_TRACK_CHANGE, 0);
        }
        break;
    }

    case ESP_AVRC_CT_METADATA_RSP_EVT:
        ESP_LOGI(TAG, "meta 0x%02x: %.*s", param->meta_rsp.attr_id,
                 param->meta_rsp.attr_length, (const char *)param->meta_rsp.attr_text);
        break;

    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        ESP_LOGI(TAG, "avrcp notify 0x%02x", param->change_ntf.event_id);
        if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
            esp_avrc_ct_send_metadata_cmd(0, BT_META_ATTRS);
            /* AVRCP notifications fire once. Re-arm or this is the last one. */
            esp_avrc_ct_send_register_notification_cmd(2, ESP_AVRC_RN_TRACK_CHANGE, 0);
        }
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
        } else {
            ESP_LOGE(TAG, "pairing failed, status %d", param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_CFM_REQ_EVT:
        /* Just works / numeric comparison. There are no buttons on this device yet,
         * so the only sane answer is yes. */
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

esp_err_t bt_av_start(const char *device_name)
{
    s_queue = xQueueCreate(BT_QUEUE_DEPTH, sizeof(bt_a2d_msg_t));
    ESP_RETURN_ON_FALSE(s_queue != NULL, ESP_ERR_NO_MEM, TAG, "no memory for the event queue");
    ESP_RETURN_ON_FALSE(
        xTaskCreate(bt_av_task, "bt_av", BT_TASK_STACK, NULL, BT_TASK_PRIO, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "no memory for the bt task");

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

    ESP_LOGI(TAG, "\"%s\" is discoverable, A2DP sink + AVRCP controller, SBC", device_name);
    return ESP_OK;
}
