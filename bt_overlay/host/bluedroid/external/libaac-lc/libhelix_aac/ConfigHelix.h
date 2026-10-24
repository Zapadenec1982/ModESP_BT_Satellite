/*
 * SPDX-FileCopyrightText: 2026 Zapadenec1982
 * SPDX-License-Identifier: MIT
 *
 * Build settings for the Helix AAC decoder in this firmware, included by
 * aaccommon.h and buffers.c. Written for this project, not part of Helix.
 *
 * The standard C library is used, not Helix's own hlxclib, which is not carried.
 * HELIX_FEATURE_AUDIO_CODEC_AAC_SBR is left undefined, so SBR is not built: the
 * AAC endpoint is AAC-LC only (CONFIG_BT_A2DP_AAC_DECODER).
 */

#ifndef CONFIG_HELIX_H
#define CONFIG_HELIX_H

#ifndef USE_DEFAULT_STDLIB
#define USE_DEFAULT_STDLIB
#endif

#endif /* CONFIG_HELIX_H */
