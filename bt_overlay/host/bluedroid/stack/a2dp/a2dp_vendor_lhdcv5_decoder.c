/* SPDX-License-Identifier: Apache-2.0
 * Adapted from WillyBilly06/LHDC-V5-Decoder at 676af03d2b7a2ed5c1f9bdca7bf8938cc85f704b.
 * Bounded RTP/reassembly, native PCM widths and explicit decoder lifetime.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "common/bt_target.h"
#include "esp_log.h"
#include "lhdc_dec.h"
#include "lhdc_entropy_dec.h"
#include "lhdc_imdct.h"
#include "stack/a2dp_vendor_lhdcv5.h"
#include "stack/a2dp_vendor_lhdcv5_decoder.h"

#if (defined(LHDCV5_DEC_INCLUDED) && LHDCV5_DEC_INCLUDED == TRUE)
#define TAG "lhdcv5"
#define FRAGMENT_CAPACITY 3072
typedef struct {
    void *workspace;
    lhdc_decoder_t *decoder;
    decoded_data_callback_t callback;
    uint32_t rate;
    uint8_t pcm_bits;
    uint8_t fragments_left;
    uint16_t sequence;
    size_t fragment_size;
    uint8_t fragment[FRAGMENT_CAPACITY];
} decoder_state_t;
static decoder_state_t *state;

static void clear_fragments(void) {
    if (state) { state->fragments_left = 0; state->fragment_size = 0; }
}

bool a2dp_lhdcv5_decoder_reset(void) {
    clear_fragments();
    if (state && state->decoder) lhdc_dec_reset(state->decoder);
    return state && state->decoder;
}

void a2dp_lhdcv5_decoder_cleanup(void) {
    if (state) {
        if (state->workspace) lhdc_dec_deinit(state->workspace);
        free(state->workspace);
        free(state);
        state = NULL;
    }
    lhdc_imdct_free_960();
    lhdc_imdct_free_480();
    lhdc_imdct_free_1920();
    lhdc_imdct_free_cos();
    lhdc_dec_free_window();
    lhdc_dec_free_slot_scratch();
    lhdc_entropy_free();
}

bool a2dp_lhdcv5_decoder_init(decoded_data_callback_t callback) {
    a2dp_lhdcv5_decoder_cleanup();
    if (!callback) return false;
    state = calloc(1, sizeof(*state));
    if (!state) return false;
    state->callback = callback;
    return true;
}

void a2dp_lhdcv5_decoder_configure(const uint8_t *codec_info) {
    if (!state) return;
    clear_fragments();
    if (state->workspace) lhdc_dec_deinit(state->workspace);
    state->decoder = NULL;
    tA2DP_LHDCV5_CIE cie;
    if (A2DP_ParseInfoLhdcV5(&cie, codec_info, false) != A2D_SUCCESS) {
        ESP_LOGE(TAG, "invalid configuration; decoder muted");
        return;
    }
    lhdc_dec_config_t cfg = {
        .sample_rate = cie.sampleRate == A2DP_LHDCV5_SAMPLING_FREQ_96000 ? LHDC_DEC_SR_96000 :
                       cie.sampleRate == A2DP_LHDCV5_SAMPLING_FREQ_48000 ? LHDC_DEC_SR_48000 : LHDC_DEC_SR_44100,
        .bit_depth = cie.bitsPerSample == A2DP_LHDCV5_BITS_PER_SAMPLE_24 ? LHDC_DEC_BITDEPTH_24 : LHDC_DEC_BITDEPTH_16,
        .frame_duration = LHDC_DEC_FRAME_5MS,
        .channels = 2,
        .max_frame_bytes = FRAGMENT_CAPACITY,
        .lossless_enable = 0,
    };
    if (!state->workspace)
        state->workspace = calloc(1, lhdc_dec_get_workspace_size(cfg.sample_rate, cfg.frame_duration));
    if (state->workspace) state->decoder = lhdc_dec_init(state->workspace, &cfg);
    if (!state->decoder) {
        ESP_LOGE(TAG, "decoder allocation failed; muted");
        return;
    }
    state->rate = cfg.sample_rate;
    state->pcm_bits = cfg.bit_depth == 24 ? 32 : 16;
    g_lhdc_trace = 0;
    ESP_LOGI(TAG, "configured %lu Hz, %u-bit source, %u-bit PCM",
             (unsigned long)state->rate, (unsigned)cfg.bit_depth, state->pcm_bits);
}

ssize_t a2dp_lhdcv5_decoder_decode_packet_header(BT_HDR *packet) {
    if (!state || !packet || a2dp_rtp_strip_header(packet) < 0 || packet->len < 1) {
        clear_fragments();
        return -EINVAL;
    }
    const uint8_t header = *((const uint8_t *)(packet + 1) + packet->offset);
    const uint8_t count = header & 0x0f;
    const bool fragmented = (header & 0x80) != 0;
    const bool first = (header & 0x40) != 0;
    const bool last = (header & 0x20) != 0;
    if (fragmented) {
        if (!count || (last != (count == 1))) goto invalid;
        if (first) {
            clear_fragments();
        } else if (!state->fragments_left || count + 1 != state->fragments_left ||
                   packet->layer_specific != (uint16_t)(state->sequence + 1)) {
            goto invalid;
        }
        state->fragments_left = count;
        state->sequence = packet->layer_specific;
    } else {
        /* Upstream observed count==0 in valid low-bitrate multi-frame packets. */
        clear_fragments();
    }
    ++packet->offset;
    --packet->len;
    return 0;
invalid:
    clear_fragments();
    return -EINVAL;
}

bool a2dp_lhdcv5_decoder_decode_packet(BT_HDR *packet, unsigned char *pcm, size_t capacity) {
    if (!state || !state->decoder || !packet || !pcm) return false;
    const uint8_t *src = (const uint8_t *)(packet + 1) + packet->offset;
    size_t bytes = packet->len;
    if (state->fragments_left) {
        if (bytes > sizeof(state->fragment) - state->fragment_size) {
            clear_fragments();
            return false;
        }
        memcpy(state->fragment + state->fragment_size, src, bytes);
        state->fragment_size += bytes;
        if (state->fragments_left > 1) return true;
        src = state->fragment;
        bytes = state->fragment_size;
        clear_fragments();
    }
    /* Per upstream over-the-air captures: one LHDC marker precedes raw frames. */
    size_t offset = 1;
    unsigned frames = 0;
    const size_t stride = 2 * (state->pcm_bits / 8);
    const uint32_t max_samples = capacity / stride;
    while (offset + 2 <= bytes) {
        size_t consumed = 0;
        uint32_t samples = 0;
        lhdc_dec_frame_info_t info = {0};
        lhdc_dec_ret_t ret = lhdc_dec_decode_frame(state->decoder, src + offset, bytes - offset,
                                                 pcm, max_samples, &consumed, &samples, &info);
        if (ret != LHDC_DEC_OK || !consumed || consumed > bytes - offset ||
            samples > max_samples || info.channels != 2 || info.bit_depth != state->pcm_bits ||
            info.sample_rate != state->rate) {
            /* Never publish stale/partial scratch or a different format. */
            a2dp_lhdcv5_decoder_reset();
            return false;
        }
        state->callback(pcm, samples * stride);
        offset += consumed;
        ++frames;
    }
    return frames != 0 && offset == bytes;
}
#endif
