/*
 * SPDX-FileCopyrightText: 2016 The Android Open Source Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * aptX Low Latency: a vendor codec of its own (vendor 0x0000000A, codec 0x0002)
 * whose audio is plain aptX. The codec information element is aptX's plus one
 * octet of flags, the packets carry no RTP header, and libfreeaptx decodes them
 * through the aptX decoder with the standard (not HD) context. Layout and framing
 * follow WillyBilly06/ESP32-A2DP-SINK-WITH-CODECS-UPDATED; neither has been seen
 * against a real aptX Low Latency source on this board yet.
 */

#include <string.h>
#include "a2d_int.h"
#include "common/bt_defs.h"
#include "common/bt_target.h"
#include "stack/a2d_api.h"
#include "stack/a2d_sbc.h"
#include "stack/a2dp_vendor_aptx_ll.h"
#include "stack/a2dp_vendor_aptx_decoder.h"

#if (defined(APTX_DEC_INCLUDED) && APTX_DEC_INCLUDED == TRUE)

/* aptX Low Latency Sink codec capabilities.
 *
 * Stereo only, as for aptX: libfreeaptx decodes two channels and nothing else.
 * No back channel and no extended capabilities. 48 kHz goes and comes with
 * A2DP_SinkAllow48kHz, like every other endpoint here. */
static tA2DP_APTX_LL_CIE a2dp_aptx_ll_sink_caps = {
    A2DP_APTX_LL_VENDOR_ID,                                          /* vendorId */
    A2DP_APTX_LL_CODEC_ID_BLUETOOTH,                                 /* codecId */
    (A2DP_APTX_LL_SAMPLERATE_44100 | A2DP_APTX_LL_SAMPLERATE_48000), /* sampleRate */
    A2DP_APTX_LL_CHANNELS_STEREO,                                    /* channelMode */
    0,                                                               /* bidirect_link */
    0,                                                               /* has_new_caps */
    BTAV_A2DP_CODEC_BITS_PER_SAMPLE_16 /* bits_per_sample */
};

/* Default aptX Low Latency codec configuration */
static const tA2DP_APTX_LL_CIE a2dp_aptx_ll_default_config = {
    A2DP_APTX_LL_VENDOR_ID,            /* vendorId */
    A2DP_APTX_LL_CODEC_ID_BLUETOOTH,   /* codecId */
    A2DP_APTX_LL_SAMPLERATE_48000,     /* sampleRate */
    A2DP_APTX_LL_CHANNELS_STEREO,      /* channelMode */
    0,                                 /* bidirect_link */
    0,                                 /* has_new_caps */
    BTAV_A2DP_CODEC_BITS_PER_SAMPLE_16 /* bits_per_sample */
};

static const tA2DP_DECODER_INTERFACE a2dp_decoder_interface_aptx_ll = {
    a2dp_aptx_decoder_init,
    a2dp_aptx_decoder_cleanup,
    a2dp_aptx_decoder_reset,
    a2dp_aptx_decoder_decode_packet_header,
    a2dp_aptx_decoder_decode_packet,
    NULL,  // decoder_start
    NULL,  // decoder_suspend
    a2dp_aptx_decoder_configure,
};

/* Withheld until the application asks for it, unlike the other endpoints.
 *
 * The application can only name this codec to a host that speaks protocol 1.10,
 * and an endpoint a phone may take but the host is never told about is worse than
 * none (docs/BASE.md). So the default is the safe one, and main/bt_av.c turns it
 * on when the host has said it can follow. */
static bool a2dp_aptx_ll_sink_offered = false;

void A2DP_AptxLlSinkOffer(bool offer) {
  a2dp_aptx_ll_sink_offered = offer;
}

void A2DP_AptxLlSinkAllow48kHz(bool allow) {
  if (allow) {
    a2dp_aptx_ll_sink_caps.sampleRate |= A2DP_APTX_LL_SAMPLERATE_48000;
  } else {
    a2dp_aptx_ll_sink_caps.sampleRate &= ~A2DP_APTX_LL_SAMPLERATE_48000;
  }
}

tA2D_STATUS A2DP_BuildInfoAptxLl(uint8_t media_type,
                                 const tA2DP_APTX_LL_CIE* p_ie,
                                 uint8_t* p_result) {
  if (p_ie == NULL || p_result == NULL) {
    return A2D_INVALID_PARAMS;
  }

  *p_result++ = A2DP_APTX_LL_CODEC_LEN;
  *p_result++ = (media_type << 4);
  *p_result++ = A2D_MEDIA_CT_NON_A2DP;
  *p_result++ = (uint8_t)(p_ie->vendorId & 0x000000FF);
  *p_result++ = (uint8_t)((p_ie->vendorId & 0x0000FF00) >> 8);
  *p_result++ = (uint8_t)((p_ie->vendorId & 0x00FF0000) >> 16);
  *p_result++ = (uint8_t)((p_ie->vendorId & 0xFF000000) >> 24);
  *p_result++ = (uint8_t)(p_ie->codecId & 0x00FF);
  *p_result++ = (uint8_t)((p_ie->codecId & 0xFF00) >> 8);
  *p_result++ = p_ie->sampleRate | p_ie->channelMode;
  *p_result++ = (p_ie->has_new_caps ? A2DP_APTX_LL_HAS_NEW_CAPS : 0) |
                (p_ie->bidirect_link ? A2DP_APTX_LL_BIDIRECT_LINK : 0);

  return A2D_SUCCESS;
}

