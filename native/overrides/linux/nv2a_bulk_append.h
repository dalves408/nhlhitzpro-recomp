/* Bulk payload ingestion for non-incrementing data ports only. No draws,
 * fences, register-state transitions or guest ownership updates occur here. */
#ifndef HITZ_NV2A_BULK_APPEND_H
#define HITZ_NV2A_BULK_APPEND_H
#include <stdint.h>
#include <string.h>
static uint32_t hitz_bulk_append(uint32_t method, uint32_t prim,
    uint32_t *indices, uint32_t *index_count, uint32_t index_capacity,
    uint32_t *inline_data, uint32_t *inline_count, uint32_t inline_capacity,
    const uint32_t *words, uint32_t n)
{
    uint32_t take, i;
    if (method != 0x1800 && method != 0x1808 && method != 0x1818) return 0;
    if (!prim || !n) return n;
    if (method == 0x1818) {
        take = *inline_count < inline_capacity ? inline_capacity-*inline_count : 0;
        if (take > n) take = n;
        memcpy(inline_data+*inline_count, words, (size_t)take*4);
        *inline_count += take;
    } else if (method == 0x1808) {
        take = *index_count < index_capacity ? index_capacity-*index_count : 0;
        if (take > n) take = n;
        memcpy(indices+*index_count, words, (size_t)take*4);
        *index_count += take;
    } else {
        take = *index_count < index_capacity ? (index_capacity-*index_count)/2 : 0;
        if (take > n) take = n;
        for (i=0;i<take;i++) {
            indices[*index_count+2*i] = words[i]&0xffffu;
            indices[*index_count+2*i+1] = words[i]>>16;
        }
        *index_count += take*2;
    }
    return n;
}
#endif
