/*
 * SPDX-FileCopyrightText: 2026 Zapadenec1982
 * SPDX-License-Identifier: MIT
 *
 * The allocator AllocateBuffers() and FreeBuffers() in buffers.c call: the C
 * library's. In this firmware neither runs, because aac_decoder_helix.c gives the
 * decoder static memory through AACInitDecoderPre(). Written for this project,
 * not part of Helix.
 */

#ifndef HELIX_MEMORY_H
#define HELIX_MEMORY_H

#include <stdlib.h>

static inline void *helix_malloc(int size) { return malloc((size_t)size); }
static inline void helix_free(void *ptr) { free(ptr); }

#endif /* HELIX_MEMORY_H */
