/*
 * Every message in docs/PROTOCOL.md 1.0, as a struct plus an encoder and a decoder.
 *
 * Encoders write a payload and return its length, or 0 if it would not fit — the
 * frame layer adds type, seq and CRC around it. Decoders return false on anything
 * they do not fully understand, including trailing bytes: a payload longer than
 * the layout means the two ends disagree, and guessing is how that turns into a
 * silent corruption instead of a log line.
 *
 * Strings are borrowed, never copied. On decode they point into the caller's
 * payload buffer and live exactly as long as it does.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "satlink/proto.h"

typedef struct {
    const char *ptr;
    uint16_t    len;
} satlink_str_t;

/* Wraps a C string. NULL becomes the empty string, which the protocol reads as
 * "unknown" rather than as an error. */
satlink_str_t satlink_str(const char *s);

/* ------------------------------------------------------------------ 0x01 HELLO */

typedef struct {
    uint8_t       major;
    uint8_t       minor;
    uint32_t      caps;
    satlink_str_t build;
} satlink_hello_t;

size_t satlink_enc_hello(uint8_t *buf, size_t cap, const satlink_hello_t *m);
bool   satlink_dec_hello(const uint8_t *buf, size_t len, satlink_hello_t *m);

/* ---------------------------------------------------------- 0x02 PING, 0x03 PONG */

typedef struct {
    uint32_t tick_ms;
} satlink_ping_t;

size_t satlink_enc_ping(uint8_t *buf, size_t cap, const satlink_ping_t *m);
bool   satlink_dec_ping(const uint8_t *buf, size_t len, satlink_ping_t *m);

typedef struct {
    uint32_t echo_ms;
    uint32_t own_ms;
} satlink_pong_t;

size_t satlink_enc_pong(uint8_t *buf, size_t cap, const satlink_pong_t *m);
bool   satlink_dec_pong(const uint8_t *buf, size_t len, satlink_pong_t *m);

/* --------------------------------------------------------------- 0x10 BT_STATE */

typedef struct {
    uint8_t       state;        /* satlink_bt_state_t */
    uint8_t       codec;        /* satlink_codec_t */
    uint8_t       addr[6];      /* BD_ADDR, zeros when not connected */
    satlink_str_t name;
} satlink_bt_state_t_msg;

size_t satlink_enc_bt_state(uint8_t *buf, size_t cap, const satlink_bt_state_t_msg *m);
bool   satlink_dec_bt_state(const uint8_t *buf, size_t len, satlink_bt_state_t_msg *m);

/* ------------------------------------------------------------- 0x11 TRACK_META */

typedef struct {
    satlink_str_t title;
    satlink_str_t artist;
    satlink_str_t album;
} satlink_track_meta_t;

size_t satlink_enc_track_meta(uint8_t *buf, size_t cap, const satlink_track_meta_t *m);
bool   satlink_dec_track_meta(const uint8_t *buf, size_t len, satlink_track_meta_t *m);

/* -------------------------------------------------------------- 0x12 TRACK_POS */

typedef struct {
    uint8_t  state;             /* satlink_play_state_t */
    uint32_t elapsed_ms;
    uint32_t total_ms;          /* 0 = unknown */
} satlink_track_pos_t;

size_t satlink_enc_track_pos(uint8_t *buf, size_t cap, const satlink_track_pos_t *m);
bool   satlink_dec_track_pos(const uint8_t *buf, size_t len, satlink_track_pos_t *m);

/* Phones lie about track length: one on the bench returned -1000. Anything
 * negative or longer than a day becomes 0, meaning unknown. */
uint32_t satlink_sanitize_total_ms(int64_t reported_ms);

/* ------------------------------------------------------------- 0x13 STREAM_FMT */

typedef struct {
    uint32_t rate_hz;
    uint8_t  bits;              /* slot width on the wire: 16 or 32 */
    uint8_t  channels;
} satlink_stream_fmt_t;

size_t satlink_enc_stream_fmt(uint8_t *buf, size_t cap, const satlink_stream_fmt_t *m);
bool   satlink_dec_stream_fmt(const uint8_t *buf, size_t len, satlink_stream_fmt_t *m);

/* ------------------------------------------------------------------ 0x14 COVER */

typedef struct {
    uint16_t       chunk;       /* from zero */
    uint16_t       total;       /* how many chunks in all */
    uint8_t        mime;        /* satlink_mime_t */
    uint32_t       total_len;   /* whole image, repeated in every chunk on purpose */
    uint16_t       len;
    const uint8_t *data;
} satlink_cover_t;

size_t satlink_enc_cover(uint8_t *buf, size_t cap, const satlink_cover_t *m);
bool   satlink_dec_cover(const uint8_t *buf, size_t len, satlink_cover_t *m);

/* ---------------------------------------------------------- 0x15 VOLUME_STATE */