tA2D_STATUS A2DP_ParseInfoAptxLl(tA2DP_APTX_LL_CIE* p_ie,
                                 const uint8_t* p_codec_info,
                                 bool is_capability) {
  uint8_t losc;
  uint8_t media_type;
  tA2D_CODEC_TYPE codec_type;

  if (p_ie == NULL || p_codec_info == NULL) return A2D_INVALID_PARAMS;

  // Check the codec capability length
  losc = *p_codec_info++;
  if (losc != A2DP_APTX_LL_CODEC_LEN) return A2D_WRONG_CODEC;

  media_type = (*p_codec_info++) >> 4;
  codec_type = *p_codec_info++;
  /* Check the Media Type and Media Codec Type */
  if (media_type != AVDT_MEDIA_AUDIO ||
      codec_type != A2D_MEDIA_CT_NON_A2DP) {
    return A2D_WRONG_CODEC;
  }

  // Check the Vendor ID and Codec ID */
  p_ie->vendorId = (*p_codec_info & 0x000000FF) |
                   (*(p_codec_info + 1) << 8 & 0x0000FF00) |
                   (*(p_codec_info + 2) << 16 & 0x00FF0000) |
                   (*(p_codec_info + 3) << 24 & 0xFF000000);
  p_codec_info += 4;
  p_ie->codecId =
      (*p_codec_info & 0x00FF) | (*(p_codec_info + 1) << 8 & 0xFF00);
  p_codec_info += 2;
  if (p_ie->vendorId != A2DP_APTX_LL_VENDOR_ID ||
      p_ie->codecId != A2DP_APTX_LL_CODEC_ID_BLUETOOTH) {
    return A2D_WRONG_CODEC;
  }

  p_ie->channelMode = *p_codec_info & 0x0F;
  p_ie->sampleRate = *p_codec_info & 0xF0;
  p_codec_info++;

  p_ie->has_new_caps = (*p_codec_info & A2DP_APTX_LL_HAS_NEW_CAPS) ? 1 : 0;
  p_ie->bidirect_link = (*p_codec_info & A2DP_APTX_LL_BIDIRECT_LINK) ? 1 : 0;
  p_ie->bits_per_sample = BTAV_A2DP_CODEC_BITS_PER_SAMPLE_16;

  if (is_capability) {
    // NOTE: The checks here are very liberal. We should be using more
    // pedantic checks specific to the SRC or SNK as specified in the spec.
    if (A2D_BitsSet(p_ie->sampleRate) == A2D_SET_ZERO_BIT)
      return A2D_BAD_SAMP_FREQ;
    if (A2D_BitsSet(p_ie->channelMode) == A2D_SET_ZERO_BIT)
      return A2D_BAD_CH_MODE;

    return A2D_SUCCESS;
  }

  if (A2D_BitsSet(p_ie->sampleRate) != A2D_SET_ONE_BIT)
    return A2D_BAD_SAMP_FREQ;
  if (A2D_BitsSet(p_ie->channelMode) != A2D_SET_ONE_BIT)
    return A2D_BAD_CH_MODE;
  return A2D_SUCCESS;
}

tA2D_STATUS A2DP_IsVendorPeerSourceCodecValidAptxLl(const uint8_t* p_codec_info) {
  tA2DP_APTX_LL_CIE cfg_cie;

  /* Withheld means not acceptable either. A phone cannot configure an endpoint
   * that was never registered, but when this device opens the connection it picks
   * from the phone's endpoints through here, and a withheld codec must not be
   * picked: the host may be one that cannot name it. */
  if (!a2dp_aptx_ll_sink_offered) {
    return A2D_NS_CODEC_TYPE;
  }

  /* Parse first, then check against what this device offers, as for aptX. */
  tA2D_STATUS status = A2DP_ParseInfoAptxLl(&cfg_cie, p_codec_info, true);
  if (status != A2D_SUCCESS) {
    status = A2DP_ParseInfoAptxLl(&cfg_cie, p_codec_info, false);
  }
  if (status != A2D_SUCCESS) {
    return status;
  }

  if ((cfg_cie.sampleRate & a2dp_aptx_ll_sink_caps.sampleRate) == 0) {
    APPL_TRACE_WARNING("%s: rate 0x%x is not one of the 0x%x this device offers",
                       __func__, cfg_cie.sampleRate, a2dp_aptx_ll_sink_caps.sampleRate);
    return A2D_NS_SAMP_FREQ;
  }
  if ((cfg_cie.channelMode & a2dp_aptx_ll_sink_caps.channelMode) == 0) {
    APPL_TRACE_WARNING("%s: channel mode 0x%x, this device decodes stereo only",
                       __func__, cfg_cie.channelMode);
    return A2D_NS_CH_MODE;
  }

  return A2D_SUCCESS;
}

