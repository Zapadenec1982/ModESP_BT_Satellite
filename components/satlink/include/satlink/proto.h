/*
 * Wire constants from docs/PROTOCOL.md, version 1.0.
 *
 * There is a second implementation on the other end of the cable. Nothing here
 * changes without changing that document and both sides together.
 */

#pragma once
#include "cover_wire.h"
#include "format_sync.h"

#include <stdint.h>

#define SATLINK_PROTO_MAJOR   1
/* 1.3: optional correlated STREAM_PREPARE / STREAM_READY transitions.
 * Legacy STREAM_FMT layouts remain unchanged.
 * 1.7: COVER_RETRY, device capability bit 6.
 * 1.8: Bluetooth settings and host-confirmed pairing, device and host bit 7.
 * 1.9: the host may clock the I2S link; device bit 8 and host bit 8.
 * 1.10: aptX Low Latency, BT_STATE codec 6 and BT_CONFIG codec bit 5. */
#define SATLINK_PROTO_MINOR   10

/* The largest decoded frame: type | seq | payload | crc16. */
#define SATLINK_FRAME_MAX     1040
#define SATLINK_PAYLOAD_MAX   (SATLINK_FRAME_MAX - 1 - 1 - 2)   /* 1036 */

/* What a receiver must actually be able to hold. COBS adds ceil(n/254) bytes in
 * the worst case, and the frame carries delimiters: 1040 + 5 + 2 = 1047, rounded
 * up. This is a different number from SATLINK_FRAME_MAX on purpose. */
#define SATLINK_WIRE_MAX      1048

#define SATLINK_DELIMITER     0x00

/* Message types. D = device (us), H = host. */
typedef enum {
    SATLINK_MSG_HELLO          = 0x01,  /* both */
    SATLINK_MSG_PING           = 0x02,  /* both */
    SATLINK_MSG_PONG           = 0x03,  /* both */
    SATLINK_MSG_BT_STATE       = 0x10,  /* D -> H */
    SATLINK_MSG_TRACK_META     = 0x11,  /* D -> H */
    SATLINK_MSG_TRACK_POS      = 0x12,  /* D -> H */
    SATLINK_MSG_STREAM_FMT     = 0x13,  /* D -> H */
    SATLINK_MSG_COVER          = 0x14,  /* D -> H */
    SATLINK_MSG_VOLUME_STATE   = 0x15,  /* D -> H */
    SATLINK_MSG_PAIR_REQUEST   = 0x1A,  /* D -> H, 1.8 */
    SATLINK_MSG_PAIR_RESULT    = 0x1B,  /* D -> H, 1.8 */
    SATLINK_MSG_BT_CONFIG      = 0x1C,  /* D -> H, 1.8 */
    SATLINK_MSG_BOND_LIST      = 0x1D,  /* D -> H, 1.8 */
    SATLINK_MSG_CMD_TRANSPORT  = 0x20,  /* H -> D */
    SATLINK_MSG_CMD_VOLUME     = 0x21,  /* H -> D */
    SATLINK_MSG_CMD_RADIO      = 0x22,  /* H -> D */
    SATLINK_MSG_COVER_RETRY    = COVER_RETRY,  /* H -> D, 1.7 */
    SATLINK_MSG_PAIR_REPLY     = 0x25,  /* H -> D, 1.8 */
    SATLINK_MSG_BT_CONFIG_SET  = 0x26,  /* H -> D, 1.8 */
    SATLINK_MSG_BOND_REMOVE    = 0x27,  /* H -> D, 1.8 */
    SATLINK_MSG_BT_REFRESH     = 0x28,  /* H -> D, 1.8 */
    SATLINK_MSG_GPIO_IN        = 0x30,  /* D -> H */
    SATLINK_MSG_GPIO_OUT       = 0x31,  /* H -> D */
    SATLINK_MSG_GPIO_CFG       = 0x32,  /* H -> D */
    SATLINK_MSG_GPIO_MAP       = 0x33,  /* D -> H */
    SATLINK_MSG_ADC_VALUE      = 0x34,  /* D -> H */
    SATLINK_MSG_ADC_READ       = 0x35,  /* H -> D */
    SATLINK_MSG_LOG            = 0x40,  /* D -> H */
} satlink_msg_t;

