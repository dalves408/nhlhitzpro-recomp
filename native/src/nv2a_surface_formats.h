#ifndef HITZ_NV2A_SURFACE_FORMATS_H
#define HITZ_NV2A_SURFACE_FORMATS_H
#include <stdint.h>
#include <dxgiformat.h>

/* NV097_SET_SURFACE_FORMAT_COLOR. Keep the storage format through rendering,
 * resolves and texture sampling instead of silently dropping 16-bit draws. */
static DXGI_FORMAT hitz_rt_dxgi_format(uint32_t color)
{
    switch (color & 15u) {
    case 1: case 2: return DXGI_FORMAT_B5G5R5A1_UNORM;
    case 3: return DXGI_FORMAT_B5G6R5_UNORM;
    case 4: case 5: case 6: case 7: case 8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

static uint32_t hitz_rt_bgra16(uint16_t v, DXGI_FORMAT format)
{
    uint32_t b = v & 31u, g, r, a;
    b = (b << 3) | (b >> 2);
    if (format == DXGI_FORMAT_B5G6R5_UNORM) {
        g = (v >> 5) & 63u; g = (g << 2) | (g >> 4);
        r = (v >> 11) & 31u; a = 255;
    } else {
        g = (v >> 5) & 31u; g = (g << 3) | (g >> 2);
        r = (v >> 10) & 31u; a = (v & 0x8000u) ? 255 : 0;
    }
    r = (r << 3) | (r >> 2);
    return (a << 24) | (r << 16) | (g << 8) | b;
}
#endif
