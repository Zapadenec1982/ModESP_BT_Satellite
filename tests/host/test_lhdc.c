/* Actual decoder/adapter; no proprietary encoder or known-good audio vectors.
 * These tests cover negotiation, lifetime and packet/buffer boundaries only. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stack/a2dp_vendor_lhdcv5.h"
#include "stack/a2dp_vendor_lhdcv5_decoder.h"
#include "lhdc_dec.h"
static unsigned callbacks;
/* Upstream's host-only diagnostic switches, normally supplied by its encoder harness. */
int g_nzc_formula = 0, g_pkframe = 0;
static void pcm_callback(uint8_t *data, uint32_t len) {
    assert(data && len && len <= 3840); callbacks++;
}
typedef struct { BT_HDR hdr; uint8_t data[4096]; } packet_t;
static void packet(packet_t *p, uint16_t seq, uint8_t flags, unsigned bytes) {
    memset(p, 0, sizeof(*p));
    p->hdr.len = 13 + bytes;
    p->data[0] = 0x80;
    p->data[2] = seq >> 8; p->data[3] = seq;
    p->data[12] = flags;
}
int main(void) {
    uint8_t caps[20], cfg[20], selected[20];
    tA2DP_LHDCV5_CIE cie;
    assert(A2DP_VendorInitCodecConfigLhdcV5Sink(caps));
    assert(caps[9] == 0x20 && caps[10] == 6 && caps[12] == 0 && caps[13] == 0);
    A2DP_LhdcV5SinkAllow96kHz(true);
    assert(A2DP_VendorInitCodecConfigLhdcV5Sink(caps));
    assert(caps[9] == 0x20); /* RELOCK is a separate prerequisite. */
    A2DP_LhdcV5SinkAllow48kHz(true);
    assert(A2DP_VendorInitCodecConfigLhdcV5Sink(caps));
    assert(caps[9] == 0x34); /* Never 192 kHz. */
    assert(A2DP_IsVendorPeerSourceCodecSupportedLhdcV5(caps) == A2D_SUCCESS);
    assert(A2DP_IsVendorPeerSourceCodecValidLhdcV5(caps) != A2D_SUCCESS);
    assert(A2DP_VendorBuildCodecConfigLhdcV5(caps, selected));
    assert(selected[9] == 4 && selected[10] == 2);
    assert(A2DP_ParseInfoLhdcV5(&cie, selected, false) == A2D_SUCCESS);
    for (unsigned rate=0; rate<256; ++rate) {
        memcpy(cfg, selected, sizeof(cfg)); cfg[9] = rate;
        const bool accepted = A2DP_IsVendorPeerSourceCodecValidLhdcV5(cfg) == A2D_SUCCESS;
        /* Reserved bits are ignored; selected defined rate bits must be singular. */
        unsigned masked = rate & 0x35;
        assert(accepted == (masked == 4 || masked == 0x10 || masked == 0x20));
    }
    for (unsigned depth=0; depth<8; ++depth) {
        memcpy(cfg, selected, sizeof(cfg)); cfg[10] = depth;
        assert((A2DP_ParseInfoLhdcV5(&cie,cfg,false) == A2D_SUCCESS) == (depth == 2 || depth == 4));
    }
    for (unsigned feature=1; feature<=128; feature<<=1) {
        if (!(feature & 0xf7)) continue;
        memcpy(cfg, selected, sizeof(cfg)); cfg[12] = feature;
        assert(A2DP_ParseInfoLhdcV5(&cie,cfg,false) != A2D_SUCCESS);
    }
    A2DP_LhdcV5SinkAllow96kHz(false);
    assert(A2DP_ParseInfoLhdcV5(&cie,selected,false) != A2D_SUCCESS);
    assert(A2DP_VendorBuildCodecConfigLhdcV5(caps,cfg) && cfg[9] == 0x10);
    A2DP_LhdcV5SinkAllow96kHz(true);
    assert(!a2dp_lhdcv5_decoder_init(NULL));
    packet_t p;
    struct { uint32_t before; uint8_t pcm[4096]; uint32_t after; } output;
    output.before = 0xdeadbeef; output.after = 0xcafebabe;
    const uint8_t rates[] = {0x20, 0x10, 4, 0x10, 0x20, 4};
    for (unsigned cycle=0; cycle<18; ++cycle) {
        assert(a2dp_lhdcv5_decoder_init(pcm_callback));
        for (unsigned r=0; r<sizeof(rates); ++r) {
            memcpy(cfg,selected,sizeof(cfg)); cfg[9]=rates[r]; cfg[10]=(r&1)?4:2;
            a2dp_lhdcv5_decoder_configure(cfg);
            assert(a2dp_lhdcv5_decoder_reset());
            for (unsigned length=0; length<14; ++length) {
                packet(&p,0,0,0); p.hdr.len=length;
                if (length < 13) assert(a2dp_lhdcv5_decoder_decode_packet_header(&p.hdr) < 0);
            }
            packet(&p,0,0,4);
            assert(a2dp_lhdcv5_decoder_decode_packet_header(&p.hdr) == 0);
            assert(!a2dp_lhdcv5_decoder_decode_packet(&p.hdr,output.pcm,sizeof(output.pcm)));
            /* RTP extension claims bytes not present. */
            packet(&p,0,0,4); p.data[0] |= 0x10; p.data[15]=255;
            assert(a2dp_lhdcv5_decoder_decode_packet_header(&p.hdr) < 0);
            /* Continuation without first; then dropped fragment sequence. */
            packet(&p,1,0xa1,5);
            assert(a2dp_lhdcv5_decoder_decode_packet_header(&p.hdr) < 0);
            packet(&p,2,0xc2,10);
            assert(a2dp_lhdcv5_decoder_decode_packet_header(&p.hdr) == 0);
            assert(a2dp_lhdcv5_decoder_decode_packet(&p.hdr,output.pcm,sizeof(output.pcm)));
            packet(&p,4,0xa1,10);
            assert(a2dp_lhdcv5_decoder_decode_packet_header(&p.hdr) < 0);
            /* Reassembly overflow must discard state. */
            packet(&p,5,0xc2,3070);
            assert(a2dp_lhdcv5_decoder_decode_packet_header(&p.hdr) == 0);
            assert(a2dp_lhdcv5_decoder_decode_packet(&p.hdr,output.pcm,sizeof(output.pcm)));
            packet(&p,6,0xa1,8);
            assert(a2dp_lhdcv5_decoder_decode_packet_header(&p.hdr) == 0);
            assert(!a2dp_lhdcv5_decoder_decode_packet(&p.hdr,output.pcm,sizeof(output.pcm)));
            assert(output.before == 0xdeadbeef && output.after == 0xcafebabe);
        }
        a2dp_lhdcv5_decoder_cleanup();
        a2dp_lhdcv5_decoder_cleanup();
    }
    assert(callbacks == 0);
    /* Core checks output capacity before touching PCM, even for a complete frame. */
    lhdc_dec_config_t config = {96000,24,5,2,3072,0};
    void *ws=calloc(1,lhdc_dec_get_workspace_size(96000,5));
    lhdc_decoder_t *dec=lhdc_dec_init(ws,&config); assert(dec);
    uint8_t frame[202]={100,0};
    size_t consumed; uint32_t generated;
    assert(lhdc_dec_decode_frame(dec,frame,sizeof(frame),output.pcm,479,&consumed,&generated,NULL) == LHDC_DEC_BUF_NOT_ENOUGH);
    assert(output.before == 0xdeadbeef && output.after == 0xcafebabe);
    lhdc_dec_deinit(dec); free(ws);
    puts("LHDC: negotiation, 108 rate/depth reconfigurations, RTP/reassembly and PCM bounds passed");
    return 0;
}
