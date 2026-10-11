/* The sink as AVDTP initiator: choosing a peer source SEP and building its config.
 *
 * bta_av_audio_sink_getconfig and its two helpers come out of the generated
 * bta_av_co.c, the SBC and aptX code is the real thing, and only the codec
 * dispatcher, the peer table and the lock are stubbed. A source SEP that passes
 * the support check but builds no config must be skipped, and with nothing
 * left getconfig must fail rather than hand BTA an uninitialised config.
 * DEVICE_SPEC.md: every endpoint is stereo only, so a mono-only aptX source is
 * not supported in the first place. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "stack/a2d_sbc.h"
#include "stack/a2dp_vendor_aptx.h"
#include "stack/a2dp_vendor_aptx_decoder.h"

/* From a2d_api.c and a2dp_codec_config.c, which would bring SDP and every codec
 * along. The dispatchers keep the two codecs this test needs. */
uint8_t A2D_BitsSet(uint64_t num) {
    if (num == 0) return A2D_SET_ZERO_BIT;
    return ((num & (num - 1)) == 0) ? A2D_SET_ONE_BIT : A2D_SET_MULTL_BIT;
}
tA2D_CODEC_TYPE A2DP_GetCodecType(const uint8_t *p_codec_info) {
    return (tA2D_CODEC_TYPE)p_codec_info[2];
}
tA2D_STATUS A2DP_IsPeerSourceCodecSupported(const uint8_t *p_codec_info) {
    switch (A2DP_GetCodecType(p_codec_info)) {
    case A2D_MEDIA_CT_SBC: return A2DP_IsPeerSourceCodecSupportedSbc(p_codec_info);
    case A2D_MEDIA_CT_NON_A2DP: return A2DP_IsVendorPeerSourceCodecValidAptx(p_codec_info);
    default: return A2D_FAIL;
    }
}
btav_a2dp_codec_index_t A2DP_SourceCodecIndex(const uint8_t *p_codec_info) {
    switch (A2DP_GetCodecType(p_codec_info)) {
    case A2D_MEDIA_CT_SBC: return A2DP_SourceCodecIndexSbc(p_codec_info);
    case A2D_MEDIA_CT_NON_A2DP: return A2DP_VendorSourceCodecIndexAptx(p_codec_info);
    default: return BTAV_A2DP_CODEC_INDEX_MAX;
    }
}
bool A2DP_BuildCodecConfig(UINT8 *p_src_cap, UINT8 *p_result) {
    switch (A2DP_GetCodecType(p_src_cap)) {
    case A2D_MEDIA_CT_SBC: return A2DP_BuildCodecConfigSbc(p_src_cap, p_result);
    case A2D_MEDIA_CT_NON_A2DP: return A2DP_VendorBuildCodecConfigAptx(p_src_cap, p_result);
    default: return false;
    }
}
const char *A2DP_CodecName(const uint8_t *p_codec_info) {
    (void)p_codec_info;
    return "test";
}

/* The aptX decoder is not under test. */
bool a2dp_aptx_decoder_init(decoded_data_callback_t cb) { (void)cb; return false; }
void a2dp_aptx_decoder_cleanup(void) {}
bool a2dp_aptx_decoder_reset(void) { return false; }
ssize_t a2dp_aptx_decoder_decode_packet_header(BT_HDR *p) { (void)p; return -1; }
bool a2dp_aptx_decoder_decode_packet(BT_HDR *p, unsigned char *b, size_t n) {
    (void)p; (void)b; (void)n; return false;
}
void a2dp_aptx_decoder_configure(const uint8_t *p) { (void)p; }

/* What bta_av_co.c and its headers provide around the extracted functions. */
typedef UINT8 tBTA_AV_HNDL;
#define BTA_AV_CP_INFO_LEN 3
#define BTC_SV_AV_AA_SINK_COUNT 4
#define BTC_SV_AV_AA_SOURCE_COUNT 4
#define FUNC_TRACE() ((void)0)
#define BTA_AV_CO_NUM_ELEMENTS(__a) (sizeof(__a)/sizeof((__a)[0]))
#include "co_types.inc"