/* HELLO capability bits, device side. */
#define SATLINK_CAP_A2DP_SINK     (1u << 0)
#define SATLINK_CAP_AVRCP_META    (1u << 1)
#define SATLINK_CAP_ABS_VOLUME    (1u << 2)
#define SATLINK_CAP_COVER_ART     (1u << 3)
#define SATLINK_CAP_EXPANDER      (1u << 4)  /* understands the GPIO messages, not "has pins" */
#define SATLINK_CAP_ADC           (1u << 5)
/* 1.7. Says a COVER_RETRY naming the current pair is answered with a fresh
 * generation and a repeated OBEX GET. Claimed only alongside SATLINK_CAP_COVER_ART:
 * the host's own gate requires both, and claiming one without the other would
 * describe something this build does not do. */
#define SATLINK_CAP_COVER_RETRY   COVER_RETRY_CAP
/* 1.8. Sends PAIR_REQUEST, PAIR_RESULT, BT_CONFIG and BOND_LIST, and understands
 * PAIR_REPLY, BT_CONFIG_SET, BOND_REMOVE and BT_REFRESH. */
#define SATLINK_CAP_BT_SETTINGS   (1u << 7)
/* 1.9. Set only while it is true: this device's I2S output is a SLAVE, drives DATA
 * alone and takes BCLK and WS from the host. The host drives those two lines only
 * while the device's latest HELLO carries this bit. */
#define SATLINK_CAP_I2S_SLAVE     (1u << 8)

/* HELLO capability bits, host side. */
#define SATLINK_HOST_CAP_RELOCK   (1u << 0)  /* can follow STREAM_FMT mid-flight */
#define SATLINK_HOST_CAP_96KHZ    (1u << 5)  /* protocol 1.4; requires RELOCK + FORMAT_READY */
#define SATLINK_HOST_CAP_COVER    (1u << 1)
#define SATLINK_HOST_CAP_EXPANDER (1u << 2)
#define SATLINK_HOST_CAP_WIDE     (1u << 3)  /* can take 32-bit slots; STREAM_FMT.bits says which */
/* 1.8. The host shows pairing requests and answers PAIR_REPLY. Without it the
 * device pairs the way 1.7 did: NoInputNoOutput, and nobody is asked. */
#define SATLINK_HOST_CAP_PAIR_CONFIRM (1u << 7)
/* 1.9. The host clocks the link: it drives BCLK and WS at the announced rate in
 * 32-bit slots, from the clock that also drives its DAC. Requires RELOCK and WIDE.
 * A device that sees it stays a slave; one that does not reverts to master. */
#define SATLINK_HOST_CAP_I2S_MASTER   (1u << 8)

/* BT_STATE.state */
typedef enum {
    SATLINK_BT_OFF          = 0,
    SATLINK_BT_IDLE         = 1,
    SATLINK_BT_DISCOVERABLE = 2,
    SATLINK_BT_CONNECTING   = 3,
    SATLINK_BT_CONNECTED    = 4,
    SATLINK_BT_STREAMING    = 5,
} satlink_bt_state_t;

/* BT_STATE.codec */
typedef enum {
    SATLINK_CODEC_NONE    = 0,
    SATLINK_CODEC_SBC     = 1,
    SATLINK_CODEC_AAC     = 2,
    SATLINK_CODEC_APTX    = 3,
    SATLINK_CODEC_APTX_HD = 4,
    SATLINK_CODEC_LHDC_V5 = 5,
    SATLINK_CODEC_APTX_LL = 6,  /* 1.10; a host below it is told 3, aptX */
} satlink_codec_t;

/* TRACK_POS.state */
typedef enum {
    SATLINK_PLAY_STOPPED  = 0,
    SATLINK_PLAY_PLAYING  = 1,
    SATLINK_PLAY_PAUSED   = 2,
    SATLINK_PLAY_FWD_SEEK = 3,
    SATLINK_PLAY_REV_SEEK = 4,
} satlink_play_state_t;

