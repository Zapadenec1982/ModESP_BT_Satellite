/*
 * aptX Low Latency, the real codec and decoder files against host BT stubs.
 *
 * What the sink advertises byte by byte, what it accepts and builds, and a stream
 * encoded by libfreeaptx itself decoded through the aptX LL decoder interface. No
 * aptX LL source exists in this repository, so this proves the layout as written
 * here and the path through the decoder, not interop with a real source.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "stack/a2dp_vendor_aptx.h"
#include "stack/a2dp_vendor_aptx_hd.h"
#include "stack/a2dp_vendor_aptx_ll.h"
#include "stack/a2dp_vendor_aptx_decoder.h"
#include "freeaptx.h"

/* The one dispatcher the decoder calls; the real one lives in a2dp_codec_config.c
 * with the whole stack behind it. */
btav_a2dp_codec_index_t A2DP_SinkCodecIndex(const uint8_t *p_codec_info)
{
    switch (p_codec_info[3]) {
    case 0x0A: return BTAV_A2DP_CODEC_INDEX_SINK_APTX_LL;
    case 0xD7: return BTAV_A2DP_CODEC_INDEX_SINK_APTX_HD;
    default:   return BTAV_A2DP_CODEC_INDEX_SINK_APTX;
    }
}

static size_t pcm_bytes;
static double pcm_energy;

static void pcm_callback(uint8_t *buf, uint32_t len)
{
    assert(len % 8 == 0);           /* int32 stereo frames */
    pcm_bytes += len;
    const int32_t *s = (const int32_t *)buf;
    for (uint32_t i = 0; i < len / 4; i++) {
        double v = (double)s[i] / 2147483648.0;
        pcm_energy += v * v;
    }
}

