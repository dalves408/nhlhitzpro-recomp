#ifndef HITZ_TEXTURE_MEMORY_HASH_H
#define HITZ_TEXTURE_MEMORY_HASH_H
#include <stdint.h>
#include <stddef.h>
#define XXH_INLINE_ALL
#include "../third_party/xxhash.h"
/* Read every byte on every use, including the tail. These transient cache
 * keys are separate from the stable replacement-pack content identifiers. */
static uint64_t texture_memory_hash(const unsigned char *p, size_t n)
{
    return XXH3_64bits(p, n);
}
#endif