typedef struct {
    uint8_t volume;             /* 0..127 */
    uint8_t source;             /* 0 local, 1 changed by the peer */
} satlink_volume_state_t;

size_t satlink_enc_volume_state(uint8_t *buf, size_t cap, const satlink_volume_state_t *m);
bool   satlink_dec_volume_state(const uint8_t *buf, size_t len, satlink_volume_state_t *m);

/* -------------------------------------------------------- 0x20..0x22 host commands */

typedef struct {
    uint8_t cmd;                /* satlink_transport_t */
} satlink_cmd_transport_t;

size_t satlink_enc_cmd_transport(uint8_t *buf, size_t cap, const satlink_cmd_transport_t *m);
bool   satlink_dec_cmd_transport(const uint8_t *buf, size_t len, satlink_cmd_transport_t *m);

typedef struct {
    uint8_t volume;             /* 0..127 */
} satlink_cmd_volume_t;

size_t satlink_enc_cmd_volume(uint8_t *buf, size_t cap, const satlink_cmd_volume_t *m);
bool   satlink_dec_cmd_volume(const uint8_t *buf, size_t len, satlink_cmd_volume_t *m);

typedef struct {
    uint8_t radio;
    uint8_t discoverable;
    uint8_t disconnect;
} satlink_cmd_radio_t;

size_t satlink_enc_cmd_radio(uint8_t *buf, size_t cap, const satlink_cmd_radio_t *m);
bool   satlink_dec_cmd_radio(const uint8_t *buf, size_t len, satlink_cmd_radio_t *m);

/* ---------------------------------------------------------------- 0x30 GPIO_IN */

typedef struct {
    uint32_t values;
    uint32_t changed;
} satlink_gpio_in_t;

size_t satlink_enc_gpio_in(uint8_t *buf, size_t cap, const satlink_gpio_in_t *m);
bool   satlink_dec_gpio_in(const uint8_t *buf, size_t len, satlink_gpio_in_t *m);

/* --------------------------------------------------------------- 0x31 GPIO_OUT */

typedef struct {
    uint32_t values;
    uint32_t mask;              /* only the bits set here are applied */
} satlink_gpio_out_t;

size_t satlink_enc_gpio_out(uint8_t *buf, size_t cap, const satlink_gpio_out_t *m);
bool   satlink_dec_gpio_out(const uint8_t *buf, size_t len, satlink_gpio_out_t *m);

/* --------------------------------------------------------------- 0x32 GPIO_CFG */

typedef struct {
    uint8_t index;
    uint8_t mode;               /* satlink_pin_mode_t */
    uint8_t initial;            /* outputs: start level. Mode 6: period, hundreds of ms */
} satlink_pin_cfg_t;

typedef struct {
    uint8_t           count;
    satlink_pin_cfg_t pins[SATLINK_GPIO_MAX];
} satlink_gpio_cfg_t;

size_t satlink_enc_gpio_cfg(uint8_t *buf, size_t cap, const satlink_gpio_cfg_t *m);
bool   satlink_dec_gpio_cfg(const uint8_t *buf, size_t len, satlink_gpio_cfg_t *m);

/* --------------------------------------------------------------- 0x33 GPIO_MAP */

typedef struct {
    uint8_t index;
    uint8_t gpio;               /* the chip's own number, for humans reading logs */
    uint8_t flags;              /* SATLINK_PIN_CAN_* */
} satlink_pin_map_t;

typedef struct {
    uint8_t           count;    /* zero is a valid, and the starting, configuration */
    satlink_pin_map_t pins[SATLINK_GPIO_MAX];
} satlink_gpio_map_t;

size_t satlink_enc_gpio_map(uint8_t *buf, size_t cap, const satlink_gpio_map_t *m);
bool   satlink_dec_gpio_map(const uint8_t *buf, size_t len, satlink_gpio_map_t *m);

/* ------------------------------------------------------ 0x34 ADC_VALUE, 0x35 ADC_READ */

typedef struct {
    uint8_t  index;
    uint16_t raw;
    uint16_t mv;
} satlink_adc_value_t;

size_t satlink_enc_adc_value(uint8_t *buf, size_t cap, const satlink_adc_value_t *m);
bool   satlink_dec_adc_value(const uint8_t *buf, size_t len, satlink_adc_value_t *m);

typedef struct {
    uint8_t index;
} satlink_adc_read_t;

size_t satlink_enc_adc_read(uint8_t *buf, size_t cap, const satlink_adc_read_t *m);
bool   satlink_dec_adc_read(const uint8_t *buf, size_t len, satlink_adc_read_t *m);

/* -------------------------------------------------------------------- 0x40 LOG */

typedef struct {
    uint8_t       level;        /* satlink_log_level_t */
    satlink_str_t text;
} satlink_log_t;

size_t satlink_enc_log(uint8_t *buf, size_t cap, const satlink_log_t *m);
bool   satlink_dec_log(const uint8_t *buf, size_t len, satlink_log_t *m);
