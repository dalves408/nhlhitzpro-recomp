#ifndef HITZ_TEXTURE_CONTENT_HASH_H
#define HITZ_TEXTURE_CONTENT_HASH_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* Stable replacement-pack key: CRC32 and Adler32 of dimensions + BGRA bytes.
 * Guest-memory cache validation is separate and must remain active. */
static uint64_t px_hash(uint32_t w, uint32_t hh, const uint32_t *px)
{
    static uint32_t tab[256]; static int init;
    uint32_t crc = 0xFFFFFFFFu, a = 1, b = 0;
    uint8_t wh[8];
    const uint8_t *p = (const uint8_t *)px;
    size_t n = (size_t)w * hh * 4, i, k;
    if (!init) {
        for (i = 0; i < 256; i++) { uint32_t c = (uint32_t)i; int j; for (j = 0; j < 8; j++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; }
        init = 1;
    }
    memcpy(wh, &w, 4); memcpy(wh + 4, &hh, 4);
    for (i = 0; i < 8; i++) { crc = tab[(crc ^ wh[i]) & 0xFF] ^ (crc >> 8); a += wh[i]; b += a; }
    a %= 65521u; b %= 65521u;
    for (i = 0; i < n; ) {
        size_t m = n - i < 5552 ? n - i : 5552;
        for (k = 0; k < m; k++) { uint8_t c = p[i + k]; crc = tab[(crc ^ c) & 0xFF] ^ (crc >> 8); a += c; b += a; }
        a %= 65521u; b %= 65521u; i += m;
    }
    return ((uint64_t)(crc ^ 0xFFFFFFFFu) << 32) | ((b << 16) | a);
}
static int px_hash_for_use(int enabled,int scale,int installed,int capture,
                          uint32_t w,uint32_t h,const uint32_t *pixels,uint64_t *result)
{
    *result=0;
    if(!capture && (!enabled || scale<2 || !installed))return 0;
    *result=px_hash(w,h,pixels);return 1;
}
#endif