static tBTA_AV_CO_PEER s_peer;
static int s_lock_depth;
static tBTA_AV_CO_PEER *bta_av_co_get_peer(tBTA_AV_HNDL hndl) { (void)hndl; return &s_peer; }
static void osi_mutex_global_lock(void) { s_lock_depth++; }
static void osi_mutex_global_unlock(void) { s_lock_depth--; }

#include "co_getconfig.inc"

static int s_failures;
#define CHECK(c) do { if (!(c)) { s_failures++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void sbc_caps(uint8_t *out, uint8_t min_bitpool, uint8_t max_bitpool) {
    tA2D_SBC_CIE cie = {
        A2D_SBC_IE_SAMP_FREQ_44, A2D_SBC_IE_CH_MD_JOINT | A2D_SBC_IE_CH_MD_STEREO,
        A2D_SBC_IE_BLOCKS_16, A2D_SBC_IE_SUBBAND_8, A2D_SBC_IE_ALLOC_MD_L,
        max_bitpool, min_bitpool,
    };
    memset(out, 0, AVDT_CODEC_SIZE);
    assert(A2D_BldSbcInfo(A2D_MEDIA_TYPE_AUDIO, &cie, out) == A2D_SUCCESS);
}

static void aptx_caps(uint8_t *out, uint8_t channel_mode) {
    tA2DP_APTX_CIE cie = {
        A2DP_APTX_VENDOR_ID, A2DP_APTX_CODEC_ID_BLUETOOTH, A2DP_APTX_SAMPLERATE_44100,
        channel_mode, A2DP_APTX_FUTURE_1, A2DP_APTX_FUTURE_2, BTAV_A2DP_CODEC_BITS_PER_SAMPLE_16,
    };
    memset(out, 0, AVDT_CODEC_SIZE);
    assert(A2DP_BuildInfoAptx(A2D_MEDIA_TYPE_AUDIO, &cie, out) == A2D_SUCCESS);
}

/* Leave a recognisable pattern where getconfig's locals will live, so a copy
 * of an unbuilt config shows up as 0xA5 rather than as whatever was there. */
static void __attribute__((noinline)) paint_stack(void) {
    volatile uint8_t junk[512];
    for (size_t i = 0; i < sizeof(junk); i++) junk[i] = 0xA5;
}

typedef struct {
    uint8_t result;
    uint8_t codec_info[AVDT_CODEC_SIZE];
    uint8_t sep_info_idx;
} outcome_t;

/* Feed getconfig the peer's source SEPs one GET_CAPABILITIES at a time, the way
 * bta_av_getcap_results does, and keep what the last call returned. */
static outcome_t discover(const uint8_t (*caps)[AVDT_CODEC_SIZE], int n) {
    outcome_t o = {0};
    memset(&s_peer, 0, sizeof(s_peer));
    s_peer.num_srcs = (UINT8)n;
    for (int i = 0; i < n; i++) {
        uint8_t protect[BTA_AV_CP_INFO_LEN] = {0};
        uint8_t num_protect = 0;
        memcpy(o.codec_info, caps[i], AVDT_CODEC_SIZE);
        o.sep_info_idx = (uint8_t)(10 + i);
        paint_stack();
        o.result = bta_av_audio_sink_getconfig(1, o.codec_info, &o.sep_info_idx,
                                               (UINT8)(i + 1), &num_protect, protect);
        CHECK(s_lock_depth == 0);
        if (i + 1 < n) CHECK(o.result == A2D_FAIL);
    }
    return o;
}

static void test_aptx_mono_is_not_supported(void) {
    uint8_t mono[AVDT_CODEC_SIZE], stereo[AVDT_CODEC_SIZE], cfg[AVDT_CODEC_SIZE];
    aptx_caps(mono, A2DP_APTX_CHANNELS_MONO);
    aptx_caps(stereo, A2DP_APTX_CHANNELS_STEREO);

    CHECK(A2DP_IsVendorPeerSourceCodecValidAptx(mono) == A2D_NS_CH_MODE);
    memset(cfg, 0xA5, sizeof(cfg));
    CHECK(!A2DP_VendorBuildCodecConfigAptx(mono, cfg));
    CHECK(cfg[0] == 0xA5);

    CHECK(A2DP_IsVendorPeerSourceCodecValidAptx(stereo) == A2D_SUCCESS);
    CHECK(A2DP_VendorBuildCodecConfigAptx(stereo, cfg));
    CHECK(cfg[9] == (A2DP_APTX_SAMPLERATE_44100 | A2DP_APTX_CHANNELS_STEREO));
}

