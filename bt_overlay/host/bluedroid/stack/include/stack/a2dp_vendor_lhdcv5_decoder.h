/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileContributor: 2026 Zapadenec1982
 *
 * Adapted from a2dp_integration/a2dp_vendor_lhdcv5_decoder.h in WillyBilly06/LHDC-V5-Decoder,
 * at the commit in lhdc.pin. Upstream names no author; its README releases the
 * repository under the Apache License 2.0 (docs/LHDC_UPSTREAM.md).
 */

#ifndef A2DP_VENDOR_LHDCV5_DECODER_H
#define A2DP_VENDOR_LHDCV5_DECODER_H

#include "a2dp_codec_api.h"
#include "a2dp_decoder.h"
#include "stack/bt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool a2dp_lhdcv5_decoder_init(decoded_data_callback_t decode_callback);
void a2dp_lhdcv5_decoder_cleanup(void);
bool a2dp_lhdcv5_decoder_reset(void);
void a2dp_lhdcv5_decoder_configure(const uint8_t* p_codec_info);
ssize_t a2dp_lhdcv5_decoder_decode_packet_header(BT_HDR* p_data);
bool a2dp_lhdcv5_decoder_decode_packet(BT_HDR* p_buf, unsigned char* buf,
                                       size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif  // A2DP_VENDOR_LHDCV5_DECODER_H
