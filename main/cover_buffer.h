#pragma once
/* Small allocations avoid realloc's simultaneous old/new JPEG buffers. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "satlink/cover_wire.h"
typedef struct cover_block {
    struct cover_block *next;
    size_t used;
    uint8_t data[COVER_PART_MAX];
} cover_block_t;
typedef struct { cover_block_t *head, *tail; size_t size; } cover_buffer_t;

static inline void cover_buffer_clear(cover_buffer_t *b) {
    while (b->head) { cover_block_t *n=b->head->next; free(b->head); b->head=n; }
    b->tail=NULL; b->size=0;
}
/* On failure the caller clears the entire partial image and drains OBEX. */
static inline bool cover_buffer_append(cover_buffer_t *b, const uint8_t *src,
                                       size_t len, size_t limit, void *(*allocate)(size_t)) {
    if ((!src && len) || b->size>limit || len>limit-b->size) return false;
    while (len) {
        if (!b->tail || b->tail->used==COVER_PART_MAX) {
            cover_block_t *n=(cover_block_t *)allocate(sizeof(cover_block_t));
            if (!n) return false;
            n->next=NULL; n->used=0;
            if (b->tail) b->tail->next=n; else b->head=n;
            b->tail=n;
        }
        size_t count=COVER_PART_MAX-b->tail->used;
        if (count>len) count=len;
        memcpy(b->tail->data+b->tail->used,src,count);
        b->tail->used+=count; b->size+=count; src+=count; len-=count;
    }
    return true;
}
