/*
 * Wire constants from docs/PROTOCOL.md, version 1.0.
 *
 * There is a second implementation on the other end of the cable. Nothing here
 * changes without changing that document and both sides together.
 */

#pragma once
#include "format_sync.h"

#include <stdint.h>

#define SATLINK_PROTO_MAJOR   1
/* 1.3: optional correlated STREAM_PREPARE / STREAM_READY transitions.
 * Legacy STREAM_FMT layouts remain unchanged. */
#define SATLINK_PROTO_MINOR   5

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
    SATLINK_MSG_CMD_TRANSPORT  = 0x20,  /* H -> D */
    SATLINK_MSG_CMD_VOLUME     = 0x21,  /* H -> D */
    SATLINK_MSG_CMD_RADIO      = 0x22,  /* H -> D */
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

/* HELLO capability bits, host side. */
#define SATLINK_HOST_CAP_RELOCK   (1u << 0)  /* can follow STREAM_FMT mid-flight */
#define SATLINK_HOST_CAP_96KHZ    (1u << 5)  /* protocol 1.4; requires RELOCK + FORMAT_READY */
#define SATLINK_HOST_CAP_COVER    (1u << 1)
#define SATLINK_HOST_CAP_EXPANDER (1u << 2)
#define SATLINK_HOST_CAP_WIDE     (1u << 3)  /* can take 32-bit slots; STREAM_FMT.bits says which */

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
