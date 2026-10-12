/* The sink as AVDTP acceptor: checking the source's SET_CONFIGURATION.
 *
 * bta_av_co_audio_setconfig and the codec dispatchers come out of the generated
 * bta_av_co.c, a2dp_codec_config.c and a2dp_vendor.c; SBC, AAC, aptX, aptX HD
 * and LHDC V5 are the real thing. Only the peer table, the lock and the call-in
 * are stubbed.
 *
 * A configuration names one value per field (A2DP, and stock's
 * A2D_ParsSbcInfo(..., FALSE)). Checked as a capability, 44.1|48 intersects a
 * 44.1-only offer and passes, and main/bt_av.c, testing 48 first, then takes the
 * stream for 48 kHz: the very rate DEVICE_SPEC withholds from a host that cannot
 * relock. Discovery still sees masks and must keep accepting them. */
#include <stdio.h>
#include <string.h>
#include "stack/a2d_sbc.h"
#include "stack/a2dp_aac.h"
#include "stack/a2dp_vendor.h"
#include "stack/a2dp_vendor_aptx.h"
#include "stack/a2dp_vendor_aptx_hd.h"
#include "stack/a2dp_vendor_aptx_decoder.h"
#include "stack/a2dp_aac_decoder.h"
#include "stack/a2dp_vendor_lhdcv5.h"
#include "stack/a2dp_vendor_lhdcv5_decoder.h"
#include "stack/avdt_api.h"

/* From a2d_api.c, which would bring SDP along. */
uint8_t A2D_BitsSet(uint64_t num) {
    if (num == 0) return A2D_SET_ZERO_BIT;
    return ((num & (num - 1)) == 0) ? A2D_SET_ONE_BIT : A2D_SET_MULTL_BIT;
}

/* The decoders are not under test. */
bool a2dp_aptx_decoder_init(decoded_data_callback_t cb) { (void)cb; return false; }
void a2dp_aptx_decoder_cleanup(void) {}
bool a2dp_aptx_decoder_reset(void) { return false; }
ssize_t a2dp_aptx_decoder_decode_packet_header(BT_HDR *p) { (void)p; return -1; }
bool a2dp_aptx_decoder_decode_packet(BT_HDR *p, unsigned char *b, size_t n) {
    (void)p; (void)b; (void)n; return false;
}
void a2dp_aptx_decoder_configure(const uint8_t *p) { (void)p; }
bool a2dp_aac_decoder_init(decoded_data_callback_t cb) { (void)cb; return false; }
void a2dp_aac_decoder_cleanup(void) {}
bool a2dp_aac_decoder_reset(void) { return false; }
ssize_t a2dp_aac_decoder_decode_packet_header(BT_HDR *p) { (void)p; return -1; }
bool a2dp_aac_decoder_decode_packet(BT_HDR *p, unsigned char *b, size_t n) {
    (void)p; (void)b; (void)n; return false;
}
void a2dp_aac_decoder_configure(const uint8_t *p) { (void)p; }
bool a2dp_lhdcv5_decoder_init(decoded_data_callback_t cb) { (void)cb; return false; }
void a2dp_lhdcv5_decoder_cleanup(void) {}
bool a2dp_lhdcv5_decoder_reset(void) { return false; }
ssize_t a2dp_lhdcv5_decoder_decode_packet_header(BT_HDR *p) { (void)p; return -1; }
bool a2dp_lhdcv5_decoder_decode_packet(BT_HDR *p, unsigned char *b, size_t n) {
    (void)p; (void)b; (void)n; return false;
}
void a2dp_lhdcv5_decoder_configure(const uint8_t *p) { (void)p; }

#include "codec_dispatch.inc"

/* What bta_av_co.c and its headers provide around the extracted function. */
typedef UINT8 tBTA_AV_HNDL;
#define BTA_AV_CP_INFO_LEN 3
#define BTC_SV_AV_AA_SINK_COUNT 5
#define BTC_SV_AV_AA_SOURCE_COUNT 5
#define FUNC_TRACE() ((void)0)
#include "co_types.inc"

static struct {
    struct { UINT8 info[AVDT_CODEC_SIZE]; } codec_cfg, codec_cfg_setconfig;
    struct { BOOLEAN active; } cp;
} bta_av_co_cb;

static tBTA_AV_CO_PEER s_peer;
static int s_lock_depth;
static int s_ci_calls;
static UINT8 s_ci_err, s_ci_category;