/* CMD_TRANSPORT.cmd */
typedef enum {
    SATLINK_XPORT_PLAY       = 0,
    SATLINK_XPORT_PAUSE      = 1,
    SATLINK_XPORT_STOP       = 2,
    SATLINK_XPORT_NEXT       = 3,
    SATLINK_XPORT_PREV       = 4,
    SATLINK_XPORT_FFWD_START = 5,
    SATLINK_XPORT_FFWD_STOP  = 6,
    SATLINK_XPORT_REW_START  = 7,
    SATLINK_XPORT_REW_STOP   = 8,
} satlink_transport_t;

/* PAIR_REQUEST.kind, 1.8 */
typedef enum {
    SATLINK_PAIR_NUMERIC = 0,   /* compare the numbers, answer PAIR_REPLY */
    SATLINK_PAIR_CONSENT = 1,   /* Just Works: no number, answer PAIR_REPLY */
    SATLINK_PAIR_PASSKEY = 2,   /* show the passkey to type on the phone; no answer */
} satlink_pair_kind_t;

#define SATLINK_PASSKEY_MAX     999999u

/* PAIR_RESULT.status, 1.8. Anything from 4 up is reserved and reads as FAILED. */
typedef enum {
    SATLINK_PAIR_OK       = 0,
    SATLINK_PAIR_REJECTED = 1,  /* the host said no */
    SATLINK_PAIR_TIMEOUT  = 2,  /* the host said nothing for 25 s */
    SATLINK_PAIR_FAILED   = 3,  /* cancelled on the phone, authentication, link lost */
} satlink_pair_status_t;

/* BT_CONFIG codec bits, 1.8. Not the BT_STATE codec numbers: those count, these mask. */
#define SATLINK_CODEC_BIT_SBC       (1u << 0)
#define SATLINK_CODEC_BIT_AAC       (1u << 1)
#define SATLINK_CODEC_BIT_APTX      (1u << 2)
#define SATLINK_CODEC_BIT_APTX_HD   (1u << 3)
#define SATLINK_CODEC_BIT_LHDC_V5   (1u << 4)
#define SATLINK_CODEC_BIT_APTX_LL   (1u << 5)   /* 1.10; never shown to a host below it */

/* BOND_LIST, 1.8 */
#define SATLINK_BOND_CONNECTED      (1u << 0)
#define SATLINK_BOND_LAST           (1u << 1)   /* the auto-reconnect target */
#define SATLINK_BOND_LIST_MAX       16
#define SATLINK_BT_NAME_MAX         32          /* device and phone names, in bytes */

/* LOG.level */
typedef enum {
    SATLINK_LOG_ERROR = 0,
    SATLINK_LOG_WARN  = 1,
    SATLINK_LOG_INFO  = 2,
    SATLINK_LOG_DEBUG = 3,
} satlink_log_level_t;

/* COVER */
#define SATLINK_COVER_CHUNK_MAX   1024
#define SATLINK_COVER_TOTAL_MAX   (256u * 1024u)   /* bigger than this is not sent at all */

typedef enum {
    SATLINK_MIME_UNKNOWN = 0,
    SATLINK_MIME_JPEG    = 1,
    SATLINK_MIME_PNG     = 2,
} satlink_mime_t;

/* GPIO_CFG.mode */
typedef enum {
    SATLINK_PIN_DISABLED    = 0,
    SATLINK_PIN_INPUT       = 1,
    SATLINK_PIN_INPUT_PU    = 2,
    SATLINK_PIN_INPUT_PD    = 3,
    SATLINK_PIN_OUTPUT      = 4,
    SATLINK_PIN_OUTPUT_OD   = 5,
    SATLINK_PIN_ANALOG      = 6,   /* `initial` carries the period in hundreds of ms */
} satlink_pin_mode_t;

/* GPIO_MAP.flags */
#define SATLINK_PIN_CAN_INPUT   (1u << 0)
#define SATLINK_PIN_CAN_OUTPUT  (1u << 1)
#define SATLINK_PIN_CAN_ADC     (1u << 2)
#define SATLINK_PIN_HAS_PULL    (1u << 3)

#define SATLINK_GPIO_MAX        32     /* GPIO_IN and GPIO_OUT carry u32 masks */

#define SATLINK_MSG_STREAM_PREPARE FMT_PREPARE
#define SATLINK_MSG_STREAM_READY FMT_READY
#define SATLINK_HOST_CAP_FORMAT_READY FMT_HOST_CAP
