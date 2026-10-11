/*
 * SPDX-FileCopyrightText: 2016 The Android Open Source Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

//
// A2DP constants for aptX Low Latency codec
//

#ifndef A2DP_VENDOR_APTX_LL_CONSTANTS_H
#define A2DP_VENDOR_APTX_LL_CONSTANTS_H

/* aptX Low Latency codec specific settings.
 *
 * The same octet as aptX for rate and channel mode, then one octet of flags. The
 * values are the ones WillyBilly06/ESP32-A2DP-SINK-WITH-CODECS-UPDATED uses; the
 * extended form a source announces with "new caps" set is longer and is not
 * parsed here, so the sink never sets that bit. */
#define A2DP_APTX_LL_CODEC_LEN 10

#define A2DP_APTX_LL_VENDOR_ID 0x0000000A
#define A2DP_APTX_LL_CODEC_ID_BLUETOOTH 0x0002
#define A2DP_APTX_LL_SAMPLERATE_44100 0x20
#define A2DP_APTX_LL_SAMPLERATE_48000 0x10
#define A2DP_APTX_LL_CHANNELS_STEREO 0x02
#define A2DP_APTX_LL_CHANNELS_MONO 0x01
#define A2DP_APTX_LL_BIDIRECT_LINK 0x01
#define A2DP_APTX_LL_HAS_NEW_CAPS 0x02

#endif  // A2DP_VENDOR_APTX_LL_CONSTANTS_H
