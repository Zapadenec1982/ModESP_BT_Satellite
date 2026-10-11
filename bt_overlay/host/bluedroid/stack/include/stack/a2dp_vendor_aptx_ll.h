/*
 * SPDX-FileCopyrightText: 2016 The Android Open Source Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

//
// A2DP Codec API for aptX Low Latency
//

#ifndef A2DP_VENDOR_APTX_LL_H
#define A2DP_VENDOR_APTX_LL_H

#include "a2d_api.h"
#include "a2dp_codec_api.h"
#include "a2dp_vendor_aptx_ll_constants.h"
#include "avdt_api.h"
#include "bt_av.h"

// data type for the aptX Low Latency Codec Information Element */
typedef struct {
  uint32_t vendorId;
  uint16_t codecId;    /* Codec ID for aptX Low Latency */
  uint8_t sampleRate;  /* Sampling Frequency */
  uint8_t channelMode; /* STEREO/DUAL/MONO */
  uint8_t bidirect_link;
  uint8_t has_new_caps;
  btav_a2dp_codec_bits_per_sample_t bits_per_sample;
} tA2DP_APTX_LL_CIE;


#ifdef __cplusplus
extern "C"
{
#endif

tA2D_STATUS A2DP_ParseInfoAptxLl(tA2DP_APTX_LL_CIE* p_ie, const uint8_t* p_codec_info,
                                 bool is_capability);

tA2D_STATUS A2DP_BuildInfoAptxLl(uint8_t media_type, const tA2DP_APTX_LL_CIE* p_ie,
                                 uint8_t* p_result);

tA2D_STATUS A2DP_IsVendorPeerSourceCodecValidAptxLl(const uint8_t* p_codec_info);

bool A2DP_IsVendorPeerSinkCodecValidAptxLl(const uint8_t* p_codec_info);

btav_a2dp_codec_index_t A2DP_VendorSinkCodecIndexAptxLl(
    const uint8_t* p_codec_info);

btav_a2dp_codec_index_t A2DP_VendorSourceCodecIndexAptxLl(
    const uint8_t* p_codec_info);

const char* A2DP_VendorCodecNameAptxLl(const uint8_t* p_codec_info);

bool A2DP_VendorCodecTypeEqualsAptxLl(const uint8_t* p_codec_info_a,
                                      const uint8_t* p_codec_info_b);

bool A2DP_VendorInitCodecConfigAptxLl(btav_a2dp_codec_index_t codec_index, UINT8 *p_result);

bool A2DP_VendorBuildCodecConfigAptxLl(UINT8 *p_src_cap, UINT8 *p_result);

const tA2DP_DECODER_INTERFACE* A2DP_GetVendorDecoderInterfaceAptxLl(
    const uint8_t* p_codec_info);

void A2DP_AptxLlSinkAllow48kHz(bool allow);

void A2DP_AptxLlSinkOffer(bool offer);

#ifdef __cplusplus
}
#endif

#endif  // A2DP_VENDOR_APTX_LL_H
