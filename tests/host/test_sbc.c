/* SBC on the sink: the advertised channel modes and the decoder's reset.
 * DEVICE_SPEC.md: every endpoint is stereo only. The capability must not offer
 * mono or dual channel, and a mono stream that reaches the decoder anyway must
 * come out on both channels rather than fail every frame. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "stack/a2d_sbc.h"
#include "stack/a2d_sbc_decoder.h"

/* From a2d_api.c and a2dp_codec_config.c, which would bring SDP and every codec along. */
uint8_t A2D_BitsSet(uint64_t num) {
    if (num == 0) return A2D_SET_ZERO_BIT;
    return ((num & (num - 1)) == 0) ? A2D_SET_ONE_BIT : A2D_SET_MULTL_BIT;
}
tA2D_CODEC_TYPE A2DP_GetCodecType(const uint8_t *p_codec_info) {
    return (tA2D_CODEC_TYPE)p_codec_info[2];
}

static uint8_t s_out[4096], s_pcm[4096];
static unsigned s_calls;
static uint32_t s_len;

static void on_pcm(uint8_t *buf, uint32_t len) {
    s_calls++;
    s_len = len;
    memcpy(s_pcm, buf, len < sizeof(s_pcm) ? len : sizeof(s_pcm));
}

static void config(uint8_t *info, uint8_t ch_mode) {
    tA2D_SBC_CIE cie = {
        A2D_SBC_IE_SAMP_FREQ_44, ch_mode, A2D_SBC_IE_BLOCKS_16, A2D_SBC_IE_SUBBAND_8,
        A2D_SBC_IE_ALLOC_MD_L, 53, A2D_SBC_IE_MIN_BITPOOL,
    };
    assert(A2D_BldSbcInfo(A2D_MEDIA_TYPE_AUDIO, &cie, info) == A2D_SUCCESS);
}

/* The SBC header CRC-8: x^8 + x^4 + x^3 + x^2 + 1, seeded with 0x0f. */
static uint8_t crc8(const uint8_t *data, unsigned n) {
    uint8_t crc = 0x0f;
    for (unsigned i = 0; i < n; i++)
        for (int bit = 7; bit >= 0; bit--) {
            unsigned in = (data[i] >> bit & 1) ^ (crc >> 7);
            crc = (uint8_t)(crc << 1);
            if (in) crc ^= 0x1d;
        }
    return crc;
}

typedef struct { BT_HDR hdr; uint8_t data[64]; } packet_t;

/* One mono frame: 44.1 kHz, 16 blocks, 8 subbands, loudness, bitpool 10. */
static void mono_packet(packet_t *p) {
    memset(p, 0, sizeof(*p));
    uint8_t *f = p->data + 1;
    p->data[0] = 1;                                   /* media payload header: one frame */
    f[0] = 0x9c;                                      /* syncword */
    f[1] = 2 << 6 | 3 << 4 | 0 << 2 | 0 << 1 | 1;     /* 44.1, 16 blocks, mono, loudness, 8 */
    f[2] = 10;                                        /* bitpool */
    memset(f + 4, 0x88, 4);                           /* eight scale factors */
    for (unsigned i = 8; i < 28; i++) f[i] = (uint8_t)(0x5a + 37 * i);
    uint8_t covered[6] = { f[1], f[2], f[4], f[5], f[6], f[7] };
    f[3] = crc8(covered, sizeof(covered));
    p->hdr.len = 1 + 28;
}

int main(void) {
    uint8_t caps[A2D_SBC_INFO_LEN + 1], cfg[A2D_SBC_INFO_LEN + 1];
    tA2D_SBC_CIE cie;

    /* The sink offers stereo and joint stereo, nothing else. */
    assert(A2DP_InitCodecConfigSbcSink(caps));
    assert(A2D_ParsSbcInfo(&cie, caps, TRUE) == A2D_SUCCESS);
    assert(cie.ch_mode == (A2D_SBC_IE_CH_MD_STEREO | A2D_SBC_IE_CH_MD_JOINT));

    config(cfg, A2D_SBC_IE_CH_MD_JOINT);
    assert(A2DP_IsPeerSourceCodecSupportedSbc(cfg) == A2D_SUCCESS);
    config(cfg, A2D_SBC_IE_CH_MD_STEREO);
    assert(A2DP_IsPeerSourceCodecSupportedSbc(cfg) == A2D_SUCCESS);
    config(cfg, A2D_SBC_IE_CH_MD_MONO);
    assert(A2DP_IsPeerSourceCodecSupportedSbc(cfg) != A2D_SUCCESS);
    config(cfg, A2D_SBC_IE_CH_MD_DUAL);
    assert(A2DP_IsPeerSourceCodecSupportedSbc(cfg) != A2D_SUCCESS);

    /* As initiator, a peer that offers only mono is refused rather than chosen. */
    uint8_t peer[A2D_SBC_INFO_LEN + 1], chosen[A2D_SBC_INFO_LEN + 1];
    config(peer, A2D_SBC_IE_CH_MD_MONO);
    assert(!A2DP_BuildCodecConfigSbc(peer, chosen));
    config(peer, A2D_SBC_IE_CH_MD_MONO | A2D_SBC_IE_CH_MD_DUAL | A2D_SBC_IE_CH_MD_JOINT);
    assert(A2DP_BuildCodecConfigSbc(peer, chosen));
    assert(A2D_ParsSbcInfo(&cie, chosen, FALSE) == A2D_SUCCESS);
    assert(cie.ch_mode == A2D_SBC_IE_CH_MD_JOINT);

    /* A mono configuration still decodes, to two equal channels. A reset with
     * maxChannels 1 and pcmStride 2 failed and left the context zeroed. */
    static packet_t pkt;
    assert(a2dp_sbc_decoder_init(on_pcm));
    config(cfg, A2D_SBC_IE_CH_MD_MONO);
    a2dp_sbc_decoder_configure(cfg);
    mono_packet(&pkt);
    assert(a2dp_sbc_decoder_decode_packet(&pkt.hdr, s_out, sizeof(s_out)));
    assert(s_calls == 1);
    assert(s_len == 16 * 8 * 2 * sizeof(int16_t));
    const int16_t *pcm = (const int16_t *)s_pcm;
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 16 * 8; i++) {
        assert(pcm[2 * i] == pcm[2 * i + 1]);
        nonzero += pcm[2 * i] != 0;
    }
    assert(nonzero > 0);

    puts("test_sbc: ok");
    return 0;
}
