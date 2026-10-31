/* xbox_host.h -- PROJECT code. Guest address -> host address, for every build.
 *
 * 64-bit builds hold the guest's whole 4 GB address space in the process, so the host address of a guest address is
 * the address plus one constant (g_xbox_mem_offset). A 32-bit process (the Windows XP / 32-bit Windows build,
 * NHL_X86) has 2 GB at most, and the guest map does not fit: RAM and 28 mirrors of it, the 64 MB contiguous window at
 * 0x80000000, the tiled alias at 0xF0000000, and the NV2A / MCPX / flash apertures at the top. There each 16 MB piece
 * of guest space (the top byte of the address) has its own offset: g_xbox_off_tbl[va >> 24] is added to the address.
 * Offsets wrap modulo 2^32 on purpose. Pieces that alias (RAM mirrors, the tiled alias of the contiguous window) simply
 * get offsets that land on the same host bytes.
 *
 * XBOX_HOST(va) is the host address of guest address va in either build. Host code that does `base + va` on a guest
 * address must use it (or xbox_host()) so it also works for the contiguous window and the apertures. Code that
 * reads a whole block (a texture, a push buffer) has to stay inside one 16 MB piece, which GPU resources do
 * (they live in the contiguous window, which is one host mapping): xbox_host_span_ok() says so. */
#ifndef NHL_XBOX_HOST_H
#define NHL_XBOX_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef NHL_X86
extern uintptr_t g_xbox_off_tbl[256];
#define XBOX_HOST(va) ((uintptr_t)(uint32_t)(va) + g_xbox_off_tbl[(uint32_t)(va) >> 24])
/* host fault address -> guest address; 0 when the host address belongs to no guest window */
int xbox_host_to_va(uintptr_t host, uint32_t *va);
#else
extern ptrdiff_t g_xbox_mem_offset;
#define XBOX_HOST(va) ((uintptr_t)(uint32_t)(va) + (uintptr_t)g_xbox_mem_offset)
static inline int xbox_host_to_va(uintptr_t host, uint32_t *va)
{
    *va = (uint32_t)(host - (uintptr_t)g_xbox_mem_offset);
    return 1;
}
#endif

static inline void *xbox_host(uint32_t va) { return (void *)XBOX_HOST(va); }

/* a block of n bytes at guest address va is contiguous in host memory (does not cross into another 16 MB piece
 * whose offset differs) */
static inline int xbox_host_span_ok(uint32_t va, size_t n)
{
    return n == 0 || XBOX_HOST(va) + (n - 1) == XBOX_HOST(va + (uint32_t)(n - 1));
}

#endif