static tBTA_AV_CO_PEER *bta_av_co_get_peer(tBTA_AV_HNDL hndl) { (void)hndl; return &s_peer; }
static void osi_mutex_global_lock(void) { s_lock_depth++; }
static void osi_mutex_global_unlock(void) { s_lock_depth--; }
bool A2DP_CodecTypeEquals(const uint8_t *a, const uint8_t *b) {
    return A2DP_GetCodecType(a) == A2DP_GetCodecType(b);
}
static void bta_av_ci_setconfig(tBTA_AV_HNDL hndl, UINT8 err_code, UINT8 category,
                                UINT8 num_seid, UINT8 *p_seid, BOOLEAN recfg_needed,
                                UINT8 avdt_handle) {
    (void)hndl; (void)num_seid; (void)p_seid; (void)recfg_needed; (void)avdt_handle;
    s_ci_calls++;
    s_ci_err = err_code;
    s_ci_category = category;
}

#include "co_setconfig.inc"

static int s_failures;
#define CHECK(c) do { if (!(c)) { s_failures++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EXPECT(cfg, want) expect(#cfg, cfg, want, __LINE__)

/* Run one SET_CONFIGURATION into the local sink and return what BTA was told. */
static UINT8 setconfig(const uint8_t *cfg) {
    uint8_t info[AVDT_CODEC_SIZE] = {0};
    uint8_t protect[BTA_AV_CP_INFO_LEN] = {0};
    BD_ADDR addr = {0};
    memcpy(info, cfg, (size_t)cfg[0] + 1);
    memset(&s_peer, 0, sizeof(s_peer));
    memset(&bta_av_co_cb, 0, sizeof(bta_av_co_cb));
    s_ci_calls = 0;
    s_ci_err = s_ci_category = 0xFF;
    bta_av_co_audio_setconfig(1, info, 1, addr, 0, protect, AVDT_TSEP_SNK, 1);
    CHECK(s_ci_calls == 1);
    CHECK(s_lock_depth == 0);
    if (s_ci_err == A2D_SUCCESS) {
        CHECK(s_peer.acp);
        CHECK(memcmp(bta_av_co_cb.codec_cfg.info, info, AVDT_CODEC_SIZE) == 0);
    } else {
        CHECK(s_ci_category == AVDT_ASC_CODEC);
        CHECK(!s_peer.acp);
    }
    return s_ci_err;
}

static void expect(const char *name, const uint8_t *cfg, UINT8 want, int line) {
    UINT8 got = setconfig(cfg);
    if (got != want) {
        s_failures++;
        printf("  FAIL line %d: %s answered 0x%02X, want 0x%02X\n", line, name, got, want);
    }
}

/* SBC octets: rate | channel mode, blocks | subbands | allocation, bitpools. */
#define SBC(rate, ch, blk, sb, alloc) \
    ((const uint8_t[]){ A2D_SBC_INFO_LEN, A2D_MEDIA_TYPE_AUDIO, A2D_MEDIA_CT_SBC, \
                        (rate) | (ch), (blk) | (sb) | (alloc), 2, 53 })
#define SBC_44  A2D_SBC_IE_SAMP_FREQ_44
#define SBC_48  A2D_SBC_IE_SAMP_FREQ_48
#define SBC_JS  A2D_SBC_IE_CH_MD_JOINT
#define SBC_ST  A2D_SBC_IE_CH_MD_STEREO
#define SBC_B16 A2D_SBC_IE_BLOCKS_16
#define SBC_S8  A2D_SBC_IE_SUBBAND_8
#define SBC_L   A2D_SBC_IE_ALLOC_MD_L

/* AAC octets: object type, rate (12 bits across two octets) | channels, VBR | bit rate. */
#define AAC(obj, rate, ch) \
    ((const uint8_t[]){ A2DP_AAC_CODEC_LEN, A2D_MEDIA_TYPE_AUDIO, A2D_MEDIA_CT_M24, (obj), \
                        (uint8_t)((rate) & 0xFF), (uint8_t)(((rate) >> 8) | (ch)), \
                        0x80 | 0x05, 0x00, 0x00 })
#define AAC_LC  A2DP_AAC_OBJECT_TYPE_MPEG2_LC
#define AAC_44  A2DP_AAC_SAMPLING_FREQ_44100
#define AAC_48  A2DP_AAC_SAMPLING_FREQ_48000
#define AAC_ST  A2DP_AAC_CHANNEL_MODE_STEREO
#define AAC_MO  A2DP_AAC_CHANNEL_MODE_MONO

