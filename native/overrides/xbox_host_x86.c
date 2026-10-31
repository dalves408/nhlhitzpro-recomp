/* xbox_host_x86.c -- PROJECT code, 32-bit build only (NHL_X86). See xbox_host.h.
 *
 * The guest's 4 GB address space is not held in the process: every 16 MB piece of it (the top byte of an address)
 * has one host offset in g_xbox_off_tbl, and the generated code and the host code add it to the address.
 *
 *   0x00000000..0x7FFFFFFF  RAM and its mirrors: piece b is piece (b mod RAM/16 MB) of the one RAM view
 *   0x80000000..0x83FFFFFF  the contiguous window (its own storage, a second file mapping)
 *   0xF0000000..0xF3FFFFFF  the tiled alias of the contiguous window (same host bytes, see xbox_x86_alias)
 *   0xFD000000              NV2A registers, 0xFE000000 MCPX (APU, AC97, USB, NIC), 0xFF000000 flash: one reserved
 *                           16 MB piece each, with only the part the layout code commits made accessible
 *   everything else         a shared no-access guard region: a stray access faults (the faulting host address then
 *                           maps back to no guest window, which the crash report says) */
#ifdef NHL_X86

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include "xbox_host.h"

#define PIECE (16u * 1024u * 1024u)

uintptr_t g_xbox_off_tbl[256];

static uintptr_t host_of_piece(unsigned b)
{
    return (uintptr_t)(b << 24) + g_xbox_off_tbl[b];
}

static void set_piece(unsigned b, uintptr_t host_start)
{
    g_xbox_off_tbl[b] = host_start - (uintptr_t)(b << 24);
}

/* Seed the table: everything faults, then RAM and its mirrors. Called once the RAM view exists. */
void xbox_x86_init(void *ram, size_t ram_size)
{
    unsigned b, n = (unsigned)(ram_size / PIECE);
    uint8_t *guard = (uint8_t *)VirtualAlloc(NULL, PIECE, MEM_RESERVE, PAGE_NOACCESS);

    if (!guard)
        fprintf(stderr, "xbox_x86_init: no address space for the guard piece (error %lu)\n", GetLastError());
    for (b = 0; b < 256; b++)
        set_piece(b, (uintptr_t)guard);
    if (n == 0) n = 1;
    for (b = 0; b < 0x80; b++)
        set_piece(b, (uintptr_t)ram + (uintptr_t)(b % n) * PIECE);
    fprintf(stderr, "  32-bit guest address translation: RAM %u MB at host %p, mirrored up to 0x80000000\n",
            (unsigned)(ram_size >> 20), ram);
}

/* Windows already placed a view of `size` bytes at `host`: make it guest address guest_base (16 MB aligned, size a
 * multiple of 16 MB). */
void xbox_x86_map(uint32_t guest_base, size_t size, void *host)
{
    unsigned b0 = guest_base >> 24, i, n = (unsigned)(size / PIECE);
    for (i = 0; i < n; i++)
        set_piece(b0 + i, (uintptr_t)host + (uintptr_t)i * PIECE);
}

/* guest_dst..+size shows the same bytes as guest_src..+size (both 16 MB aligned). */
void xbox_x86_alias(uint32_t guest_dst, size_t size, uint32_t guest_src)
{
    unsigned bd = guest_dst >> 24, bs = guest_src >> 24, i, n = (unsigned)((size + PIECE - 1) / PIECE);
    for (i = 0; i < n; i++)
        set_piece(bd + i, host_of_piece(bs + i));
}

/* A device aperture: reserve whole 16 MB pieces around [guest_base, guest_base + size), commit just that range
 * (zeroed, read-write) and return the host address of guest_base. NULL on failure. */
void *xbox_x86_commit_region(uint32_t guest_base, size_t size)
{
    unsigned b0 = guest_base >> 24, b1 = (unsigned)(((uint64_t)guest_base + size - 1) >> 24), i, n = b1 - b0 + 1;
    uint8_t *host = (uint8_t *)VirtualAlloc(NULL, (size_t)n * PIECE, MEM_RESERVE, PAGE_NOACCESS), *p;

    if (!host) return NULL;
    p = host + (guest_base - (b0 << 24));
    if (!VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(host, 0, MEM_RELEASE);
        return NULL;
    }
    for (i = 0; i < n; i++)
        set_piece(b0 + i, (uintptr_t)host + (uintptr_t)i * PIECE);
    return p;
}

int xbox_host_to_va(uintptr_t host, uint32_t *va)
{
    unsigned b;
    for (b = 0; b < 256; b++) {
        uintptr_t start = host_of_piece(b);
        if (host - start < PIECE && start != host_of_piece(0xFC) /* 0xFC is never mapped: its piece is the guard */) {
            *va = (uint32_t)(b << 24) + (uint32_t)(host - start);
            return 1;
        }
    }
    return 0;
}

#endif
