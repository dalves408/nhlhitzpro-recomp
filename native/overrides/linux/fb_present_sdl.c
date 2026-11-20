/*
 * Framebuffer-window stubs for the SDL3 Linux player (built with -DHITZ_SDL3=ON).
 *
 * The toolkit's tools/xboxrecomp/src/video/fb_present_sdl.c is a software
 * window on SDL2's renderer API. The Linux player never starts it: main_linux.c
 * runs the guest itself and backend_gl.c owns the OpenGL window, so the
 * toolkit's window stayed idle (s_running was never set and Present returned at
 * once). This file keeps the entry points the kernel and pushbuffer code call,
 * with the same behaviour, and has no SDL dependency.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "video_player.h"

static uint32_t s_fb_va_unused, s_fb_pitch_unused;
static uint64_t s_flips;
static uint32_t s_frame_draws;
static char     s_title[128] = "Xbox Recomp";

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    s_fb_va_unused = fb_va;
    if (pitch)
        s_fb_pitch_unused = pitch;
}

void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    char buf[sizeof s_title];
    size_t o = 0;
    int i;

    /* UTF-16 title to UTF-8; BMP only, a lone surrogate becomes '?'. */
    for (i = 0; i < max_chars && name[i]; i++) {
        uint32_t c = name[i];
        if (c >= 0xD800 && c <= 0xDFFF)
            c = '?';
        if (c < 0x80) {
            if (o + 1 >= sizeof buf) break;
            buf[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= sizeof buf) break;
            buf[o++] = (char)(0xC0 | (c >> 6));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= sizeof buf) break;
            buf[o++] = (char)(0xE0 | (c >> 12));
            buf[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    if (o) {
        buf[o] = 0;
        memcpy(s_title, buf, o + 1);
    }
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    __atomic_add_fetch(&s_flips, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s_frame_draws, draws, __ATOMIC_RELAXED);
}

/* No toolkit window: run the guest on the calling thread. */
int xbox_HostWindowMain(int (*guest_main)(void))
{
    return guest_main();
}
