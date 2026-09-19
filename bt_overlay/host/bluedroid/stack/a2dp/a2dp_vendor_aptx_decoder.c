/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "common/bt_trace.h"
#include "stack/a2dp_vendor_aptx.h"
#include "stack/a2dp_vendor_aptx_hd.h"
#include "stack/a2dp_vendor_aptx_decoder.h"
#include "stack/a2dp_decoder.h"


#if (defined(APTX_DEC_INCLUDED) && APTX_DEC_INCLUDED == TRUE)

/* libfreeaptx API */
extern struct aptx_context *aptx_init(int hd);
extern void aptx_reset(struct aptx_context *ctx);
extern void aptx_finish(struct aptx_context *ctx);
extern int aptx_encode_finish(struct aptx_context *ctx,
                              unsigned char *output,
                              size_t output_size,
                              size_t *written);
extern size_t aptx_decode(struct aptx_context *ctx,
                          const unsigned char *input,
                          size_t input_size,
                          unsigned char *output,
                          size_t output_size,
                          size_t *written);
extern size_t aptx_decode_sync(struct aptx_context *ctx,
                              const unsigned char *input,
                              size_t input_size,
                              unsigned char *output,
                              size_t output_size,
                              size_t *written,
                              int *synced,
                              size_t *dropped);
extern size_t aptx_decode_sync_finish(struct aptx_context *ctx);
extern size_t aptx_decode16(struct aptx_context *ctx,
                            const unsigned char *input,
                            size_t input_size,
                            unsigned char *output,
                            size_t output_size,
                            size_t *written);
extern size_t aptx_decode32(struct aptx_context *ctx,
                            const unsigned char *input,
                            size_t input_size,
                            unsigned char *output,
                            size_t output_size,
                            size_t *written);

/* One aptX frame decodes to 4 samples per channel; at 24 bits and two
 * channels that is 24 bytes of output per frame. */
#define APTX_BYTES_PER_FRAME_24     24

typedef enum
{
    APTX_STANDARD,
    APTX_HD,
} tA2DP_APTX_TYPE;

typedef struct {
  struct aptx_context* decoder_context;
  tA2DP_APTX_TYPE aptx_type;
  decoded_data_callback_t decode_callback;
  uint32_t decode_failures;
} tA2DP_APTX_DECODER_CB;

static tA2DP_APTX_DECODER_CB a2dp_aptx_decoder_cb;


bool a2dp_aptx_decoder_init(decoded_data_callback_t decode_callback) {
    struct aptx_context* decoder_context = aptx_init(0);
    if (!decoder_context) {
        APPL_TRACE_ERROR("%s decoder init failed", __func__);
        return false;
    }
    a2dp_aptx_decoder_cb.decoder_context = decoder_context;
    a2dp_aptx_decoder_cb.decode_callback = decode_callback;
    a2dp_aptx_decoder_cb.aptx_type = APTX_STANDARD;
    return true;
}

void a2dp_aptx_decoder_cleanup(void) {
    struct aptx_context* decoder_context = a2dp_aptx_decoder_cb.decoder_context;
    if (!decoder_context) {
        APPL_TRACE_ERROR("%s decoder context not initialized", __func__);
        return;
    }

    aptx_finish(decoder_context);
    a2dp_aptx_decoder_cb.decoder_context = NULL;
}

bool a2dp_aptx_decoder_reset(void) {
    struct aptx_context* decoder_context = a2dp_aptx_decoder_cb.decoder_context;
    if (!decoder_context) {
        APPL_TRACE_ERROR("%s decoder context not initialized", __func__);
        return false;
    }

    /* sync_finish, not reset: it also drops whatever partial frame was
     * cached, which would otherwise be prepended to the next stream. */
    aptx_decode_sync_finish(decoder_context);
    a2dp_aptx_decoder_cb.decode_failures = 0;
    return true;
}

ssize_t a2dp_aptx_decoder_decode_packet_header(BT_HDR* p_buf) {
    if (a2dp_aptx_decoder_cb.aptx_type != APTX_HD) {
        return 0;
    }
    return a2dp_rtp_strip_header(p_buf);
}

/* aptx_decode_sync writes 24-bit samples, three bytes each, little endian. The
 * I2S bus wants 16. Narrowing in place is safe because the destination index
 * always trails the source: 2i <= 3i, and both bytes of each sample are read
 * before either is written. */
static size_t aptx_pack24_to_pcm16(uint8_t *p, size_t bytes24)
{
    size_t samples = bytes24 / 3;
    for (size_t i = 0; i < samples; i++) {
        uint8_t lo = p[3 * i + 1];
        uint8_t hi = p[3 * i + 2];
        p[2 * i + 0] = lo;
        p[2 * i + 1] = hi;
    }
    return samples * 2;
}