bool A2DP_IsVendorPeerSinkCodecValidAptxLl(const uint8_t* p_codec_info) {
  tA2DP_APTX_LL_CIE cfg_cie;

  /* Use a liberal check when parsing the codec info */
  return (A2DP_ParseInfoAptxLl(&cfg_cie, p_codec_info, false) == A2D_SUCCESS) ||
         (A2DP_ParseInfoAptxLl(&cfg_cie, p_codec_info, true) == A2D_SUCCESS);
}

btav_a2dp_codec_index_t A2DP_VendorSinkCodecIndexAptxLl(
    const uint8_t* p_codec_info) {
  UNUSED(p_codec_info);
  return BTAV_A2DP_CODEC_INDEX_SINK_APTX_LL;
}

btav_a2dp_codec_index_t A2DP_VendorSourceCodecIndexAptxLl(
    const uint8_t* p_codec_info) {
  UNUSED(p_codec_info);
  return BTAV_A2DP_CODEC_INDEX_SOURCE_APTX_LL;
}

const char* A2DP_VendorCodecNameAptxLl(const uint8_t* p_codec_info) {
  UNUSED(p_codec_info);
  return "aptX-LL";
}

bool A2DP_VendorCodecTypeEqualsAptxLl(const uint8_t* p_codec_info_a,
                                      const uint8_t* p_codec_info_b) {
  tA2DP_APTX_LL_CIE aptx_ll_cie_a;
  tA2DP_APTX_LL_CIE aptx_ll_cie_b;

  // Check whether the codec info contains valid data
  tA2D_STATUS a2dp_status =
      A2DP_ParseInfoAptxLl(&aptx_ll_cie_a, p_codec_info_a, true);
  if (a2dp_status != A2D_SUCCESS) {
    if (a2dp_status != A2D_WRONG_CODEC)
      LOG_ERROR("%s: cannot decode codec information: %d", __func__, a2dp_status);
    return false;
  }
  a2dp_status = A2DP_ParseInfoAptxLl(&aptx_ll_cie_b, p_codec_info_b, true);
  if (a2dp_status != A2D_SUCCESS) {
    if (a2dp_status != A2D_WRONG_CODEC)
      LOG_ERROR("%s: cannot decode codec information: %d", __func__, a2dp_status);
    return false;
  }

  return true;
}

bool A2DP_VendorInitCodecConfigAptxLl(btav_a2dp_codec_index_t codec_index, UINT8 *p_result) {
  if (codec_index != BTAV_A2DP_CODEC_INDEX_SINK_APTX_LL || !a2dp_aptx_ll_sink_offered) {
    return false;
  }
  return A2DP_BuildInfoAptxLl(A2D_MEDIA_TYPE_AUDIO, &a2dp_aptx_ll_sink_caps,
                              p_result) == A2D_SUCCESS;
}

bool A2DP_VendorBuildCodecConfigAptxLl(UINT8 *p_src_cap, UINT8 *p_result) {
  tA2DP_APTX_LL_CIE src_cap;
  tA2DP_APTX_LL_CIE pref_cap = a2dp_aptx_ll_default_config;
  tA2D_STATUS status;

  if ((status = A2DP_ParseInfoAptxLl(&src_cap, p_src_cap, TRUE)) != 0) {
    APPL_TRACE_ERROR("%s: Cant parse src cap ret = %d", __func__, status);
    return false;
  }

  /* Intersect with what this device offers before choosing; see
   * A2DP_VendorBuildCodecConfigAptx for why. */
  uint8_t allowed = src_cap.sampleRate & a2dp_aptx_ll_sink_caps.sampleRate;
  if (allowed & A2DP_APTX_LL_SAMPLERATE_48000) {
    pref_cap.sampleRate = A2DP_APTX_LL_SAMPLERATE_48000;
  } else if (allowed & A2DP_APTX_LL_SAMPLERATE_44100) {
    pref_cap.sampleRate = A2DP_APTX_LL_SAMPLERATE_44100;
  } else {
    APPL_TRACE_ERROR("%s: Unsupported sample rate 0x%x", __func__,
                     src_cap.sampleRate);
    return false;
  }

  if (!(src_cap.channelMode & A2DP_APTX_LL_CHANNELS_STEREO)) {
    APPL_TRACE_ERROR("%s: Unsupported channel mode 0x%x", __func__,
                     src_cap.channelMode);
    return false;
  }

  A2DP_BuildInfoAptxLl(A2D_MEDIA_TYPE_AUDIO, &pref_cap, p_result);
  return true;
}

const tA2DP_DECODER_INTERFACE* A2DP_GetVendorDecoderInterfaceAptxLl(
    const uint8_t* p_codec_info) {
  if (!A2DP_IsVendorPeerSinkCodecValidAptxLl(p_codec_info)) return NULL;

  return &a2dp_decoder_interface_aptx_ll;
}

#endif /* defined(APTX_DEC_INCLUDED) && APTX_DEC_INCLUDED == TRUE) */