int main(void)
{
    uint8_t caps[16], cfg[16], picked[16];
    tA2DP_APTX_LL_CIE cie;

    /* Withheld unless asked for: no endpoint, and nothing from a phone accepted. */
    memset(caps, 0, sizeof(caps));
    assert(!A2DP_VendorInitCodecConfigAptxLl(BTAV_A2DP_CODEC_INDEX_SINK_APTX_LL, caps));
    static const uint8_t peer[] = { 10, 0x00, 0xFF, 0x0A, 0, 0, 0, 0x02, 0x00, 0x33, 0x00 };
    assert(A2DP_IsVendorPeerSourceCodecValidAptxLl(peer) == A2D_NS_CODEC_TYPE);

    /* Offered at 44.1 only until RELOCK says 48 may come. */
    A2DP_AptxLlSinkOffer(true);
    A2DP_AptxLlSinkAllow48kHz(false);
    assert(!A2DP_VendorInitCodecConfigAptxLl(BTAV_A2DP_CODEC_INDEX_SINK_APTX, caps));
    assert(A2DP_VendorInitCodecConfigAptxLl(BTAV_A2DP_CODEC_INDEX_SINK_APTX_LL, caps));
    static const uint8_t caps_441[] = { 10, 0x00, 0xFF, 0x0A, 0, 0, 0, 0x02, 0x00, 0x22, 0x00 };
    assert(memcmp(caps, caps_441, sizeof(caps_441)) == 0);
    A2DP_AptxLlSinkAllow48kHz(true);
    assert(A2DP_VendorInitCodecConfigAptxLl(BTAV_A2DP_CODEC_INDEX_SINK_APTX_LL, caps));
    assert(caps[9] == 0x32 && caps[10] == 0x00);    /* 44.1 | 48, stereo; no flags */

    /* Its own capability parses, as capability and not as a configuration. */
    assert(A2DP_ParseInfoAptxLl(&cie, caps, true) == A2D_SUCCESS);
    assert(cie.vendorId == 0x0000000A && cie.codecId == 0x0002);
    assert(A2DP_ParseInfoAptxLl(&cie, caps, false) == A2D_BAD_SAMP_FREQ);

    /* Neither aptX nor aptX HD is taken for it, nor it for them. */
    tA2DP_APTX_CIE aptx;
    assert(A2DP_ParseInfoAptx(&aptx, caps, true) == A2D_WRONG_CODEC);
    static const uint8_t aptx_caps[] = { 9, 0x00, 0xFF, 0x4F, 0, 0, 0, 0x01, 0x00, 0x22 };
    assert(A2DP_ParseInfoAptxLl(&cie, aptx_caps, true) == A2D_WRONG_CODEC);
    static const uint8_t extended[] = { 21, 0x00, 0xFF, 0x0A, 0, 0, 0, 0x02, 0x00, 0x22, 0x02 };
    assert(A2DP_ParseInfoAptxLl(&cie, extended, true) == A2D_WRONG_CODEC);

    /* A source offering everything, back channel included, gets 48 kHz stereo and
     * no back channel; at 44.1-only it gets 44.1. */
    assert(A2DP_IsVendorPeerSourceCodecValidAptxLl(peer) == A2D_SUCCESS);
    static const uint8_t src[] = { 10, 0x00, 0xFF, 0x0A, 0, 0, 0, 0x02, 0x00, 0x33, 0x01 };
    assert(A2DP_VendorBuildCodecConfigAptxLl((UINT8 *)src, picked));
    static const uint8_t want_48[] = { 10, 0x00, 0xFF, 0x0A, 0, 0, 0, 0x02, 0x00, 0x12, 0x00 };
    assert(memcmp(picked, want_48, sizeof(want_48)) == 0);
    assert(A2DP_ParseInfoAptxLl(&cie, picked, false) == A2D_SUCCESS);
    assert(A2DP_IsVendorPeerSourceCodecValidAptxLl(picked) == A2D_SUCCESS);
    A2DP_AptxLlSinkAllow48kHz(false);
    assert(A2DP_IsVendorPeerSourceCodecValidAptxLl(picked) == A2D_NS_SAMP_FREQ);
    assert(A2DP_VendorBuildCodecConfigAptxLl((UINT8 *)src, cfg) && cfg[9] == 0x22);

    /* Mono only is refused: libfreeaptx decodes stereo. */
    static const uint8_t mono[] = { 10, 0x00, 0xFF, 0x0A, 0, 0, 0, 0x02, 0x00, 0x21, 0x00 };
    assert(A2DP_IsVendorPeerSourceCodecValidAptxLl(mono) == A2D_NS_CH_MODE);
    assert(!A2DP_VendorBuildCodecConfigAptxLl((UINT8 *)mono, cfg));

    /* Through the decoder interface the stack would hand out for this codec. */
    const tA2DP_DECODER_INTERFACE *dec = A2DP_GetVendorDecoderInterfaceAptxLl(cfg);
    assert(dec != NULL);
    assert(dec->decoder_init(pcm_callback));
    dec->decoder_configure(cfg);

    /* One second of a 1 kHz tone at 44.1 kHz, encoded as standard aptX. */
    enum { RATE = 44100, FRAMES = RATE };
    static uint8_t pcm24[FRAMES * 2 * 3];
    for (int i = 0; i < FRAMES; i++) {
        int32_t v = (int32_t)(sin(2.0 * M_PI * 1000.0 * i / RATE) * 0.5 * 8388607.0);
        for (int ch = 0; ch < 2; ch++) {
            uint8_t *p = &pcm24[(i * 2 + ch) * 3];
            p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16);
        }
    }
    static uint8_t coded[FRAMES];               /* four bytes per four stereo frames */
    struct aptx_context *enc = aptx_init(0);
    size_t coded_len = 0;
    size_t used = aptx_encode(enc, pcm24, sizeof(pcm24), coded, sizeof(coded), &coded_len);
    assert(used == sizeof(pcm24) && coded_len == FRAMES);
    aptx_finish(enc);

    /* Packets as an aptX source sends them: payload only, no RTP header. */
    static struct { BT_HDR hdr; uint8_t data[672]; } pkt;
    static unsigned char out[4096];
    for (size_t off = 0; off < coded_len; off += sizeof(pkt.data)) {
        size_t n = coded_len - off < sizeof(pkt.data) ? coded_len - off : sizeof(pkt.data);
        memset(&pkt.hdr, 0, sizeof(pkt.hdr));
        memcpy(pkt.data, coded + off, n);
        pkt.hdr.len = (uint16_t)n;
        assert(dec->decode_packet_header(&pkt.hdr) == 0);         /* nothing stripped */
        assert(pkt.hdr.len == n && pkt.hdr.offset == 0);
        assert(dec->decode_packet(&pkt.hdr, out, sizeof(out)));
        assert(pkt.hdr.len == 0);                           /* all of it taken */
    }
    dec->decoder_cleanup();

    /* Every input frame comes out as an int32 stereo frame, less exactly the 90
     * that libfreeaptx skips for the codec's latency (LATENCY_SAMPLES) -- one byte
     * lost to resynchronisation would show here. And it is the tone, not silence:
     * a 0.5 sine has a mean square of 0.125. */
    const size_t frames_out = pcm_bytes / 8;
    assert(frames_out == FRAMES - 90);
    const double ms = pcm_energy / (double)(frames_out * 2);
    assert(ms > 0.10 && ms < 0.15);

    printf("aptX LL: caps, gating, config and %zu decoded frames (mean square %.3f) passed\n",
           frames_out, ms);
    return 0;
}