#define APTX(rate_ch) \
    ((const uint8_t[]){ A2DP_APTX_CODEC_LEN, A2D_MEDIA_TYPE_AUDIO, A2D_MEDIA_CT_NON_A2DP, \
                        A2DP_APTX_VENDOR_ID & 0xFF, (A2DP_APTX_VENDOR_ID >> 8) & 0xFF, 0, 0, \
                        A2DP_APTX_CODEC_ID_BLUETOOTH & 0xFF, A2DP_APTX_CODEC_ID_BLUETOOTH >> 8, \
                        (rate_ch) })
#define APTX_HD(rate_ch) \
    ((const uint8_t[]){ A2DP_APTX_HD_CODEC_LEN, A2D_MEDIA_TYPE_AUDIO, A2D_MEDIA_CT_NON_A2DP, \
                        A2DP_APTX_HD_VENDOR_ID & 0xFF, (A2DP_APTX_HD_VENDOR_ID >> 8) & 0xFF, 0, 0, \
                        A2DP_APTX_HD_CODEC_ID_BLUETOOTH & 0xFF, A2DP_APTX_HD_CODEC_ID_BLUETOOTH >> 8, \
                        (rate_ch), 0, 0, 0, 0 })
/* aptX and aptX HD share the octet layout: rate high nibble, channels low. */
#define APTX_44 A2DP_APTX_SAMPLERATE_44100
#define APTX_48 A2DP_APTX_SAMPLERATE_48000
#define APTX_ST A2DP_APTX_CHANNELS_STEREO
#define APTX_MO A2DP_APTX_CHANNELS_MONO

#define LHDC(rate) \
    ((const uint8_t[]){ A2DP_LHDCV5_CODEC_LEN, A2D_MEDIA_TYPE_AUDIO, A2D_MEDIA_CT_NON_A2DP, \
                        A2DP_LHDCV5_VENDOR_ID & 0xFF, (A2DP_LHDCV5_VENDOR_ID >> 8) & 0xFF, 0, 0, \
                        A2DP_LHDCV5_CODEC_ID & 0xFF, A2DP_LHDCV5_CODEC_ID >> 8, (rate), \
                        A2DP_LHDCV5_BITS_PER_SAMPLE_24, \
                        A2DP_LHDCV5_VER_1 | A2DP_LHDCV5_FRAME_LEN_5MS, 0, 0 })
#define LHDC_44 A2DP_LHDCV5_SAMPLING_FREQ_44100
#define LHDC_48 A2DP_LHDCV5_SAMPLING_FREQ_48000

static void test_host_without_relock(void) {
    printf("host without RELOCK: 44.1 kHz only\n");
    A2DP_SinkAllow48kHz(false);

    EXPECT(SBC(SBC_44, SBC_JS, SBC_B16, SBC_S8, SBC_L), A2D_SUCCESS);
    EXPECT(SBC(SBC_44 | SBC_48, SBC_JS, SBC_B16, SBC_S8, SBC_L), A2D_BAD_SAMP_FREQ);
    EXPECT(SBC(SBC_48, SBC_JS, SBC_B16, SBC_S8, SBC_L), A2D_NS_SAMP_FREQ);
    EXPECT(SBC(SBC_44, SBC_JS | SBC_ST, SBC_B16, SBC_S8, SBC_L), A2D_BAD_CH_MODE);
    EXPECT(SBC(SBC_44, SBC_JS, SBC_B16 | A2D_SBC_IE_BLOCKS_8, SBC_S8, SBC_L), A2D_BAD_BLOCK_LEN);
    EXPECT(SBC(SBC_44, SBC_JS, SBC_B16, SBC_S8 | A2D_SBC_IE_SUBBAND_4, SBC_L), A2D_BAD_SUBBANDS);
    EXPECT(SBC(SBC_44, SBC_JS, SBC_B16, SBC_S8, SBC_L | A2D_SBC_IE_ALLOC_MD_S), A2D_BAD_ALLOC_MTHD);

    EXPECT(AAC(AAC_LC, AAC_44, AAC_ST), A2D_SUCCESS);
    EXPECT(AAC(AAC_LC, AAC_44 | AAC_48, AAC_ST), A2D_BAD_SAMP_FREQ);
    EXPECT(AAC(AAC_LC, AAC_48, AAC_ST), A2D_BAD_SAMP_FREQ);
    EXPECT(AAC(AAC_LC, AAC_44, AAC_ST | AAC_MO), A2D_BAD_CH_MODE);
    EXPECT(AAC(AAC_LC | A2DP_AAC_OBJECT_TYPE_MPEG4_LC, AAC_44, AAC_ST), A2D_BAD_OBJ_TYPE);

    EXPECT(APTX(APTX_44 | APTX_ST), A2D_SUCCESS);
    EXPECT(APTX(APTX_44 | APTX_48 | APTX_ST), A2D_BAD_SAMP_FREQ);
    EXPECT(APTX(APTX_48 | APTX_ST), A2D_NS_SAMP_FREQ);
    EXPECT(APTX(APTX_44 | APTX_ST | APTX_MO), A2D_BAD_CH_MODE);

    EXPECT(APTX_HD(APTX_44 | APTX_ST), A2D_SUCCESS);
    EXPECT(APTX_HD(APTX_44 | APTX_48 | APTX_ST), A2D_BAD_SAMP_FREQ);
    EXPECT(APTX_HD(APTX_48 | APTX_ST), A2D_NS_SAMP_FREQ);
    EXPECT(APTX_HD(APTX_44 | APTX_ST | APTX_MO), A2D_BAD_CH_MODE);

    /* 0003's own check, now reached through the same dispatcher. */
    EXPECT(LHDC(LHDC_44), A2D_SUCCESS);
    EXPECT(LHDC(LHDC_44 | LHDC_48), A2D_BAD_SAMP_FREQ);
    EXPECT(LHDC(LHDC_48), A2D_BAD_SAMP_FREQ);
}