bool a2dp_aptx_decoder_decode_packet(BT_HDR* p_buf, unsigned char* buf, size_t buf_len) {
    struct aptx_context* ctx = a2dp_aptx_decoder_cb.decoder_context;

    if (!ctx || !buf || buf_len < APTX_BYTES_PER_FRAME_24) {
        return false;
    }

    const unsigned char *src = (const unsigned char *)(p_buf + 1) + p_buf->offset;
    size_t src_len = p_buf->len;
    size_t taken = 0;

    /* Hand aptx_decode_sync no more input per call than its output can hold.
     * If the output fills mid-call it stops early, and the sync layer reads a
     * short return as a parity failure and throws the frame away -- silent,
     * self-inflicted packet loss. Capping the input makes that unreachable. */
    const size_t max_in = (buf_len / APTX_BYTES_PER_FRAME_24) *
                          (a2dp_aptx_decoder_cb.aptx_type == APTX_HD ? 6 : 4);

    while (taken < src_len) {
        size_t chunk = src_len - taken;
        if (chunk > max_in) {
            chunk = max_in;
        }

        size_t written24 = 0, dropped = 0;
        int synced = 0;
        size_t used = aptx_decode_sync(ctx, src + taken, chunk,
                                       buf, buf_len, &written24, &synced, &dropped);

        if (written24 > 0) {
            size_t pcm_len = aptx_pack24_to_pcm16((uint8_t *)buf, written24);
            if (pcm_len > 0) {
                a2dp_aptx_decoder_cb.decode_callback((uint8_t *)buf, pcm_len);
            }
        }

        if (dropped > 0) {
            /* Bytes thrown away while hunting for the next valid frame. A burst
             * at stream start is normal -- we joined mid-stream. A steady trickle
             * means packets are being lost somewhere upstream. */
            a2dp_aptx_decoder_cb.decode_failures += dropped;
            if (a2dp_aptx_decoder_cb.decode_failures <= 3 ||
                (a2dp_aptx_decoder_cb.decode_failures % 4096) < dropped) {
                APPL_TRACE_WARNING("%s: resynchronised, %u bytes dropped (%u total)",
                                   __func__, (unsigned)dropped,
                                   (unsigned)a2dp_aptx_decoder_cb.decode_failures);
            }
        }

        if (used == 0) {
            break;      /* no forward progress; the rest is cached internally */
        }
        taken += used;
    }

    p_buf->offset += taken;
    p_buf->len -= taken;
    return true;
}

void a2dp_aptx_decoder_configure(const uint8_t* p_codec_info) {
    struct aptx_context* decoder_context = a2dp_aptx_decoder_cb.decoder_context;
    btav_a2dp_codec_index_t index = A2DP_SinkCodecIndex(p_codec_info);

    if (!decoder_context) {
        LOG_ERROR("%s: decoder not initialized", __func__);
        return;
    }

    tA2DP_APTX_HD_CIE cie;
    tA2D_STATUS status;

    status = A2DP_ParseInfoAptx((tA2DP_APTX_CIE*)&cie, p_codec_info, FALSE);
    if (status != A2D_SUCCESS) {
        status = A2DP_ParseInfoAptxHd((tA2DP_APTX_HD_CIE*)&cie, p_codec_info,
                                       FALSE);
    }
    if (status != A2D_SUCCESS) {
        LOG_ERROR("%s: failed to parse codec info. 0x%x", __func__, status);
        return;
    }

    uint32_t sr = 0;
    if (cie.sampleRate == A2DP_APTX_SAMPLERATE_44100) {
        sr = 44100;
    } else if (cie.sampleRate == A2DP_APTX_SAMPLERATE_48000) {
        sr = 48000;
    }
    LOG_INFO("%s: aptX Sampling frequency = %lu", __func__, sr);

    if (cie.channelMode == A2DP_APTX_CHANNELS_STEREO) {
        LOG_INFO("%s: aptX Channel mode: Stereo", __func__);
    } else if (cie.channelMode == A2DP_APTX_CHANNELS_MONO) {
        LOG_INFO("%s: aptX Channel mode: Mono", __func__);
    }

    a2dp_aptx_decoder_cb.aptx_type =
        (index == BTAV_A2DP_CODEC_INDEX_SINK_APTX_HD) ? APTX_HD : APTX_STANDARD;
    a2dp_aptx_decoder_cb.decode_failures = 0;

    /* At WARNING because everything downstream depends on getting this right:
     * the variant decides the frame size, whether an RTP header is stripped,
     * and which context libfreeaptx is initialised with. Getting it wrong is
     * not subtly wrong, it is silence. */
    APPL_TRACE_WARNING("%s: %s, codec index %d, %lu Hz, %s", __func__,
                       a2dp_aptx_decoder_cb.aptx_type == APTX_HD ? "aptX HD" : "aptX",
                       (int)index, sr,
                       cie.channelMode == A2DP_APTX_CHANNELS_MONO ? "mono" : "stereo");

    aptx_finish(decoder_context);
    a2dp_aptx_decoder_cb.decoder_context = aptx_init(a2dp_aptx_decoder_cb.aptx_type == APTX_HD);
}

#endif /* defined(APTX_DEC_INCLUDED) && APTX_DEC_INCLUDED == TRUE) */