/* min_bitpool 60 passes the support check (the sink cap's max is 250) but the
 * builder caps max at 53, so nothing builds. */
static void test_unbuildable_sbc_alone_fails(void) {
    uint8_t caps[1][AVDT_CODEC_SIZE];
    sbc_caps(caps[0], 60, 250);
    CHECK(A2DP_IsPeerSourceCodecSupportedSbc(caps[0]) == A2D_SUCCESS);

    outcome_t o = discover(caps, 1);
    CHECK(o.result == A2D_FAIL);
    CHECK(memcmp(o.codec_info, caps[0], AVDT_CODEC_SIZE) == 0);
    CHECK(o.sep_info_idx == 10);
    CHECK(s_peer.p_src == NULL);
}

/* The last supported SEP does not build; the one before it does. */
static void test_falls_back_past_unbuildable_sbc(void) {
    uint8_t caps[2][AVDT_CODEC_SIZE];
    sbc_caps(caps[0], 2, 53);
    sbc_caps(caps[1], 60, 250);

    outcome_t o = discover(caps, 2);
    CHECK(o.result == A2D_SUCCESS);
    CHECK(o.sep_info_idx == 10);
    CHECK(s_peer.p_src == &s_peer.srcs[0]);
    CHECK(o.codec_info[0] == A2D_SBC_INFO_LEN);
    CHECK(o.codec_info[2] == A2D_MEDIA_CT_SBC);
    CHECK(o.codec_info[3] == (A2D_SBC_IE_SAMP_FREQ_44 | A2D_SBC_IE_CH_MD_JOINT));
    CHECK(o.codec_info[5] == 2 && o.codec_info[6] == 53);
    CHECK(memcmp(s_peer.codec_cfg, o.codec_info, AVDT_CODEC_SIZE) == 0);
}

/* A mono-only aptX SEP after a good SBC one must not be what gets configured. */
static void test_mono_aptx_leaves_sbc(void) {
    uint8_t caps[2][AVDT_CODEC_SIZE];
    sbc_caps(caps[0], 2, 53);
    aptx_caps(caps[1], A2DP_APTX_CHANNELS_MONO);

    outcome_t o = discover(caps, 2);
    CHECK(o.result == A2D_SUCCESS);
    CHECK(o.sep_info_idx == 10);
    CHECK(o.codec_info[2] == A2D_MEDIA_CT_SBC);
    CHECK(o.codec_info[5] == 2 && o.codec_info[6] == 53);
}

/* And a stereo aptX SEP is still preferred over SBC, as before. */
static void test_stereo_aptx_still_chosen(void) {
    uint8_t caps[2][AVDT_CODEC_SIZE];
    sbc_caps(caps[0], 2, 53);
    aptx_caps(caps[1], A2DP_APTX_CHANNELS_STEREO);

    outcome_t o = discover(caps, 2);
    CHECK(o.result == A2D_SUCCESS);
    CHECK(o.sep_info_idx == 11);
    CHECK(o.codec_info[2] == A2D_MEDIA_CT_NON_A2DP);
    CHECK(o.codec_info[9] == (A2DP_APTX_SAMPLERATE_44100 | A2DP_APTX_CHANNELS_STEREO));
}

int main(void) {
    test_aptx_mono_is_not_supported();
    test_unbuildable_sbc_alone_fails();
    test_falls_back_past_unbuildable_sbc();
    test_mono_aptx_leaves_sbc();
    test_stereo_aptx_still_chosen();
    printf("sink getconfig: %s\n", s_failures ? "FAILED" : "ok");
    return s_failures ? 1 : 0;
}