static void test_host_with_relock(void) {
    printf("host with RELOCK: 48 kHz offered, still one rate per config\n");
    A2DP_SinkAllow48kHz(true);

    EXPECT(SBC(SBC_48, SBC_JS, SBC_B16, SBC_S8, SBC_L), A2D_SUCCESS);
    EXPECT(SBC(SBC_44 | SBC_48, SBC_JS, SBC_B16, SBC_S8, SBC_L), A2D_BAD_SAMP_FREQ);
    EXPECT(AAC(AAC_LC, AAC_48, AAC_ST), A2D_SUCCESS);
    EXPECT(AAC(AAC_LC, AAC_44 | AAC_48, AAC_ST), A2D_BAD_SAMP_FREQ);
    EXPECT(APTX(APTX_48 | APTX_ST), A2D_SUCCESS);
    EXPECT(APTX(APTX_44 | APTX_48 | APTX_ST), A2D_BAD_SAMP_FREQ);
    EXPECT(APTX_HD(APTX_48 | APTX_ST), A2D_SUCCESS);
    EXPECT(APTX_HD(APTX_44 | APTX_48 | APTX_ST), A2D_BAD_SAMP_FREQ);
    EXPECT(LHDC(LHDC_48), A2D_SUCCESS);
    EXPECT(LHDC(LHDC_44 | LHDC_48), A2D_BAD_SAMP_FREQ);

    A2DP_SinkAllow48kHz(false);
}

static void test_discovery_keeps_masks(void) {
    printf("discovery: a source's capability masks are still supported\n");
    A2DP_SinkAllow48kHz(false);
    CHECK(A2DP_IsPeerSourceCodecSupported(
              SBC(SBC_44 | SBC_48, SBC_JS | SBC_ST | A2D_SBC_IE_CH_MD_MONO | A2D_SBC_IE_CH_MD_DUAL,
                  A2D_SBC_IE_BLOCKS_4 | A2D_SBC_IE_BLOCKS_8 | A2D_SBC_IE_BLOCKS_12 | SBC_B16,
                  A2D_SBC_IE_SUBBAND_4 | SBC_S8, SBC_L | A2D_SBC_IE_ALLOC_MD_S)) == A2D_SUCCESS);
    CHECK(A2DP_IsPeerSourceCodecSupported(AAC(AAC_LC | A2DP_AAC_OBJECT_TYPE_MPEG4_LC,
                                              AAC_44 | AAC_48, AAC_ST | AAC_MO)) == A2D_SUCCESS);
    CHECK(A2DP_IsPeerSourceCodecSupported(APTX(APTX_44 | APTX_48 | APTX_ST)) == A2D_SUCCESS);
    CHECK(A2DP_IsPeerSourceCodecSupported(APTX_HD(APTX_44 | APTX_48 | APTX_ST)) == A2D_SUCCESS);
    CHECK(A2DP_IsPeerSourceCodecSupported(LHDC(LHDC_44 | LHDC_48)) == A2D_SUCCESS);
}

int main(void) {
    test_host_without_relock();
    test_host_with_relock();
    test_discovery_keeps_masks();
    if (s_failures) {
        printf("sink setconfig: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("sink setconfig: all passed\n");
    return 0;
}
