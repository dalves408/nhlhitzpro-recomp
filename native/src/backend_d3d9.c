/*
 * Direct3D 9 render back end for the NV2A executor (nv2a_backend.h): the Windows 7 build.
 *
 * Project code, not toolkit. Same job as backend_d3d11.c, cut down to 1x native: no render scale,
 * anti-aliasing, anisotropic filtering, texture pack, upscaling or GPU vertex programs (vertex programs run
 * on the CPU, in the executor). Shader Model 3.0 has no bit operations, so the register-combiner pixel shader
 * is generated per combiner program as plain float HLSL (gen_rc_ps) instead of decoding the combiner words
 * in the shader. The same entry point name (backend_d3d11_register) keeps main.c shared.
 *
 * Everything runs on the graphics thread (or the NV2A poll thread without it): the window and the device are
 * created lazily on the first callback and window messages are pumped in flip().
 */
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3dcommon.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"
#include "nv2a_pbx.h"        /* project: combiner state for batches that have it */
#include "xbox_host.h"          /* guest -> host address (the 32-bit build translates per 16 MB piece) */

#define MAX_RT 16

typedef struct {
    uint32_t va, w, h, zeta_va;
    IDirect3DTexture9 *tex;          /* the render target, also sampled as a texture */
    IDirect3DSurface9 *surf;         /* its level 0 */
    IDirect3DSurface9 *depth;        /* depth/stencil, created on first use (shared per zeta address) */
    uint32_t uses;                   /* clears + draws since the last flip */
} RenderTarget;

static struct {
    int inited, failed, lost, reset_pending, in_scene;
    HWND hwnd;
    IDirect3D9 *d9;
    IDirect3DDevice9 *dev;
    D3DPRESENT_PARAMETERS pp;
    D3DCAPS9 caps;
    RenderTarget rt[MAX_RT];
    int nrt;
    int vsync, log;
    IDirect3DVertexBuffer9 *vb, *xvb;
    IDirect3DVertexDeclaration9 *decl_s, *decl_rc;
    IDirect3DVertexShader9 *vs_s, *vs_rc;
    IDirect3DTexture9 *white;
    uint32_t vb_off, xvb_off;
    IDirect3DSurface9 *cur_rt, *cur_ds;
    uint32_t draws, tris, flips;
    DWORD t0;
} g;
static IDirect3DVertexBuffer9 *s_gvb;         /* GPU vertex programs: the decoded attribute stream (d3d9_gvs.inc) */
static uint32_t s_gvb_off;
static IDirect3DIndexBuffer9 *s_gib;          /* indices of GPU vertex-program draws */
static uint32_t s_gib_off;                    /* in indices */
static int s_vc_valid;                        /* the vertex constant registers hold the last fast-path block; any other writer clears it */

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[D3D9] ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* Graphics thread (RECOMP_GFX_THREAD, on by default): the executor only records each draw, clear, flip and
 * blit; this back end's own thread replays them. s_async says so; while a recorded draw is replayed the
 * batch's extra state is s_wx (the executor's copy is overwritten as soon as it moves on). */
static int s_async;
static volatile LONG s_dev_ready;     /* the graphics thread has made the window and device */
static const Nv2aBatchExt *s_wx;
static const Nv2aBatchExt *pbx_cur(void) { return s_async ? s_wx : (nv2a_pbx_current)(); }
#define nv2a_pbx_current() pbx_cur()

/* keyboard.h (toolkit input library): keys reach the title through these. */
extern void xbox_InputKeySet(int vk, int down);
extern void xbox_InputKeysClear(void);

/* RECOMP_KEYLOG: every key press and release in the game window, with the
 * seconds since the process started and the pad button it stands for (the
 * keyboard map in tools/xboxrecomp/src/input/keyboard.c). A person's play
 * session then becomes timings for scripted runs (RECOMP_PAD_LIVE lines). */
static const char *keylog_button(int vk)
{
    switch (vk) {
    case VK_RETURN: return "start";  case VK_BACK: return "back";
    case VK_UP: return "up";         case VK_DOWN: return "down";
    case VK_LEFT: return "left";     case VK_RIGHT: return "right";
    case 'Z': return "a"; case 'X': return "b"; case 'C': return "x"; case 'V': return "y";
    case 'Q': return "white"; case 'E': return "black";
    case '1': return "lt"; case '3': return "rt";
    case 'W': return "lup"; case 'S': return "ldown"; case 'A': return "lleft"; case 'D': return "lright";
    case 'I': return "rup"; case 'K': return "rdown"; case 'J': return "rleft"; case 'L': return "rright";
    default: return "-";
    }
}

static void keylog(int vk, int down)
{
    static int on = -1;
    FILETIME created, x1, x2, x3, now;
    if (on < 0)
        on = getenv("RECOMP_KEYLOG") != NULL;
    if (!on)
        return;
    GetProcessTimes(GetCurrentProcess(), &created, &x1, &x2, &x3);
    GetSystemTimeAsFileTime(&now);
    {
        ULARGE_INTEGER c, n;
        c.LowPart = created.dwLowDateTime; c.HighPart = created.dwHighDateTime;
        n.LowPart = now.dwLowDateTime;     n.HighPart = now.dwHighDateTime;
        fprintf(stderr, "[KEYLOG] t=%.3f %s vk=0x%02X pad=%s\n",
                (double)(n.QuadPart - c.QuadPart) / 1e7, down ? "down" : "up  ",
                vk, keylog_button(vk));
        fflush(stderr);
    }
}

static int aspect_on(void);
static int s_fs;                    /* borderless fullscreen is on */
static int f11_on(void);
static void toggle_fullscreen(HWND h);
static void user_toggle_fullscreen(HWND h);
static int want_fullscreen_at_start(void);
static void aspect_on_size(void);
static int aspect_window(void);
static void aspect_sizing(HWND h, int edge, RECT *rc);
static void aspect_minmax(HWND h, MINMAXINFO *mm);
static void aspect_poschanging(HWND h, WINDOWPOS *p);

/* ---- ESC: "Do you want to quit?" overlay ----------------------------------
 * A layered, non-activating popup owned by the game window, sized to its client
 * area: the game keeps drawing underneath, dimmed. While it is up the keyboard
 * goes to the overlay, not the game. ESC opens it (and closes it again); Left /
 * Right / Tab choose, Enter or Space confirms, Y quits, N closes; the mouse works
 * too. The pad is not read here, so a controller still reaches the game. */
static int s_q_active, s_q_sel;            /* sel: 0 = No (default), 1 = Yes */
static HWND s_q_hwnd;
static RECT s_q_btn[2];                    /* No, Yes, in overlay client coordinates */
static int s_q_cw, s_q_ch, s_q_ox, s_q_oy; /* what was last drawn */


/* ---- F1: settings menu (the 1x subset of the D3D11 build's) ----------------------------------
 * Fullscreen, Stretch image, VSync and Frame Rate. Saved at once in %LOCALAPPDATA%\NHLHitzPro\settings.ini, [display]. */
static int s_set_active, s_set_sel, s_set_armed;
extern volatile int nhl_input_block;           /* usb_gamepad.c override: the game sees neutral pads */
static int s_pad_valid, s_unblock_wait;   /* controller 1 driving the overlay / waiting for its buttons to be released */
static DWORD s_unblock_t;
static RECT s_set_row[10];
static int s_stretch;             /* F1 "Stretch image": fill the window instead of the 4:3 shape */
static int s_fpsi;                /* F1 "Frame Rate": index into SET_FPS_HZ */
static const unsigned SET_FPS_HZ[6] = { 30, 60, 120, 144, 240, 360 };
extern void xbox_set_vblank_hz(unsigned hz);   /* kernel_bridge.c override: the display rate the game sees */
static int quit_key(WPARAM w, LPARAM l, int down);

#define SET_ITEMS 6
enum { IT_FS, IT_STRETCH, IT_VSYNC, IT_FPS, IT_CLOSE, IT_QUIT, IT_HIRES = 99 };

static int cfg_path(char *out, size_t n)
{
    char base[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base) || strlen(base) > MAX_PATH - 40)
        return 0;
    snprintf(out, n, "%s\\NHLHitzPro", base);
    CreateDirectoryA(out, NULL);
    snprintf(out, n, "%s\\NHLHitzPro\\settings.ini", base);
    return 1;
}

static int cfg_get(const char *key, int def)
{
    char path[MAX_PATH];
    return cfg_path(path, sizeof path) ? (int)GetPrivateProfileIntA("display", key, def, path) : def;
}

static void cfg_put(const char *key, int v)
{
    char path[MAX_PATH], val[16];
    if (!cfg_path(path, sizeof path)) return;
    snprintf(val, sizeof val, "%d", v);
    WritePrivateProfileStringA("display", key, val, path);
}

static int cfg_get_s(const char *sec, const char *key, int def)
{
    char path[MAX_PATH];
    return cfg_path(path, sizeof path) ? (int)GetPrivateProfileIntA(sec, key, def, path) : def;
}

static void cfg_put_s(const char *sec, const char *key, int v)
{
    char path[MAX_PATH], val[16];
    if (!cfg_path(path, sizeof path)) return;
    snprintf(val, sizeof val, "%d", v);
    WritePrivateProfileStringA(sec, key, val, path);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void settings_load(void)
{
    int hz = cfg_get("frame_rate", 60), i, best = 0, bd = 1 << 30;
    s_stretch = cfg_get("stretch", 0) != 0;
    for (i = 0; i < 6; i++)
        if (abs((int)SET_FPS_HZ[i] - hz) < bd) { bd = abs((int)SET_FPS_HZ[i] - hz); best = i; }
    s_fpsi = best;
    if (SET_FPS_HZ[best] != 60) xbox_set_vblank_hz(SET_FPS_HZ[best]);
    if (!getenv("RECOMP_D3D11_VSYNC"))
        g.vsync = cfg_get("vsync", 0) != 0;
}

static const char *const SET_LABEL[SET_ITEMS] = { "Fullscreen", "Stretch image", "VSync", "Frame Rate", "Close menu", "Quit game" };
static const char *const SET_OFFON[2] = { "Off", "On" };
static const char *const SET_STRETCH[2] = { "Off (4:3)", "On (fills window)" };
static const char *const SET_FPS[6] = { "30", "60 (original)", "120", "144", "240", "Uncapped (up to 360)" };

static int set_disabled(int i) { (void)i; return 0; }

static int set_count(int i)
{
    switch (i) {
    case IT_FPS: return 6;
    case IT_FS: case IT_STRETCH: case IT_VSYNC: return 2;
    }
    return 0;
}

static int s_fs;                    /* borderless fullscreen is on (defined with toggle_fullscreen) */

static int set_get(int i)
{
    switch (i) {
    case IT_FS: return s_fs ? 1 : 0;
    case IT_STRETCH: return s_stretch ? 1 : 0;
    case IT_VSYNC: return g.vsync ? 1 : 0;
    case IT_FPS: return s_fpsi;
    }
    return 0;
}

static const char *set_text(int i)
{
    int v = set_get(i);
    switch (i) {
    case IT_STRETCH: return SET_STRETCH[v];
    case IT_FPS: return SET_FPS[v];
    case IT_FS: case IT_VSYNC: return SET_OFFON[v];
    }
    return "";
}

static void aspect_on_size(void);

static void set_put(int i, int v)
{
    switch (i) {
    case IT_FS: user_toggle_fullscreen(g.hwnd); break;
    case IT_STRETCH:
        s_stretch = v; cfg_put("stretch", v);
        if (!s_stretch && !s_fs) aspect_on_size();      /* back to the 4:3 window shape */
        break;
    case IT_VSYNC: g.vsync = v; cfg_put("vsync", v); g.reset_pending = 1; break;   /* the present interval is a device setting */
    case IT_FPS: s_fpsi = v; cfg_put("frame_rate", (int)SET_FPS_HZ[v]); xbox_set_vblank_hz(SET_FPS_HZ[v]); break;
    }
}

static void set_draw(void)
{
    RECT cr, rr;
    POINT o = { 0, 0 };
    int cw, ch, dw, dh, dx, dy, rh, i, x, y;
    BITMAPINFO bi;
    void *bits = NULL;
    HDC sdc, mdc;
    HBITMAP bm;
    HGDIOBJ oldbm, oldf;
    HFONT ft, fr;
    uint32_t *px;
    BLENDFUNCTION bf;
    POINT p0 = { 0, 0 };
    SIZE sz;
    HBRUSH br;

    if (!s_q_hwnd || !g.hwnd || IsIconic(g.hwnd)) return;
    GetClientRect(g.hwnd, &cr);
    ClientToScreen(g.hwnd, &o);
    cw = cr.right; ch = cr.bottom;
    if (cw <= 0 || ch <= 0) return;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = cw; bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    sdc = GetDC(NULL);
    mdc = CreateCompatibleDC(sdc);
    bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits) { if (bm) DeleteObject(bm); DeleteDC(mdc); ReleaseDC(NULL, sdc); return; }
    oldbm = SelectObject(mdc, bm);
    px = (uint32_t *)bits;
    for (i = 0; i < cw * ch; i++) px[i] = 0x96000000u;

    rh = ch * 7 / 100; if (rh < 22) rh = 22;
    dw = rh * 17;
    if (dw > cw * 95 / 100) { dw = cw * 95 / 100; rh = dw / 17; }
    dh = rh * 133 / 10;
    dx = (cw - dw) / 2; dy = (ch - dh) / 2;
    SetRect(&rr, dx, dy, dx + dw, dy + dh);
    br = CreateSolidBrush(RGB(255, 122, 0)); FillRect(mdc, &rr, br); DeleteObject(br);
    InflateRect(&rr, -(dh / 40 + 2), -(dh / 40 + 2));          /* the quit prompt's frame */
    br = CreateSolidBrush(RGB(14, 22, 40)); FillRect(mdc, &rr, br); DeleteObject(br);

    SetBkMode(mdc, TRANSPARENT);
    ft = CreateFontA(-(rh * 80 / 100), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Impact");
    fr = CreateFontA(-(rh * 56 / 100), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Bahnschrift SemiCondensed");
    oldf = SelectObject(mdc, ft);
    SetTextColor(mdc, RGB(255, 255, 255));
    SetRect(&rr, dx, dy + rh / 4, dx + dw, dy + rh * 13 / 10);
    DrawTextA(mdc, "SETTINGS", -1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mdc, fr);
    for (i = 0; i < SET_ITEMS; i++) {
        int sel = (i == s_set_sel);
        char buf[96];
        y = dy + rh * 14 / 10 + i * rh;
        x = dx + rh / 3;
        SetRect(&s_set_row[i], x, y, dx + dw - rh / 3, y + rh * 82 / 100);
        br = CreateSolidBrush(sel ? RGB(255, 122, 0) : set_disabled(i) ? RGB(28, 34, 46) : RGB(52, 60, 78));   /* the quit prompt's buttons */
        FillRect(mdc, &s_set_row[i], br); DeleteObject(br);
        SetTextColor(mdc, sel ? RGB(10, 10, 14) : set_disabled(i) ? RGB(92, 100, 114) : RGB(200, 210, 225));
        rr = s_set_row[i]; rr.left += rh / 3;
        DrawTextA(mdc, SET_LABEL[i], -1, &rr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rr = s_set_row[i]; rr.right -= rh / 3;
        if (set_count(i))
            snprintf(buf, sizeof buf, "<  %s  >", set_text(i));
        else if (i == IT_QUIT && s_set_armed)
            snprintf(buf, sizeof buf, "press again to quit");
        else if (i == IT_HIRES)
            snprintf(buf, sizeof buf, "%s", set_text(i));
        else
            buf[0] = 0;
        DrawTextA(mdc, buf, -1, &rr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    SetTextColor(mdc, RGB(150, 162, 182));
    SetRect(&rr, dx + rh / 3, dy + rh * 14 / 10 + SET_ITEMS * rh + rh / 6, dx + dw - rh / 3, dy + dh - rh / 8);
    {
        HFONT fh = CreateFontA(-(rh * 38 / 100), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Bahnschrift SemiCondensed");
        HGDIOBJ prev = SelectObject(mdc, fh);
        DrawTextA(mdc, "Up/Down choose, Left/Right change, Enter or A confirms. Saved automatically. F1, Esc or B closes.", -1, &rr, DT_LEFT | DT_WORDBREAK);
        SelectObject(mdc, prev);
        DeleteObject(fh);
    }
    SelectObject(mdc, oldf);
    DeleteObject(ft); DeleteObject(fr);
    GdiFlush();
    for (y = dy; y < dy + dh; y++)
        for (x = dx; x < dx + dw; x++)
            px[(size_t)y * cw + x] |= 0xFF000000u;

    bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
    sz.cx = cw; sz.cy = ch;
    UpdateLayeredWindow(s_q_hwnd, sdc, &o, &sz, mdc, &p0, 0, &bf, ULW_ALPHA);
    s_q_cw = cw; s_q_ch = ch; s_q_ox = o.x; s_q_oy = o.y;
    SelectObject(mdc, oldbm);
    DeleteObject(bm); DeleteDC(mdc); ReleaseDC(NULL, sdc);
}

static void quit_close(void);

/* Left/Right/Enter on line i. */
static void set_change(int i, int dir)
{
    int n = set_count(i);
    if (n) {
        int v = (set_get(i) + dir + n) % n;
        if (i == IT_FS) v = !s_fs;
        set_put(i, v);
        s_set_armed = 0;
    } else if (dir == 0) {
        if (i == IT_CLOSE) quit_close();
        else if (i == IT_QUIT) {
            if (s_set_armed) TerminateProcess(GetCurrentProcess(), 0);
            s_set_armed = 1;
        }
    }
    set_draw();
}

static int set_key(WPARAM w, LPARAM l, int down)
{
    int repeat = (l & (1L << 30)) != 0;
    if (!down) return 1;
    if (w == VK_ESCAPE || w == VK_F1) { if (!repeat) quit_close(); return 1; }
    if (w == VK_UP || w == VK_DOWN) {
        do s_set_sel = (s_set_sel + (w == VK_UP ? SET_ITEMS - 1 : 1)) % SET_ITEMS;
        while (set_disabled(s_set_sel));
        s_set_armed = 0;
        set_draw();
    } else if (w == VK_LEFT) set_change(s_set_sel, -1);
    else if (w == VK_RIGHT) set_change(s_set_sel, +1);
    else if ((w == VK_RETURN || w == VK_SPACE) && !repeat)
        set_change(s_set_sel, set_count(s_set_sel) ? +1 : 0);
    return 1;
}


static void quit_draw(void);
static void quit_close(void);
extern void xbox_time_pause_begin(void);   /* kernel_hal.c override */
extern void xbox_time_pause_end(void);
extern void xa2_set_muted(int muted);      /* apu_xaudio2.c override */

static LRESULT CALLBACK quit_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* The overlays are driven by the keyboard and controller 1 only: the window takes no mouse. */
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcA(h, m, w, l);
}

static void quit_draw(void)
{
    RECT cr;
    if (s_set_active) { set_draw(); return; }
    POINT o = { 0, 0 };
    int cw, ch, dw, dh, dx, dy, bw, bh, by, x, y, i;
    BITMAPINFO bi;
    void *bits = NULL;
    HDC sdc, mdc;
    HBITMAP bm;
    HGDIOBJ oldbm, oldf;
    HFONT ft, fb;
    uint32_t *px;
    BLENDFUNCTION bf;
    POINT p0 = { 0, 0 };
    SIZE sz;
    RECT dr;
    HBRUSH br;

    if (!s_q_hwnd || !g.hwnd || IsIconic(g.hwnd)) return;
    GetClientRect(g.hwnd, &cr);
    ClientToScreen(g.hwnd, &o);
    cw = cr.right; ch = cr.bottom;
    if (cw <= 0 || ch <= 0) return;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = cw; bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    sdc = GetDC(NULL);
    mdc = CreateCompatibleDC(sdc);
    bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits) { if (bm) DeleteObject(bm); DeleteDC(mdc); ReleaseDC(NULL, sdc); return; }
    oldbm = SelectObject(mdc, bm);
    px = (uint32_t *)bits;
    for (i = 0; i < cw * ch; i++) px[i] = 0x96000000u;          /* dim: black, alpha 150 */

    dw = ch * 4 / 5; if (dw > cw * 9 / 10) dw = cw * 9 / 10;
    dh = dw * 9 / 20;
    dx = (cw - dw) / 2; dy = (ch - dh) / 2;
    SetRect(&dr, dx, dy, dx + dw, dy + dh);
    br = CreateSolidBrush(RGB(255, 122, 0)); FillRect(mdc, &dr, br); DeleteObject(br);
    InflateRect(&dr, -dh / 40 - 2, -dh / 40 - 2);
    br = CreateSolidBrush(RGB(14, 22, 40)); FillRect(mdc, &dr, br); DeleteObject(br);

    bw = dw * 3 / 10; bh = dh * 28 / 100; by = dy + dh * 58 / 100;
    SetRect(&s_q_btn[0], dx + dw / 2 - bw - dw / 25, by, dx + dw / 2 - dw / 25, by + bh);
    SetRect(&s_q_btn[1], dx + dw / 2 + dw / 25, by, dx + dw / 2 + dw / 25 + bw, by + bh);
    SetBkMode(mdc, TRANSPARENT);
    ft = CreateFontA(-(dh * 17 / 100), 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Bahnschrift SemiCondensed");
    fb = CreateFontA(-(dh * 15 / 100), 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Bahnschrift SemiCondensed");
    oldf = SelectObject(mdc, ft);
    SetTextColor(mdc, RGB(255, 255, 255));
    SetRect(&dr, dx, dy + dh * 12 / 100, dx + dw, dy + dh * 50 / 100);
    DrawTextA(mdc, "Do you want to quit?", -1, &dr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mdc, fb);
    for (i = 0; i < 2; i++) {
        int on = (s_q_sel == i);
        br = CreateSolidBrush(on ? RGB(255, 122, 0) : RGB(52, 60, 78));
        FillRect(mdc, &s_q_btn[i], br); DeleteObject(br);
        SetTextColor(mdc, on ? RGB(10, 10, 14) : RGB(200, 210, 225));
        DrawTextA(mdc, i ? "YES" : "NO", -1, &s_q_btn[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(mdc, oldf);
    DeleteObject(ft); DeleteObject(fb);
    GdiFlush();
    for (y = dy; y < dy + dh; y++)                                /* the dialog itself is opaque */
        for (x = dx; x < dx + dw; x++)
            px[(size_t)y * cw + x] |= 0xFF000000u;

    bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
    sz.cx = cw; sz.cy = ch;
    UpdateLayeredWindow(s_q_hwnd, sdc, &o, &sz, mdc, &p0, 0, &bf, ULW_ALPHA);
    s_q_cw = cw; s_q_ch = ch; s_q_ox = o.x; s_q_oy = o.y;
    SelectObject(mdc, oldbm);
    DeleteObject(bm); DeleteDC(mdc); ReleaseDC(NULL, sdc);
}

static void quit_open(void)
{
    if (!s_q_hwnd) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = quit_wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "NhlQuitOverlay";
        RegisterClassA(&wc);
        s_q_hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT, wc.lpszClassName, "",
                                   WS_POPUP, 0, 0, 1, 1, g.hwnd, NULL, wc.hInstance, NULL);
        if (!s_q_hwnd) return;
    }
    xbox_InputKeysClear();            /* nothing the game saw as held stays held */
    nhl_input_block = 1; s_unblock_wait = 0; s_pad_valid = 0;     /* the game sees neutral pads while this is up */
    s_q_sel = 0; s_q_active = 1;
    xbox_time_pause_begin();          /* the game's clocks stop; present() holds below */
    xa2_set_muted(1);
    quit_draw();
    ShowWindow(s_q_hwnd, SW_SHOWNOACTIVATE);
}

static void quit_close(void)
{
    if (s_q_active) {
        xa2_set_muted(0); xbox_time_pause_end();
        s_unblock_wait = 1; s_unblock_t = GetTickCount();         /* neutral until the buttons are released */
    }
    s_q_active = 0;
    s_set_active = 0; s_set_armed = 0;
    if (s_q_hwnd) ShowWindow(s_q_hwnd, SW_HIDE);
}

/* Controller 1 (XInput slot 0) moves through the overlays like the keyboard: D-pad or left stick
 * to choose and change, A to confirm, B or Start to close. Other controllers do nothing. */
typedef struct { DWORD pkt; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } NhlPadState;
extern DWORD WINAPI XInputGetState(DWORD user, void *state);
static WORD s_pad_prev;
static DWORD s_pad_rep;

static void pad_overlay_poll(void)
{
    NhlPadState st;
    WORD b, press, dirs;
    DWORD now = GetTickCount();
    if (!s_q_active) return;
    if (XInputGetState(0, &st) != 0) { s_pad_prev = 0; s_pad_valid = 1; return; }
    b = st.buttons;
    if (st.lx < -16000) b |= 0x0004;
    if (st.lx > 16000)  b |= 0x0008;
    if (st.ly > 16000)  b |= 0x0001;
    if (st.ly < -16000) b |= 0x0002;
    if (!s_pad_valid) { s_pad_prev = b; s_pad_valid = 1; return; }   /* what was held when it opened is not a press */
    press = (WORD)(b & ~s_pad_prev);
    dirs = (WORD)(b & 0x000F);
    if (press & 0x1000) quit_key(VK_RETURN, 0, 1);                /* A */
    if (press & 0x2000) quit_key(VK_ESCAPE, 0, 1);                /* B */
    else if (press & 0x0010) quit_key(VK_ESCAPE, 0, 1);           /* Start */
    if (s_q_active) {
        WORD fire = 0;
        if (press & 0x000F) { fire = (WORD)(press & 0x000F); s_pad_rep = now + 350; }
        else if (dirs && now >= s_pad_rep) { fire = dirs; s_pad_rep = now + 110; }   /* held: repeat */
        if (fire & 0x0001) quit_key(VK_UP, 0, 1);
        else if (fire & 0x0002) quit_key(VK_DOWN, 0, 1);
        else if (fire & 0x0004) quit_key(VK_LEFT, 0, 1);
        else if (fire & 0x0008) quit_key(VK_RIGHT, 0, 1);
    }
    s_pad_prev = b;
}

/* After an overlay closes, the game keeps seeing neutral pads until the buttons are let go, so the
 * press that closed it is not also a press in the game. */
static void pad_unblock_tick(void)
{
    NhlPadState st;
    if (!s_unblock_wait) return;
    if (XInputGetState(0, &st) != 0 || (st.buttons == 0 && st.lx > -16000 && st.lx < 16000 &&
                                        st.ly > -16000 && st.ly < 16000) ||
        GetTickCount() - s_unblock_t > 2000) {
        nhl_input_block = 0;
        s_unblock_wait = 0;
    }
}

static void set_open(void)
{
    s_set_active = 1; s_set_sel = 0; s_set_armed = 0;
    quit_open();
}

/* Hold Back and press Start on controller 1: the settings menu (like F1). Called once per frame while no
 * overlay is open. The game never sees that Start (usb_gamepad.c hides Start while Back is held). */
static void pad_open_poll(void)
{
    static WORD prev;
    NhlPadState st;
    WORD b;
    if (s_q_active) return;
    if (XInputGetState(0, &st) != 0) { prev = 0; return; }
    b = st.buttons;
    if ((b & 0x0020) && (b & 0x0010) && !(prev & 0x0010)) {
        prev = b;
        set_open();
        return;
    }
    prev = b;
}

/* Per frame: follow the game window when it moves or is resized. */
static void quit_tick(void)
{
    RECT cr;
    POINT o = { 0, 0 };
    if (!s_q_active || !g.hwnd) return;
    if (IsIconic(g.hwnd)) return;
    if (s_q_hwnd && !IsWindowVisible(s_q_hwnd)) {      /* hidden behind our back: show it again, on top, redrawn */
        ShowWindow(s_q_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(s_q_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        s_q_cw = 0;                                    /* forces the redraw below */
    }
    GetClientRect(g.hwnd, &cr);
    ClientToScreen(g.hwnd, &o);
    if (cr.right != s_q_cw || cr.bottom != s_q_ch || o.x != s_q_ox || o.y != s_q_oy) quit_draw();
}

/* A key message for the game window; 1 when the overlay took it. */
static int quit_key(WPARAM w, LPARAM l, int down)
{
    int repeat = (l & (1L << 30)) != 0;
    if (s_q_active && s_set_active) return set_key(w, l, down);
    if (w == VK_ESCAPE) {
        if (down && !repeat) { if (s_q_active) quit_close(); else quit_open(); }
        return 1;
    }
    if (!s_q_active) return 0;
    if (down && (w == VK_LEFT || w == VK_RIGHT || w == VK_TAB || w == 'A' || w == 'D')) {
        s_q_sel = (w == VK_LEFT || w == 'A') ? 0 : (w == VK_RIGHT || w == 'D') ? 1 : !s_q_sel;
        quit_draw();
    } else if (down && !repeat && (w == VK_RETURN || w == VK_SPACE)) {
        if (s_q_sel) TerminateProcess(GetCurrentProcess(), 0);
        quit_close();
    } else if (down && !repeat && w == 'Y') {
        TerminateProcess(GetCurrentProcess(), 0);
    } else if (down && !repeat && w == 'N') {
        quit_close();
    }
    return 1;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SIZE:
        if (aspect_on() && w != SIZE_MINIMIZED) aspect_on_size();
        break;
    case WM_SIZING:                 /* RECOMP_ASPECT_4_3, windowed: the dragged frame stays 4:3 */
        if (aspect_window()) { aspect_sizing(h, (int)w, (RECT *)l); return TRUE; }
        break;
    case WM_GETMINMAXINFO:          /* ... and so does the maximised window */
        if (aspect_window()) { aspect_minmax(h, (MINMAXINFO *)l); return 0; }
        break;
    case WM_WINDOWPOSCHANGING:      /* ... and any other resize (SetWindowPos, snapping) */
        if (aspect_window()) aspect_poschanging(h, (WINDOWPOS *)l);
        break;
    case WM_CLOSE:
    case WM_DESTROY:
        /* The guest threads keep running; ExitProcess can wait on them. */
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (w == VK_F11 && f11_on()) {  /* RECOMP_F11_FULLSCREEN: ours, never the game's */
            if (!(l & (1L << 30))) user_toggle_fullscreen(h);
            return 0;
        }
        if (w == VK_RETURN && (l & (1L << 29)) && f11_on()) {   /* Alt+Enter, like F11 */
            if (!(l & (1L << 30))) user_toggle_fullscreen(h);
            return 0;
        }
        if (w == VK_F1) {                  /* settings menu */
            if (!(l & (1L << 30))) { if (!s_q_active) set_open(); else if (s_set_active) quit_close(); }
            return 0;
        }
        if (quit_key(w, l, 1)) return 0;   /* ESC quit overlay */
        if (!(l & (1L << 30)))          /* not an auto-repeat */
            keylog((int)w, 1);
        xbox_InputKeySet((int)w, 1);
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (w == VK_F11 && f11_on()) return 0;
        if (w == VK_RETURN && m == WM_SYSKEYUP && f11_on()) return 0;   /* the Alt+Enter release */
        if (w == VK_F1) return 0;
        if (quit_key(w, l, 0)) return 0;
        keylog((int)w, 0);
        xbox_InputKeySet((int)w, 0);
        return 0;
    case WM_SETCURSOR:              /* borderless fullscreen: no mouse pointer over the game */
        if (s_fs && LOWORD(l) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

static int aspect_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_ASPECT_4_3"); on = e && e[0] == '1'; }
    return on;
}

/* RECOMP_F11_FULLSCREEN=1: F11 toggles borderless fullscreen on the window's monitor (a WS_POPUP
 * window covering it; no DXGI exclusive mode) and back to the saved style and placement. F11 is
 * not passed to the game's keyboard mapping. */
static int f11_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_F11_FULLSCREEN"); on = e && e[0] == '1'; }
    return on;
}

static int s_fs;
static LONG s_fs_style;
static WINDOWPLACEMENT s_fs_wp;

static void toggle_fullscreen(HWND h)
{
    if (!s_fs) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        memset(&s_fs_wp, 0, sizeof s_fs_wp);
        s_fs_wp.length = sizeof s_fs_wp;
        if (!GetWindowPlacement(h, &s_fs_wp) ||
            !GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        s_fs_style = GetWindowLongA(h, GWL_STYLE);
        s_fs = 1;
        SetWindowLongA(h, GWL_STYLE, (s_fs_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        logf_("F11: borderless fullscreen %ldx%ld\n", (long)(mi.rcMonitor.right - mi.rcMonitor.left),
              (long)(mi.rcMonitor.bottom - mi.rcMonitor.top));
    } else {
        s_fs = 0;
        SetWindowLongA(h, GWL_STYLE, s_fs_style);
        SetWindowPlacement(h, &s_fs_wp);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        logf_("F11: windowed\n");
    }
}

/* The player build starts fullscreen and remembers the last mode in
 * %LOCALAPPDATA%\NHLHitzPro\settings.ini ("fullscreen=1" or "fullscreen=0"). */
#ifdef NHL_RELEASE
static int settings_path(char *out, size_t n)
{
    char base[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base) || strlen(base) > MAX_PATH - 40)
        return 0;
    snprintf(out, n, "%s\\NHLHitzPro", base);
    CreateDirectoryA(out, NULL);
    snprintf(out, n, "%s\\NHLHitzPro\\settings.ini", base);
    return 1;
}

static int want_fullscreen_at_start(void)
{
    char path[MAX_PATH];
    if (!settings_path(path, sizeof path)) return 1;
    return GetPrivateProfileIntA("display", "fullscreen", 1, path) != 0;
}

static void save_fullscreen_setting(int fs)
{
    char path[MAX_PATH];
    if (settings_path(path, sizeof path))
        WritePrivateProfileStringA("display", "fullscreen", fs ? "1" : "0", path);
}
#else
static int want_fullscreen_at_start(void) { return 0; }
static void save_fullscreen_setting(int fs) { (void)fs; }
#endif

/* F11 or Alt+Enter: switch, and remember the choice. */
static void user_toggle_fullscreen(HWND h)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 400) return;          /* a switch still settling: ignore the extra press */
    last = now;
    toggle_fullscreen(h);
    save_fullscreen_setting(s_fs);
    if (s_q_active && s_q_hwnd) {          /* an open overlay is shown again over the new window */
        ShowWindow(s_q_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(s_q_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        s_q_cw = 0;
        quit_draw();
    }
}

/* RECOMP_ASPECT_4_3, windowed: the window itself keeps a 4:3 client area, so there are no
 * bars; they appear only in F11 fullscreen, where the monitor's shape is fixed. */
static int aspect_window(void)
{
    return aspect_on() && !s_fs && !s_stretch;
}

/* Width and height the frame (borders, title bar) adds to the client area. */
static void aspect_frame(HWND h, int *fw, int *fh, RECT *ins)
{
    RECT r = { 0, 0, 0, 0 };
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongA(h, GWL_STYLE), FALSE,
                       (DWORD)GetWindowLongA(h, GWL_EXSTYLE));
    *fw = r.right - r.left;
    *fh = r.bottom - r.top;
    if (ins) *ins = r;
}

#define ASP_MIN_W 320   /* smallest client: 320x240 */

static int asp_h_of(int cw) { return (cw * 3 + 2) / 4; }
static int asp_w_of(int ch) { return (ch * 4 + 1) / 3; }

/* Keep a 4:3 client no bigger than Windows lets a window be (SM_C?MAXTRACK), so the clamp
 * Windows applies afterwards never cuts one side off. */
static void asp_cap(int fw, int fh, int *cw, int *ch)
{
    int mw = GetSystemMetrics(SM_CXMAXTRACK) - fw, mh = GetSystemMetrics(SM_CYMAXTRACK) - fh;
    if (*cw > mw) { *cw = mw; *ch = asp_h_of(*cw); }
    if (*ch > mh) { *ch = mh; *cw = asp_w_of(*ch); }
}

/* WM_SIZING: fix the dragged rectangle so its client area is 4:3. Side edges set the height
 * from the width (growing down), top/bottom edges the width from the height (growing right),
 * corners follow whichever dimension the drag asks more of; the edges opposite the one being
 * dragged stay put. */
static void aspect_sizing(HWND h, int edge, RECT *rc)
{
    int fw, fh, cw, ch, top, left;
    aspect_frame(h, &fw, &fh, NULL);
    cw = (rc->right - rc->left) - fw;
    ch = (rc->bottom - rc->top) - fh;
    if (cw < ASP_MIN_W) cw = ASP_MIN_W;
    if (ch < asp_h_of(ASP_MIN_W)) ch = asp_h_of(ASP_MIN_W);
    switch (edge) {
    case WMSZ_LEFT: case WMSZ_RIGHT:
        ch = asp_h_of(cw); break;
    case WMSZ_TOP: case WMSZ_BOTTOM:
        cw = asp_w_of(ch); break;
    default:                                        /* corners */
        if (cw * 3 >= ch * 4) ch = asp_h_of(cw); else cw = asp_w_of(ch);
        break;
    }
    asp_cap(fw, fh, &cw, &ch);                      /* within the max track size */
    top = (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT);
    left = (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT);
    if (top) rc->top = rc->bottom - (ch + fh); else rc->bottom = rc->top + ch + fh;
    if (left) rc->left = rc->right - (cw + fw); else rc->right = rc->left + cw + fw;
}

/* WM_GETMINMAXINFO: maximise to the largest 4:3 client area that fits the monitor's work
 * area under the title bar, centred (the side borders hang off-screen as usual), and keep
 * the minimum track size at a 320x240 client. */
static void aspect_minmax(HWND h, MINMAXINFO *mm)
{
    MONITORINFO mi;
    RECT ins;
    int fw, fh, ww, wh, cap, cw, ch;
    aspect_frame(h, &fw, &fh, &ins);
    mm->ptMinTrackSize.x = ASP_MIN_W + fw;
    mm->ptMinTrackSize.y = asp_h_of(ASP_MIN_W) + fh;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
    ww = mi.rcWork.right - mi.rcWork.left;
    wh = mi.rcWork.bottom - mi.rcWork.top;
    cap = -ins.top - ins.bottom;                    /* title bar (top inset minus border) */
    ch = wh - cap;
    cw = asp_w_of(ch);
    if (cw > ww) { cw = ww; ch = asp_h_of(cw); }
    if (cw < 1 || ch < 1) return;
    /* ptMaxPosition is relative to the work area's origin. */
    mm->ptMaxSize.x = cw + fw;
    mm->ptMaxSize.y = ch + fh;
    mm->ptMaxPosition.x = (ww - cw) / 2 + ins.left;
    mm->ptMaxPosition.y = -ins.bottom;
    if (mm->ptMaxTrackSize.x < mm->ptMaxSize.x) mm->ptMaxTrackSize.x = mm->ptMaxSize.x;
    if (mm->ptMaxTrackSize.y < mm->ptMaxSize.y) mm->ptMaxTrackSize.y = mm->ptMaxSize.y;
}

/* WM_WINDOWPOSCHANGING: a resize that did not come through a drag (SetWindowPos, Aero snap,
 * restore) gets the largest 4:3 client area inside the size asked for, top-left kept. */
static void aspect_poschanging(HWND h, WINDOWPOS *p)
{
    int fw, fh, cw, ch;
    if (p->flags & SWP_NOSIZE) return;
    if (IsIconic(h) || IsZoomed(h)) return;         /* minimised; maximised uses MINMAXINFO */
    if (GetWindowLongA(h, GWL_STYLE) & WS_POPUP) return;
    aspect_frame(h, &fw, &fh, NULL);
    cw = p->cx - fw;
    ch = p->cy - fh;
    if (cw <= 0 || ch <= 0) return;
    if (abs(cw * 3 - ch * 4) <= 4) return;          /* already 4:3 (to rounding) */
    if (cw * 3 > ch * 4) cw = asp_w_of(ch); else ch = asp_h_of(cw);
    asp_cap(fw, fh, &cw, &ch);
    p->cx = cw + fw;
    p->cy = ch + fh;
}

static uint64_t qpc_us(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000ull / f.QuadPart);
}

/* ~512 words spread over the span plus the first/last: far cheaper than a full hash. */
static uint64_t sample_span(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t words = n / 8, step, i;
    if (words == 0) return 0;
    step = words / 512 ? words / 512 : 1;
    for (i = 0; i < words; i += step) {
        uint64_t v;
        memcpy(&v, p + i * 8, 8);
        h = (h ^ v) * 1099511628211ull;
        h ^= h >> 29;
    }
    {
        uint64_t v;
        memcpy(&v, p + (words - 1) * 8, 8);
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

static uint64_t hash_span(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 1099511628211ull;
        h ^= h >> 29;
    }
    return h;
}

/* Bytes of guest memory a texture occupies (level 0), over-estimated for 16-bit
 * formats; 0 if the span is not guest memory. */
static size_t tex_span(const Nv2aTexture *t)
{
    size_t n;
    const uint64_t ram = 64ull * 1024 * 1024, base = 0x80000000ull;
    if (t->pitch) n = (size_t)t->pitch * t->height;
    else if (t->color == 0x0C) n = (size_t)t->width * t->height / 2;
    else if (t->color == 0x0E || t->color == 0x0F) n = (size_t)t->width * t->height;
    else n = (size_t)t->width * t->height * 4;
    if ((uint64_t)t->offset + n <= ram) return n;
    if (t->offset >= base && (uint64_t)t->offset + n <= base + ram) return n;
    return 0;
}

/* ---- shader compiler: d3dcompiler_47.dll beside the exe, else the system's, else the June 2010 d3dcompiler_43 ---- */
typedef HRESULT (WINAPI *PFN_D3DCOMPILE_)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR,
                                          UINT, UINT, ID3DBlob **, ID3DBlob **);
static PFN_D3DCOMPILE_ s_D3DCompile;

static int load_compiler(void)
{
    static int tried;
    HMODULE m = NULL;
    char path[MAX_PATH + 32], *slash;
    if (tried) return s_D3DCompile != NULL;
    tried = 1;
    {
        /* Windows XP: d3dcompiler_47.dll needs functions XP lacks and would pop an "Entry Point Not Found" box, so no error
         * boxes for these loads, and on XP try the June 2010 runtime's d3dcompiler_43.dll first (beside the exe, then the system). */
        UINT old_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
        int xp = (GetVersion() & 0xFF) < 6, k;
        static const char *const names[2][2] = { { "d3dcompiler_47.dll", "d3dcompiler_43.dll" },
                                                 { "d3dcompiler_43.dll", "d3dcompiler_47.dll" } };
        for (k = 0; k < 2 && !m; k++) {
            if (GetModuleFileNameA(NULL, path, MAX_PATH) && (slash = strrchr(path, '\\')) != NULL) {
                strcpy(slash + 1, names[xp][k]);
                m = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
            }
            if (!m) m = LoadLibraryA(names[xp][k]);
        }
        SetErrorMode(old_mode);
    }
    if (m) s_D3DCompile = (PFN_D3DCOMPILE_)(void *)GetProcAddress(m, "D3DCompile");
    if (!s_D3DCompile) logf_("no shader compiler (d3dcompiler_47.dll / d3dcompiler_43.dll) found\n");
    return s_D3DCompile != NULL;
}

static ID3DBlob *compile_src(const char *src, size_t len, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr;
    if (!load_compiler()) return NULL;
    hr = s_D3DCompile(src, len, "d3d9_backend.hlsl", NULL, NULL, entry, target, 1u << 15 /* optimisation level 3 */, 0, &code, &err);
    if (FAILED(hr)) {
        logf_("D3DCompile %s failed 0x%08lX: %s\n", entry, (unsigned long)hr,
              err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        code = NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static IDirect3DPixelShader9 *make_ps(const char *src, size_t len, const char *entry)
{
    IDirect3DPixelShader9 *ps = NULL;
    ID3DBlob *b = compile_src(src, len, entry, "ps_3_0");
    if (b) {
        if (FAILED(IDirect3DDevice9_CreatePixelShader(g.dev, (const DWORD *)ID3D10Blob_GetBufferPointer(b), &ps)))
            { logf_("CreatePixelShader failed (%s)\n", entry); ps = NULL; }
        ID3D10Blob_Release(b);
    }
    return ps;
}

static IDirect3DVertexShader9 *make_vs(const char *src, size_t len, const char *entry)
{
    IDirect3DVertexShader9 *vs = NULL;
    ID3DBlob *b = compile_src(src, len, entry, "vs_3_0");
    if (b) {
        if (FAILED(IDirect3DDevice9_CreateVertexShader(g.dev, (const DWORD *)ID3D10Blob_GetBufferPointer(b), &vs)))
            { logf_("CreateVertexShader failed (%s)\n", entry); vs = NULL; }
        ID3D10Blob_Release(b);
    }
    return vs;
}

/* ---- vertex shaders: surface pixels -> clip space, w = 1/rhw so colour and texel UVs interpolate
 * perspective-correct. The D3D9 half-pixel shift (pixel centres on integers) is applied here. */
static const char k_hlsl_vs[] =
    "float4 K : register(c0);\n"                                  /* x, y: surface size; z: depth scale */
    "float4 xf(float3 p, float rhw) {\n"
    "  float w = (rhw > 1e-12) ? 1.0 / rhw : 1.0;\n"
    "  float cx = p.x / K.x * 2.0 - 1.0 - 1.0 / K.x;\n"
    "  float cy = 1.0 - p.y / K.y * 2.0 + 1.0 / K.y;\n"
    "  float cz = saturate(p.z * K.z);\n"
    "  return float4(cx * w, cy * w, cz * w, w);\n"
    "}\n"
    "struct VS  { float3 p : POSITION; float rhw : TEXCOORD7; float4 c : COLOR0; float2 uv : TEXCOORD0; };\n"
    "struct VSO { float4 pos : POSITION; float4 c : COLOR0; float2 uv : TEXCOORD0; };\n"
    "VSO vs_s(VS i) { VSO o; o.pos = xf(i.p, i.rhw); o.c = i.c; o.uv = i.uv; return o; }\n"
    "struct VR  { float3 p : POSITION; float rhw : TEXCOORD7; float4 c : COLOR0;\n"
    "             float4 d1 : TEXCOORD1; float4 t0 : TEXCOORD2; float4 t1 : TEXCOORD3;\n"
    "             float4 t2 : TEXCOORD4; float4 t3 : TEXCOORD5; float fog : TEXCOORD6; };\n"
    "struct VRO { float4 pos : POSITION; float4 c : COLOR0; float4 d1 : TEXCOORD1;\n"
    "             float4 t0 : TEXCOORD2; float4 t1 : TEXCOORD3; float4 t2 : TEXCOORD4;\n"
    "             float4 t3 : TEXCOORD5; float fog : TEXCOORD6; };\n"
    "VRO vs_rc(VR i) {\n"
    "  VRO o; o.pos = xf(i.p, i.rhw); o.c = i.c; o.d1 = i.d1;\n"
    "  o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3; o.fog = i.fog;\n"
    "  return o;\n"
    "}\n";

/* ---- text building for the generated pixel shaders ---- */
typedef struct { char *p; size_t n, cap; } Sb;

static void sb_f(Sb *s, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (s->n + 1 >= s->cap) return;
    va_start(ap, fmt);
    k = vsnprintf(s->p + s->n, s->cap - s->n, fmt, ap);
    va_end(ap);
    if (k > 0) s->n += (size_t)k;
    if (s->n >= s->cap) s->n = s->cap - 1;
}

/* alpha test: the NV2A compares the 8-bit alpha with an 8-bit reference; K[23].x (or Kz) holds the reference */
static void emit_atest(Sb *s, const char *alpha, const char *ref, int atest, uint32_t afunc)
{
    static const char *const cond[8] = { "false", "a_ < r_", "a_ == r_", "a_ <= r_", "a_ > r_", "a_ != r_", "a_ >= r_", "true" };
    if (!atest) return;
    sb_f(s, "  { float a_ = floor(%s * 255.0 + 0.5); float r_ = %s; clip((%s) ? 1.0 : -1.0); }\n",
         alpha, ref, cond[afunc & 7u]);
}

/* ---- the stage-0 pixel shader: diffuse x texture, with the alpha test ---- */
static IDirect3DPixelShader9 *get_ps_simple(int textured, int atest, uint32_t afunc)
{
    static IDirect3DPixelShader9 *ps[2][9];
    static unsigned char tried[2][9];
    int ai = atest ? 1 + (int)(afunc & 7u) : 0;
    char buf[1024];
    Sb s = { buf, 0, sizeof buf };
    textured = textured ? 1 : 0;
    if (tried[textured][ai]) return ps[textured][ai];
    tried[textured][ai] = 1;
    sb_f(&s, "sampler2D s0 : register(s0);\nfloat4 K : register(c0);\n"      /* xy: 1 / texture size, z: alpha reference */
             "struct PI { float4 c : COLOR0; float2 uv : TEXCOORD0; };\n"
             "float4 ps_s(PI i) : COLOR {\n  float4 c = i.c;\n");
    if (textured) sb_f(&s, "  c *= tex2D(s0, i.uv * K.xy);\n");
    emit_atest(&s, "c.a", "K.z", atest, afunc);
    sb_f(&s, "  return c;\n}\n");
    ps[textured][ai] = make_ps(buf, s.n, "ps_s");
    return ps[textured][ai];
}

/* ---- the register-combiner pixel shader, one per distinct combiner program ----
 * Port of d3d11_rc.hlsl (and overrides/nv2a_combiner.c): the combiner words are decoded here, in C, so the
 * shader is plain float arithmetic with every register index and branch already resolved.
 *   Constants: K[0..3] texture stage k (xy: texel -> uv scale, z: 1 when the stage has a texture), K[4] fog
 *   colour, K[5], K[6] final-combiner constants, K[7+2s], K[8+2s] stage s factor 0 / factor 1, K[23].x alpha ref. */
#define RC_NK 24

static void rc_in(char *d, size_t n, uint32_t b, int alpha_only)
{
    uint32_t reg = b & 15u, m = (b >> 5) & 7u;
    char v[24];
    if (alpha_only) snprintf(v, sizeof v, (b & 0x10u) ? "R%u.a" : "R%u.b", reg);
    else            snprintf(v, sizeof v, (b & 0x10u) ? "R%u.aaa" : "R%u.rgb", reg);
    switch (m) {
    case 0:  snprintf(d, n, "max(%s, 0.0)", v); break;
    case 1:  snprintf(d, n, "(1.0 - saturate(%s))", v); break;
    case 2:  snprintf(d, n, "(2.0 * max(%s, 0.0) - 1.0)", v); break;
    case 3:  snprintf(d, n, "(-2.0 * max(%s, 0.0) + 1.0)", v); break;
    case 4:  snprintf(d, n, "(max(%s, 0.0) - 0.5)", v); break;
    case 5:  snprintf(d, n, "(-max(%s, 0.0) + 0.5)", v); break;
    case 6:  snprintf(d, n, "%s", v); break;
    default: snprintf(d, n, "(-%s)", v); break;
    }
}

static void rc_map(char *d, size_t n, const char *x, uint32_t m)    /* m: output mapping bits (fl & 0x38) */
{
    switch (m) {
    case 0x08: snprintf(d, n, "(%s - 0.5)", x); break;
    case 0x10: snprintf(d, n, "(%s * 2.0)", x); break;
    case 0x18: snprintf(d, n, "((%s - 0.5) * 2.0)", x); break;
    case 0x20: snprintf(d, n, "(%s * 4.0)", x); break;
    case 0x30: snprintf(d, n, "(%s * 0.5)", x); break;
    default:   snprintf(d, n, "%s", x); break;
    }
}

static int gen_rc_ps(Sb *s, const Nv2aCombiner *rc, int atest, uint32_t afunc)
{
    uint32_t control = rc->control, sp = rc->stage_program, f0 = rc->final0, f1 = rc->final1;
    uint32_t n = control & 255u, flags = control >> 8, k, st;
    char a[96], b[96], c[96], d[96], m1[160], m2[160];

    if (n > 8u) n = 8u;
    sb_f(s, "sampler2D s0 : register(s0);\nsampler2D s1 : register(s1);\n"
            "sampler2D s2 : register(s2);\nsampler2D s3 : register(s3);\n"
            "float4 K[%d] : register(c0);\n"
            "struct PI { float4 c : COLOR0; float4 d1 : TEXCOORD1; float4 t0 : TEXCOORD2; float4 t1 : TEXCOORD3;\n"
            "            float4 t2 : TEXCOORD4; float4 t3 : TEXCOORD5; float fog : TEXCOORD6; };\n"
            "float4 ps_rc(PI i) : COLOR {\n", RC_NK);
    for (k = 0; k < 16; k++) sb_f(s, "  float4 R%u = float4(0, 0, 0, 0);\n", k);
    sb_f(s, "  float4 tc0 = i.t0, tc1 = i.t1, tc2 = i.t2, tc3 = i.t3;\n");
    for (k = 0; k < 4; k++) {
        uint32_t mode = (sp >> (k * 5u)) & 31u;
        if (mode == 1u || mode == 2u)
            sb_f(s, "  float4 sm%u = tex2D(s%u, tc%u.xy / ((tc%u.w != 0.0) ? tc%u.w : 1.0) * K[%u].xy);\n"
                    "  if (K[%u].z == 0.0) sm%u = float4(1, 1, 1, 1);\n", k, k, k, k, k, k, k, k);
    }
    sb_f(s, "  R3 = float4(K[4].rgb, i.fog);\n  R4 = i.c;\n  R5 = i.d1;\n");
    for (k = 0; k < 4; k++) {
        uint32_t mode = (sp >> (k * 5u)) & 31u;
        switch (mode) {
        case 0:  sb_f(s, "  R%u = float4(0, 0, 0, 1);\n", 8 + k); break;
        case 1: case 2: sb_f(s, "  R%u = sm%u;\n", 8 + k, k); break;
        case 3:  sb_f(s, "  R%u = float4(1, 1, 1, 1);\n", 8 + k); break;     /* cube map: not supported, reads white */
        case 4:  sb_f(s, "  R%u = saturate(tc%u);\n", 8 + k, k); break;
        default: sb_f(s, "  R%u = float4(0, 0, 0, 0);\n", 8 + k); break;
        }
    }
    sb_f(s, (sp & 31u) ? "  R12.a = R8.a;\n" : "  R12.a = 1.0;\n");

    for (st = 0; st < n; st++) {
        uint32_t icw = rc->color_icw[st], aicw = rc->alpha_icw[st], ocw = rc->color_ocw[st], aocw = rc->alpha_ocw[st];
        uint32_t fl = ocw >> 12, afl = aocw >> 12, mo = fl & 0x38u, amo = afl & 0x38u, d4;
        sb_f(s, "  {\n");
        sb_f(s, "    R1 = K[%u]; R2 = K[%u];\n", 7u + 2u * ((flags & 0x010u) ? st : 0u), 8u + 2u * ((flags & 0x100u) ? st : 0u));
        if ((fl & 4u) || (afl & 4u))
            sb_f(s, (flags & 1u) ? "    bool mux = (R12.a >= 0.5);\n"
                                 : "    bool mux = (abs(fmod(trunc(R12.a * 255.0), 2.0)) >= 1.0);\n");
        rc_in(a, sizeof a, (icw >> 24) & 255u, 0); rc_in(b, sizeof b, (icw >> 16) & 255u, 0);
        rc_in(c, sizeof c, (icw >> 8) & 255u, 0);  rc_in(d, sizeof d, icw & 255u, 0);
        sb_f(s, "    float3 A = %s, B = %s, C = %s, D = %s;\n", a, b, c, d);
        sb_f(s, (fl & 2u) ? "    float3 pab = dot(A, B).xxx;\n" : "    float3 pab = A * B;\n");
        sb_f(s, (fl & 1u) ? "    float3 pcd = dot(C, D).xxx;\n" : "    float3 pcd = C * D;\n");
        rc_map(m1, sizeof m1, "pab", mo); sb_f(s, "    float3 ab = clamp(%s, -1.0, 1.0);\n", m1);
        rc_map(m1, sizeof m1, "pcd", mo); sb_f(s, "    float3 cd = clamp(%s, -1.0, 1.0);\n", m1);
        if (fl & 4u) sb_f(s, "    float3 sum = mux ? pcd : pab;\n"); else sb_f(s, "    float3 sum = pab + pcd;\n");
        rc_map(m1, sizeof m1, "sum", mo); sb_f(s, "    float3 ms = clamp(%s, -1.0, 1.0);\n", m1);

        rc_in(a, sizeof a, (aicw >> 24) & 255u, 1); rc_in(b, sizeof b, (aicw >> 16) & 255u, 1);
        rc_in(c, sizeof c, (aicw >> 8) & 255u, 1);  rc_in(d, sizeof d, aicw & 255u, 1);
        sb_f(s, "    float aA = %s, aB = %s, aC = %s, aD = %s;\n", a, b, c, d);
        sb_f(s, "    float apab = aA * aB, apcd = aC * aD;\n");
        rc_map(m1, sizeof m1, "apab", amo); sb_f(s, "    float aab = clamp(%s, -1.0, 1.0);\n", m1);
        rc_map(m1, sizeof m1, "apcd", amo); sb_f(s, "    float acd = clamp(%s, -1.0, 1.0);\n", m1);
        if (afl & 4u) sb_f(s, "    float asum = mux ? apcd : apab;\n"); else sb_f(s, "    float asum = apab + apcd;\n");
        rc_map(m2, sizeof m2, "asum", amo); sb_f(s, "    float ams = clamp(%s, -1.0, 1.0);\n", m2);

        d4 = (ocw >> 4) & 15u;
        if (d4) { sb_f(s, "    R%u.rgb = ab;\n", d4); if (fl & 0x80u) sb_f(s, "    R%u.a = ab.z;\n", d4); }
        d4 = ocw & 15u;
        if (d4) { sb_f(s, "    R%u.rgb = cd;\n", d4); if (fl & 0x40u) sb_f(s, "    R%u.a = cd.z;\n", d4); }
        d4 = (ocw >> 8) & 15u;
        if (d4) sb_f(s, "    R%u.rgb = ms;\n", d4);
        d4 = (aocw >> 4) & 15u; if (d4) sb_f(s, "    R%u.a = aab;\n", d4);
        d4 = aocw & 15u;        if (d4) sb_f(s, "    R%u.a = acd;\n", d4);
        d4 = (aocw >> 8) & 15u; if (d4) sb_f(s, "    R%u.a = ams;\n", d4);
        sb_f(s, "    R0 = float4(0, 0, 0, 0);\n  }\n");
    }

    if (f0 || f1) {
        uint32_t ff = f1 & 255u;
        sb_f(s, "  float4 outc;\n  {\n    R1 = K[5]; R2 = K[6];\n");
        sb_f(s, "    float3 fa = %s, fb = %s;\n", (ff & 0x40u) ? "1.0 - R5.rgb" : "R5.rgb", (ff & 0x20u) ? "1.0 - R12.rgb" : "R12.rgb");
        sb_f(s, (ff & 0x80u) ? "    R14 = float4(saturate(fa + fb), 0.0);\n" : "    R14 = float4(fa + fb, 0.0);\n");
        rc_in(a, sizeof a, (f1 >> 24) & 255u, 0); rc_in(b, sizeof b, (f1 >> 16) & 255u, 0);
        sb_f(s, "    R15 = float4((%s) * (%s), 0.0);\n", a, b);
        rc_in(a, sizeof a, (f0 >> 24) & 255u, 0); rc_in(b, sizeof b, (f0 >> 16) & 255u, 0);
        rc_in(c, sizeof c, (f0 >> 8) & 255u, 0);  rc_in(d, sizeof d, f0 & 255u, 0);
        sb_f(s, "    float3 A = %s, B = %s, C = %s, D = %s;\n", a, b, c, d);
        rc_in(a, sizeof a, (f1 >> 8) & 255u, 1);
        sb_f(s, "    outc = float4(D + C * (1.0 - A) + B * A, %s);\n  }\n", a);
    } else {
        sb_f(s, "  float4 outc = R12;\n");
    }
    sb_f(s, "  outc = saturate(outc);\n");
    emit_atest(s, "outc.a", "K[23].x", atest, afunc);
    sb_f(s, "  return outc;\n}\n");
    return s->n + 2 < s->cap;
}

#define RC_SPEC_MAX 2048
typedef struct { uint32_t key[4 + 32 + 2]; IDirect3DPixelShader9 *ps; } RcSpec;
static RcSpec s_spec[RC_SPEC_MAX];
static uint32_t s_nspec, s_spec_fail;

static IDirect3DPixelShader9 *rc_get_ps(const Nv2aCombiner *rc, int atest, uint32_t afunc)
{
    uint32_t key[4 + 32 + 2] = { 0 }, n = rc->control & 255u, i;
    char *buf;
    Sb s;
    IDirect3DPixelShader9 *ps = NULL;
    DWORD t0;

    if (n > 8u) n = 8u;
    key[0] = rc->control; key[1] = rc->stage_program; key[2] = rc->final0; key[3] = rc->final1;
    for (i = 0; i < n; i++) {
        key[4 + 4 * i] = rc->color_icw[i]; key[5 + 4 * i] = rc->alpha_icw[i];
        key[6 + 4 * i] = rc->color_ocw[i]; key[7 + 4 * i] = rc->alpha_ocw[i];
    }
    key[36] = atest ? 1u : 0u; key[37] = atest ? (afunc & 7u) : 0u;
    for (i = 0; i < s_nspec; i++)
        if (!memcmp(s_spec[i].key, key, sizeof key)) return s_spec[i].ps;
    if (s_nspec >= RC_SPEC_MAX) return NULL;
    buf = (char *)malloc(65536);
    if (!buf) return NULL;
    s.p = buf; s.n = 0; s.cap = 65536;
    t0 = GetTickCount();
    if (gen_rc_ps(&s, rc, atest, afunc))
        ps = make_ps(buf, s.n, "ps_rc");
    if (!ps) {
        s_spec_fail++;
        if (s_spec_fail <= 3) logf_("combiner shader failed (control %08X stage_program %05X final %08X %08X); source:\n%s\n",
                                    key[0], key[1], key[2], key[3], buf);
    }
    free(buf);
    memcpy(s_spec[s_nspec].key, key, sizeof key);
    s_spec[s_nspec].ps = ps;
    s_nspec++;
    if (g.log) logf_("combiner shader #%u compiled in %lu ms%s\n", s_nspec, (unsigned long)(GetTickCount() - t0), ps ? "" : " (FAILED)");
    return ps;
}

/* ---- render state ---- */
static DWORD s_rsv[256];
static unsigned char s_rsk[256];
static DWORD s_ssv[4][16];
static unsigned char s_ssk[4][16];
static IDirect3DBaseTexture9 *s_bound[4];

static void rs(D3DRENDERSTATETYPE t, DWORD v)
{
    if (s_rsk[t] && s_rsv[t] == v) return;
    s_rsk[t] = 1; s_rsv[t] = v;
    IDirect3DDevice9_SetRenderState(g.dev, t, v);
}

static void ss(int stage, D3DSAMPLERSTATETYPE t, DWORD v)
{
    if (s_ssk[stage][t] && s_ssv[stage][t] == v) return;
    s_ssk[stage][t] = 1; s_ssv[stage][t] = v;
    IDirect3DDevice9_SetSamplerState(g.dev, (DWORD)stage, t, v);
}

static void settex(int stage, IDirect3DTexture9 *t)
{
    if (s_bound[stage] == (IDirect3DBaseTexture9 *)t) return;
    s_bound[stage] = (IDirect3DBaseTexture9 *)t;
    IDirect3DDevice9_SetTexture(g.dev, (DWORD)stage, (IDirect3DBaseTexture9 *)t);
}

static void state_forget(void)
{
    memset(s_rsk, 0, sizeof s_rsk);
    s_vc_valid = 0;
    memset(s_ssk, 0, sizeof s_ssk);
    memset(s_bound, 0, sizeof s_bound);
    g.cur_rt = NULL; g.cur_ds = NULL;
}

static DWORD blend_rgb(uint32_t gl)
{
    switch (gl) {
    case 0:      return D3DBLEND_ZERO;
    case 1:      return D3DBLEND_ONE;
    case 0x300:  return D3DBLEND_SRCCOLOR;
    case 0x301:  return D3DBLEND_INVSRCCOLOR;
    case 0x302:  return D3DBLEND_SRCALPHA;
    case 0x303:  return D3DBLEND_INVSRCALPHA;
    case 0x304:  return D3DBLEND_DESTALPHA;
    case 0x305:  return D3DBLEND_INVDESTALPHA;
    case 0x306:  return D3DBLEND_DESTCOLOR;
    case 0x307:  return D3DBLEND_INVDESTCOLOR;
    case 0x308:  return D3DBLEND_SRCALPHASAT;
    case 0x8001: case 0x8003: return D3DBLEND_BLENDFACTOR;
    case 0x8002: case 0x8004: return D3DBLEND_INVBLENDFACTOR;
    default:     return D3DBLEND_ONE;
    }
}

static DWORD blend_alpha(DWORD b)       /* the alpha channel takes no _COLOR factors */
{
    switch (b) {
    case D3DBLEND_SRCCOLOR:     return D3DBLEND_SRCALPHA;
    case D3DBLEND_INVSRCCOLOR:  return D3DBLEND_INVSRCALPHA;
    case D3DBLEND_DESTCOLOR:    return D3DBLEND_DESTALPHA;
    case D3DBLEND_INVDESTCOLOR: return D3DBLEND_INVDESTALPHA;
    default:                    return b;
    }
}

static DWORD blend_op(uint32_t gl)
{
    switch (gl) {
    case 0x800A: return D3DBLENDOP_SUBTRACT;
    case 0x800B: return D3DBLENDOP_REVSUBTRACT;
    case 0x8007: return D3DBLENDOP_MIN;
    case 0x8008: return D3DBLENDOP_MAX;
    default:     return D3DBLENDOP_ADD;
    }
}

static DWORD stencil_op(uint32_t gl)
{
    switch (gl) {
    case 0:      return D3DSTENCILOP_ZERO;
    case 0x1E01: return D3DSTENCILOP_REPLACE;
    case 0x1E02: return D3DSTENCILOP_INCRSAT;
    case 0x1E03: return D3DSTENCILOP_DECRSAT;
    case 0x150A: return D3DSTENCILOP_INVERT;
    case 0x8507: return D3DSTENCILOP_INCR;
    case 0x8508: return D3DSTENCILOP_DECR;
    default:     return D3DSTENCILOP_KEEP;
    }
}

static DWORD cmp_func(uint32_t gl) { return (DWORD)((gl & 7u) + 1u); }     /* GL_NEVER..GL_ALWAYS = 0x200..0x207 */

static void apply_state(const Nv2aRenderState *s, int has_depth)
{
    DWORD mask = 0;
    if (s->color_mask & 0x00010000u) mask |= D3DCOLORWRITEENABLE_RED;
    if (s->color_mask & 0x00000100u) mask |= D3DCOLORWRITEENABLE_GREEN;
    if (s->color_mask & 0x00000001u) mask |= D3DCOLORWRITEENABLE_BLUE;
    if (s->color_mask & 0x01000000u) mask |= D3DCOLORWRITEENABLE_ALPHA;
    rs(D3DRS_COLORWRITEENABLE, mask);
    if (s->blend_enable) {
        DWORD sf = blend_rgb(s->blend_src), df = blend_rgb(s->blend_dst), op = blend_op(s->blend_eq);
        uint32_t c = s->blend_color;
        int ca = (s->blend_src == 0x8003 || s->blend_src == 0x8004 || s->blend_dst == 0x8003 || s->blend_dst == 0x8004) &&
                 !(s->blend_src == 0x8001 || s->blend_src == 0x8002 || s->blend_dst == 0x8001 || s->blend_dst == 0x8002);
        uint32_t a = (c >> 24) & 0xFF, r = ca ? a : (c >> 16) & 0xFF, gg = ca ? a : (c >> 8) & 0xFF, bb = ca ? a : c & 0xFF;
        rs(D3DRS_ALPHABLENDENABLE, TRUE);
        rs(D3DRS_SRCBLEND, sf); rs(D3DRS_DESTBLEND, df); rs(D3DRS_BLENDOP, op);
        rs(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        rs(D3DRS_SRCBLENDALPHA, blend_alpha(sf)); rs(D3DRS_DESTBLENDALPHA, blend_alpha(df)); rs(D3DRS_BLENDOPALPHA, op);
        rs(D3DRS_BLENDFACTOR, D3DCOLOR_ARGB(a, r, gg, bb));
    } else {
        rs(D3DRS_ALPHABLENDENABLE, FALSE);
    }
    rs(D3DRS_ALPHATESTENABLE, FALSE);          /* done in the pixel shader */
    rs(D3DRS_CULLMODE, D3DCULL_NONE);
    rs(D3DRS_SCISSORTESTENABLE, FALSE);
    rs(D3DRS_FOGENABLE, FALSE);
    rs(D3DRS_LIGHTING, FALSE);
    if (has_depth) {
        int zt = s->depth_test_enable != 0, zw = s->depth_write != 0;
        /* The NV2A's z test and z write are independent; D3D9's enable gates both, so "write without test"
         * becomes enable + ALWAYS. */
        rs(D3DRS_ZENABLE, (zt || zw) ? D3DZB_TRUE : D3DZB_FALSE);
        rs(D3DRS_ZFUNC, zt ? cmp_func(s->depth_func) : D3DCMP_ALWAYS);
        rs(D3DRS_ZWRITEENABLE, zw ? TRUE : FALSE);
        if (s->stencil_enable) {
            rs(D3DRS_STENCILENABLE, TRUE);
            rs(D3DRS_STENCILFUNC, cmp_func(s->stencil_func));
            rs(D3DRS_STENCILREF, s->stencil_ref & 0xFF);
            rs(D3DRS_STENCILMASK, s->stencil_func_mask & 0xFF);
            rs(D3DRS_STENCILWRITEMASK, s->stencil_write_mask & 0xFF);
            rs(D3DRS_STENCILFAIL, stencil_op(s->stencil_fail));
            rs(D3DRS_STENCILZFAIL, stencil_op(s->stencil_zfail));
            rs(D3DRS_STENCILPASS, stencil_op(s->stencil_zpass));
        } else {
            rs(D3DRS_STENCILENABLE, FALSE);
        }
    } else {
        rs(D3DRS_ZENABLE, D3DZB_FALSE);
        rs(D3DRS_ZWRITEENABLE, FALSE);
        rs(D3DRS_STENCILENABLE, FALSE);
    }
}

static void apply_sampler(int stage, const Nv2aTexture *t)
{
    uint32_t mn = (t->filter >> 16) & 0xFF, mg = (t->filter >> 24) & 0x0F;
    DWORD am[2], f = (mn == 1 && mg == 1) ? D3DTEXF_POINT : D3DTEXF_LINEAR;
    uint32_t a[2];
    int k;
    a[0] = t->addr_u; a[1] = t->addr_v;
    for (k = 0; k < 2; k++) {
        switch (a[k]) {
        case 1:  am[k] = D3DTADDRESS_WRAP; break;
        case 2:  am[k] = D3DTADDRESS_MIRROR; break;
        case 4:  am[k] = D3DTADDRESS_BORDER; break;
        default: am[k] = D3DTADDRESS_CLAMP; break;       /* 3 clamp, 5 clamp-to-edge */
        }
    }
    ss(stage, D3DSAMP_ADDRESSU, am[0]); ss(stage, D3DSAMP_ADDRESSV, am[1]);
    ss(stage, D3DSAMP_MINFILTER, f); ss(stage, D3DSAMP_MAGFILTER, f); ss(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
}

static void apply_sampler_point(int stage)
{
    ss(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); ss(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    ss(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT); ss(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    ss(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
}

/* ---- textures: decoded by nv2a_backend_decode_texture() into 0xAARRGGBB (== D3DFMT_A8R8G8B8) and cached by
 * (offset, format, size, pitch). Titles rewrite textures in place, so each use re-checks a hash of the guest
 * bytes and re-decodes + re-uploads when it changed. A texture whose address is a render surface we drew into is
 * sampled straight from that surface (render-to-texture). */
extern ptrdiff_t xbox_GetMemoryOffset(void);

#define MAX_TEX 768
typedef struct {
    uint32_t off, color, w, h, pitch;
    uint64_t hash;
    uint32_t hash_flip;              /* flip count when the texture was last checked */
    uint64_t sample;                 /* cheap sampled hash */
    uint64_t full_us;                /* time of the last full hash */
    IDirect3DTexture9 *tex;
    uint32_t last_use;
} TexEntry;

static TexEntry s_tex[MAX_TEX];
static int s_ntex;
static uint32_t *s_scratch;
static size_t s_scratch_cap;

static int tex_upload(IDirect3DTexture9 *t, const uint32_t *px, uint32_t w, uint32_t h)
{
    D3DLOCKED_RECT lr;
    uint32_t y;
    if (FAILED(IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0))) return 0;
    for (y = 0; y < h; y++)
        memcpy((uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch, px + (size_t)y * w, (size_t)w * 4);
    IDirect3DTexture9_UnlockRect(t, 0);
    return 1;
}

static TexEntry *get_texture(const Nv2aTexture *t)
{
    TexEntry *e = NULL;
    int i;
    uint64_t h = 0, t0;
    size_t span, need;

    if (!t->width || !t->height || t->width > 4096 || t->height > 4096) return NULL;
    for (i = 0; i < s_ntex; i++)
        if (s_tex[i].off == t->offset && s_tex[i].color == t->color && s_tex[i].w == t->width
            && s_tex[i].h == t->height && s_tex[i].pitch == t->pitch) { e = &s_tex[i]; break; }
    /* Hash once per presented frame: titles rewrite textures between frames, rarely between two draws of one. */
    if (e && e->hash_flip == g.flips && e->tex) { e->last_use = g.flips; return e; }
    t0 = qpc_us();
    span = tex_span(t);
    if (e && span) {
        /* Full hashes of 300+ textures every presented frame cost more than the rendering. Check a sample
         * every frame; take the full hash only when the sample moved or the last full hash is older than 500 ms. */
        uint64_t sh = sample_span(((uint8_t *)XBOX_HOST(t->offset)), span);
        if (sh == e->sample && t0 - e->full_us < 500000) {
            e->hash_flip = g.flips; e->last_use = g.flips;
            return e;
        }
    }
    if (span) h = hash_span(((uint8_t *)XBOX_HOST(t->offset)), span);
    if (e && e->hash == h) {
        e->sample = span ? sample_span(((uint8_t *)XBOX_HOST(t->offset)), span) : 0;
        e->full_us = t0; e->hash_flip = g.flips; e->last_use = g.flips;
        return e;
    }

    need = (size_t)t->width * t->height;
    if (need > s_scratch_cap) {
        free(s_scratch);
        s_scratch = (uint32_t *)malloc(need * 4);
        s_scratch_cap = s_scratch ? need : 0;
        if (!s_scratch) return NULL;
    }
    if (!nv2a_backend_decode_texture(t, s_scratch)) {
        if (g.log) logf_("texture decode failed: off 0x%08X fmt 0x%X %ux%u pitch %u\n",
                         t->offset, t->color, t->width, t->height, t->pitch);
        return NULL;
    }
    if (!e) {
        if (s_ntex < MAX_TEX) e = &s_tex[s_ntex++];
        else {                                           /* evict the least recently used */
            int w = 0;
            for (i = 1; i < MAX_TEX; i++) if (s_tex[i].last_use < s_tex[w].last_use) w = i;
            e = &s_tex[w];
            if (e->tex) IDirect3DTexture9_Release(e->tex);
        }
        memset(e, 0, sizeof *e);
        if (FAILED(IDirect3DDevice9_CreateTexture(g.dev, t->width, t->height, 1, 0, D3DFMT_A8R8G8B8,
                                                  D3DPOOL_MANAGED, &e->tex, NULL))) {
            logf_("texture %ux%u failed\n", t->width, t->height);
            if (e == &s_tex[s_ntex - 1]) s_ntex--; else memset(e, 0, sizeof *e);
            return NULL;
        }
        e->off = t->offset; e->color = t->color; e->w = t->width; e->h = t->height; e->pitch = t->pitch;
    }
    tex_upload(e->tex, s_scratch, t->width, t->height);
    e->hash = h;
    e->sample = span ? sample_span(((uint8_t *)XBOX_HOST(t->offset)), span) : 0;
    e->full_us = t0;
    e->hash_flip = g.flips;
    e->last_use = g.flips;
    return e;
}

/* ---- render targets and depth buffers ---- */
static struct { uint32_t va, w, h; IDirect3DSurface9 *surf; } zpool[8];
static int zpool_n;
static RenderTarget *s_last;         /* the last presented game image (valid between flips) */

static void rt_free(RenderTarget *r)
{
    if (r->surf == g.cur_rt) g.cur_rt = NULL;
    if (r->depth == g.cur_ds) g.cur_ds = NULL;
    if (r->depth) IDirect3DSurface9_Release(r->depth);
    if (r->surf) IDirect3DSurface9_Release(r->surf);
    if (r->tex) IDirect3DTexture9_Release(r->tex);
    memset(r, 0, sizeof *r);
}

static void rt_release_all(void)
{
    int i;
    for (i = 0; i < g.nrt; i++) rt_free(&g.rt[i]);
    g.nrt = 0;
    for (i = 0; i < zpool_n; i++) if (zpool[i].surf) IDirect3DSurface9_Release(zpool[i].surf);
    zpool_n = 0;
    s_last = NULL;
}

static RenderTarget *get_rt(const Nv2aSurface *s)
{
    RenderTarget *r;
    int i;

    if (s->bytes_per_pixel != 4) return NULL;      /* 16-bit surfaces: not handled yet */
    for (i = 0; i < g.nrt; i++)
        if (g.rt[i].va == s->color_va && g.rt[i].w == s->width && g.rt[i].h == s->height)
            return &g.rt[i];
    if (g.nrt >= MAX_RT) {
        int worst = 0;                              /* drop the least used entry */
        for (i = 1; i < g.nrt; i++) if (g.rt[i].uses < g.rt[worst].uses) worst = i;
        rt_free(&g.rt[worst]);
        g.rt[worst] = g.rt[--g.nrt];
        s_last = NULL;
    }
    r = &g.rt[g.nrt];
    memset(r, 0, sizeof *r);
    if (FAILED(IDirect3DDevice9_CreateTexture(g.dev, s->width, s->height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                              D3DPOOL_DEFAULT, &r->tex, NULL)) ||
        FAILED(IDirect3DTexture9_GetSurfaceLevel(r->tex, 0, &r->surf))) {
        logf_("render target %ux%u failed\n", s->width, s->height);
        if (r->tex) IDirect3DTexture9_Release(r->tex);
        memset(r, 0, sizeof *r);
        return NULL;
    }
    r->va = s->color_va;
    r->w = s->width;
    r->h = s->height;
    g.nrt++;
    if (g.log)
        logf_("new render surface #%d va=0x%08X %ux%u pitch=%u aa=%ux%u\n", g.nrt,
              s->color_va, s->width, s->height, s->pitch, s->aa_sx, s->aa_sy);
    return r;
}

/* The title has ONE zeta buffer that every pass (display buffer and the offscreen render targets) tests against;
 * it clears it with the display buffer only. All render targets of one size and zeta address share one D3D9
 * depth buffer. */
static void ensure_depth(RenderTarget *r, const Nv2aRenderState *rs_)
{
    int k;
    IDirect3DSurface9 *ds = NULL;

    if (!rs_ || !rs_->zeta_va) return;
    if (r->depth && r->zeta_va == rs_->zeta_va) return;
    if (r->depth) {
        if (r->depth == g.cur_ds) g.cur_ds = NULL;
        IDirect3DSurface9_Release(r->depth);
        r->depth = NULL;
    }
    for (k = 0; k < zpool_n; k++)
        if (zpool[k].va == rs_->zeta_va && zpool[k].w == r->w && zpool[k].h == r->h) {
            r->depth = zpool[k].surf;
            IDirect3DSurface9_AddRef(r->depth);
            r->zeta_va = rs_->zeta_va;
            return;
        }
    if (FAILED(IDirect3DDevice9_CreateDepthStencilSurface(g.dev, r->w, r->h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0,
                                                          FALSE, &ds, NULL))) {
        logf_("depth buffer %ux%u failed\n", r->w, r->h);
        return;
    }
    r->depth = ds;
    r->zeta_va = rs_->zeta_va;
    if (zpool_n < 8) {
        zpool[zpool_n].va = rs_->zeta_va; zpool[zpool_n].w = r->w; zpool[zpool_n].h = r->h; zpool[zpool_n].surf = ds;
        IDirect3DSurface9_AddRef(ds);
        zpool_n++;
    }
    if (g.log) logf_("depth buffer for surface 0x%08X: zeta 0x%08X\n", r->va, r->zeta_va);
}

static void bind_rt(RenderTarget *r, int use_depth)
{
    IDirect3DSurface9 *ds = use_depth ? r->depth : NULL;
    int i;
    for (i = 0; i < 4; i++)                      /* never sample what we render to */
        if (s_bound[i] == (IDirect3DBaseTexture9 *)r->tex) settex(i, NULL);
    if (g.cur_rt != r->surf) {
        D3DVIEWPORT9 vp;
        HRESULT h1 = IDirect3DDevice9_SetRenderTarget(g.dev, 0, r->surf);
        if (g.log && FAILED(h1)) logf_("SetRenderTarget hr=0x%08lX\n", (unsigned long)h1);
        g.cur_rt = r->surf;
        vp.X = vp.Y = 0; vp.Width = r->w; vp.Height = r->h; vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
        IDirect3DDevice9_SetViewport(g.dev, &vp);
    }
    if (g.cur_ds != ds) {
        HRESULT h2 = IDirect3DDevice9_SetDepthStencilSurface(g.dev, ds);
        if (g.log && FAILED(h2)) logf_("SetDepthStencilSurface hr=0x%08lX\n", (unsigned long)h2);
        g.cur_ds = ds;
    }
}

/* ---- device ---- */
#define VB_BYTES (4u * 1024u * 1024u)
#define XVB_BYTES ((VB_BYTES / (uint32_t)sizeof(Nv2aVertex)) * (uint32_t)sizeof(Nv2aVertexExt))

static int create_buffers(void)
{
    if (FAILED(IDirect3DDevice9_CreateVertexBuffer(g.dev, VB_BYTES, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                                                   D3DPOOL_DEFAULT, &g.vb, NULL)) ||
        FAILED(IDirect3DDevice9_CreateVertexBuffer(g.dev, XVB_BYTES, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                                                   D3DPOOL_DEFAULT, &g.xvb, NULL))) {
        logf_("vertex buffer creation failed\n");
        return 0;
    }
    if (FAILED(IDirect3DDevice9_CreateVertexBuffer(g.dev, 8u * 1024u * 1024u, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                                                   D3DPOOL_DEFAULT, &s_gvb, NULL))) {
        logf_("vertex buffer creation failed\n");
        return 0;
    }
    if (FAILED(IDirect3DDevice9_CreateIndexBuffer(g.dev, (1u << 20) * 2u, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                                  D3DPOOL_DEFAULT, &s_gib, NULL))) {
        logf_("index buffer creation failed\n");
        return 0;
    }
    g.vb_off = g.xvb_off = s_gvb_off = s_gib_off = 0;
    return 1;
}

static void release_buffers(void)
{
    if (g.vb) { IDirect3DVertexBuffer9_Release(g.vb); g.vb = NULL; }
    if (g.xvb) { IDirect3DVertexBuffer9_Release(g.xvb); g.xvb = NULL; }
    if (s_gvb) { IDirect3DVertexBuffer9_Release(s_gvb); s_gvb = NULL; }
    if (s_gib) { IDirect3DIndexBuffer9_Release(s_gib); s_gib = NULL; }
}

static BOOL CALLBACK mon_enum(HMONITOR m, HDC dc, LPRECT r, LPARAM p)
{
    int *wh = (int *)p;
    (void)m; (void)dc;
    if (r->right - r->left > wh[0]) wh[0] = r->right - r->left;
    if (r->bottom - r->top > wh[1]) wh[1] = r->bottom - r->top;
    return TRUE;
}

static int create_device(void)
{
    static const D3DVERTEXELEMENT9 el_s[] = {
        { 0, 0,  D3DDECLTYPE_FLOAT3,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT1,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 7 },
        { 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        { 0, 20, D3DDECLTYPE_FLOAT2,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 el_rc[] = {
        { 0, 0,  D3DDECLTYPE_FLOAT3,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT1,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 7 },
        { 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        { 0, 20, D3DDECLTYPE_FLOAT2,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 1, 0,  D3DDECLTYPE_FLOAT4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
        { 1, 16, D3DDECLTYPE_FLOAT4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
        { 1, 32, D3DDECLTYPE_FLOAT4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3 },
        { 1, 48, D3DDECLTYPE_FLOAT4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 4 },
        { 1, 64, D3DDECLTYPE_FLOAT4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 5 },
        { 1, 80, D3DDECLTYPE_FLOAT1,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 6 },
        D3DDECL_END()
    };
    int mon[2] = { 640, 480 };
    DWORD fl = D3DCREATE_FPU_PRESERVE;
    HRESULT hr;
    uint32_t white = 0xFFFFFFFFu;

    g.d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g.d9) { logf_("Direct3DCreate9 failed\n"); return 0; }
    if (FAILED(IDirect3D9_GetDeviceCaps(g.d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &g.caps))) {
        logf_("no Direct3D 9 hardware device\n");
        MessageBoxA(NULL, "NHL Hitz Pro needs a graphics card with Direct3D 9 hardware support.", "NHL Hitz Pro", MB_ICONERROR);
        return 0;
    }
    if (g.caps.PixelShaderVersion < D3DPS_VERSION(3, 0) || g.caps.VertexShaderVersion < D3DVS_VERSION(3, 0)) {
        logf_("Shader Model 3.0 is required (pixel shader %lu.%lu)\n", (unsigned long)D3DSHADER_VERSION_MAJOR(g.caps.PixelShaderVersion),
              (unsigned long)D3DSHADER_VERSION_MINOR(g.caps.PixelShaderVersion));
        MessageBoxA(NULL, "NHL Hitz Pro needs a graphics card with Shader Model 3.0 support.", "NHL Hitz Pro", MB_ICONERROR);
        return 0;
    }
    EnumDisplayMonitors(NULL, NULL, mon_enum, (LPARAM)mon);
    memset(&g.pp, 0, sizeof g.pp);
    g.pp.BackBufferWidth = (UINT)mon[0];
    g.pp.BackBufferHeight = (UINT)mon[1];
    g.pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    g.pp.BackBufferCount = 1;
    g.pp.SwapEffect = D3DSWAPEFFECT_COPY;          /* Present can take a source rectangle: the window size needs no Reset */
    g.pp.hDeviceWindow = g.hwnd;
    g.pp.Windowed = TRUE;
    g.pp.PresentationInterval = g.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    fl |= (g.caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ? D3DCREATE_HARDWARE_VERTEXPROCESSING : D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    hr = IDirect3D9_CreateDevice(g.d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g.hwnd, fl, &g.pp, &g.dev);
    if (FAILED(hr)) {
        fl = D3DCREATE_FPU_PRESERVE | D3DCREATE_SOFTWARE_VERTEXPROCESSING;
        hr = IDirect3D9_CreateDevice(g.d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g.hwnd, fl, &g.pp, &g.dev);
    }
    if (FAILED(hr)) { logf_("CreateDevice failed 0x%08lX\n", (unsigned long)hr); return 0; }
    logf_("device up, back buffer %ux%u, pixel shader %lu.%lu\n", g.pp.BackBufferWidth, g.pp.BackBufferHeight,
          (unsigned long)D3DSHADER_VERSION_MAJOR(g.caps.PixelShaderVersion), (unsigned long)D3DSHADER_VERSION_MINOR(g.caps.PixelShaderVersion));

    if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(g.dev, el_s, &g.decl_s)) ||
        FAILED(IDirect3DDevice9_CreateVertexDeclaration(g.dev, el_rc, &g.decl_rc))) {
        logf_("vertex declaration failed\n");
        return 0;
    }
    g.vs_s = make_vs(k_hlsl_vs, sizeof k_hlsl_vs - 1, "vs_s");
    g.vs_rc = make_vs(k_hlsl_vs, sizeof k_hlsl_vs - 1, "vs_rc");
    if (!g.vs_s || !g.vs_rc) {
        MessageBoxA(NULL, "NHL Hitz Pro could not compile its shaders. Make sure d3dcompiler_47.dll is in the game folder.",
                    "NHL Hitz Pro", MB_ICONERROR);
        return 0;
    }
    if (!create_buffers()) return 0;
    if (SUCCEEDED(IDirect3DDevice9_CreateTexture(g.dev, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g.white, NULL)))
        tex_upload(g.white, &white, 1, 1);
    if (!g.white) return 0;
    state_forget();
    return 1;
}

/* The present interval changed (F1 VSync), or the device was lost and can be reset. */
static int dev_reset(void)
{
    HRESULT hr;
    if (g.in_scene) { IDirect3DDevice9_EndScene(g.dev); g.in_scene = 0; }
    rt_release_all();
    release_buffers();
    g.pp.PresentationInterval = g.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    hr = IDirect3DDevice9_Reset(g.dev, &g.pp);
    g.reset_pending = 0;
    if (FAILED(hr)) { g.lost = 1; return 0; }
    state_forget();
    g.lost = 0;
    if (!create_buffers()) { g.lost = 1; return 0; }
    logf_("device reset (vsync %d)\n", g.vsync);
    return 1;
}

static int init_window_and_device(void)
{
    WNDCLASSA wc;

    if (g.inited) return !g.failed;
    g.inited = 1;
    g.vsync = getenv("RECOMP_D3D11_VSYNC") && getenv("RECOMP_D3D11_VSYNC")[0] == '1';
    settings_load();
    g.log = getenv("RECOMP_D3D9_LOG") != NULL;
    g.t0 = GetTickCount();

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));   /* resources/nhl_hitz_pro.rc */
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "NhlD3D9Backend";
    RegisterClassA(&wc);
    g.hwnd = CreateWindowExA(0, wc.lpszClassName,
#ifdef NHL_RELEASE
        "NHL Hitz Pro",
#else
        "NHL Hitz Pro - D3D9 back end",
#endif
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 656, 519,
                             NULL, NULL, wc.hInstance, NULL);
    if (!g.hwnd) { logf_("CreateWindow failed (%lu)\n", GetLastError()); g.failed = 1; return 0; }
    if (f11_on() && want_fullscreen_at_start()) toggle_fullscreen(g.hwnd);
    if (!create_device()) { g.failed = 1; return 0; }
    return 1;
}

/* ---- draw / clear ---- */
static void unpack4(float *o, uint32_t c)
{
    o[0] = (float)((c >> 16) & 255u) / 255.0f; o[1] = (float)((c >> 8) & 255u) / 255.0f;
    o[2] = (float)(c & 255u) / 255.0f;         o[3] = (float)(c >> 24) / 255.0f;
}

/* Copy bytes into a dynamic vertex buffer ring; returns the byte offset they were put at, or -1. */
static int64_t vb_put(IDirect3DVertexBuffer9 *vb, uint32_t *off, uint32_t cap, const void *src, uint32_t bytes)
{
    void *p;
    DWORD fl;
    uint32_t at;
    if (*off + bytes > cap) { *off = 0; fl = D3DLOCK_DISCARD; }
    else fl = *off ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD;
    at = *off;
    if (FAILED(IDirect3DVertexBuffer9_Lock(vb, at, bytes, &p, fl))) return -1;
    memcpy(p, src, bytes);
    IDirect3DVertexBuffer9_Unlock(vb);
    *off += bytes;
    return (int64_t)at;
}

/* The texture a batch samples at address t: a render surface of ours, else the cached guest texture.
 * *self is set when it is the surface being drawn to (then nothing is sampled, as in the D3D11 build). */
static IDirect3DTexture9 *tex_for(const Nv2aTexture *t, const RenderTarget *cur, int *self)
{
    int i;
    *self = 0;
    for (i = 0; i < g.nrt; i++)
        if (g.rt[i].va == t->offset && g.rt[i].w == t->width && g.rt[i].h == t->height) {
            if (&g.rt[i] == cur) { *self = 1; return NULL; }
            return g.rt[i].tex;
        }
    {
        TexEntry *e = get_texture(t);
        return e ? e->tex : NULL;
    }
}

#include "d3d9_gvs.inc"

static void be_draw_impl(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs_;
    const Nv2aBatchExt *x;
    RenderTarget *r;
    uint32_t count, bytes;
    int64_t voff;
    float vk[4], zscale;
    int atest, usez, gpu;
    uint32_t afunc;

    if (!b || !b->count || !b->state || !init_window_and_device() || g.lost) return;
    x = nv2a_pbx_current();
    gpu = x && x->gpu && x->gpu->block;          /* a GPU vertex-program batch: no CPU vertices */
    if (!gpu && !b->vertices) return;
    rs_ = b->state;
    r = get_rt(s);
    if (!r) return;
    r->uses++;
    ensure_depth(r, rs_);

    count = b->count - b->count % 3;
    if (count > (VB_BYTES / (uint32_t)sizeof(Nv2aVertex)) / 3 * 3) count = (VB_BYTES / (uint32_t)sizeof(Nv2aVertex)) / 3 * 3;
    voff = 0;
    if (!gpu) {
        if (!count) return;
        bytes = count * (uint32_t)sizeof(Nv2aVertex);
        voff = vb_put(g.vb, &g.vb_off, VB_BYTES, b->vertices, bytes);
        if (voff < 0) return;
    }

    zscale = (rs_->depth_max > 1.0f) ? 1.0f / rs_->depth_max : 1.0f;
    vk[0] = (float)r->w; vk[1] = (float)r->h; vk[2] = zscale; vk[3] = 0.0f;
    atest = rs_->alpha_test_enable != 0;
    afunc = rs_->alpha_func & 7u;

    usez = (rs_->zeta_va && r->depth) ? 1 : 0;
    if (!g.in_scene) { IDirect3DDevice9_BeginScene(g.dev); g.in_scene = 1; }     /* draws are only valid inside a scene */
    if (g.log && !gpu) {
        static int nl;
        const Nv2aBatchExt *xl = nv2a_pbx_current();
        if (nl < 40) {
            nl++;
            logf_("draw %d: rt %08X %ux%u n %u tex %d rc %d blend %u z %u/%u v0 (%.1f,%.1f,%g,%g) diff %08X\n", nl, s->color_va, r->w, r->h, count,
                  b->texture != NULL, xl && xl->ext && xl->rc, rs_->blend_enable, rs_->depth_test_enable, rs_->depth_write,
                  b->vertices[0].x, b->vertices[0].y, b->vertices[0].z, b->vertices[0].rhw, (unsigned)b->vertices[0].diffuse);
        }
    }
    if (getenv("RECOMP_D3D9_NOZ")) usez = 0;
    bind_rt(r, usez);
    if (!getenv("RECOMP_D3D9_NOSTATE")) apply_state(rs_, usez);
    IDirect3DDevice9_SetVertexShaderConstantF(g.dev, 0, vk, 1);
    s_vc_valid = 0;                              /* c0 is now this draw's; the GPU vertex-program cache must reload */

    {   static int simple = -1;            /* RECOMP_D3D9_SIMPLE=1: diagnostic, stage-0 path only */
        if (simple < 0) simple = getenv("RECOMP_D3D9_SIMPLE") != NULL;
        if (simple) x = NULL;
    }
    if (x && (x->ext || gpu) && x->rc) {                 /* register combiners */
        const Nv2aCombiner *rc = x->rc;
        IDirect3DPixelShader9 *ps = rc_get_ps(rc, atest, afunc);
        if (ps) {
            IDirect3DTexture9 *tx[4];
            float K[RC_NK][4];
            uint32_t st, xb = count * (uint32_t)sizeof(Nv2aVertexExt);
            int64_t xoff;
            memset(K, 0, sizeof K);
            for (st = 0; st < 4; st++) {
                IDirect3DTexture9 *t = NULL;
                int self = 0;
                if (x->tex_valid[st] && !x->tex_cube[st] && ((rc->stage_program >> (st * 5)) & 0x1F) != 0)
                    t = tex_for(&x->tex[st], r, &self);
                if (t) {
                    K[st][2] = 1.0f;
                    if (x->tex_sized[st]) { K[st][0] = K[st][1] = 1.0f; }
                    else { K[st][0] = 1.0f / (float)x->tex[st].width; K[st][1] = 1.0f / (float)x->tex[st].height; }
                } else {
                    t = g.white;
                    K[st][0] = K[st][1] = 1.0f;
                }
                tx[st] = t;
            }
            unpack4(K[5], rc->final_c0); unpack4(K[6], rc->final_c1);
            K[4][0] = x->fog_color[0]; K[4][1] = x->fog_color[1]; K[4][2] = x->fog_color[2];
            for (st = 0; st < 8; st++) { unpack4(K[7 + 2 * st], rc->factor0[st]); unpack4(K[8 + 2 * st], rc->factor1[st]); }
            K[23][0] = (float)(rs_->alpha_ref & 0xFF);
            xoff = gpu ? 0 : vb_put(g.xvb, &g.xvb_off, XVB_BYTES, x->ext, xb);
            if (xoff >= 0) {
                for (st = 0; st < 4; st++) {
                    settex((int)st, tx[st]);
                    if (K[st][2] != 0.0f) apply_sampler((int)st, &x->tex[st]); else apply_sampler_point((int)st);
                }
                if (gpu) {                                   /* the vertex program runs on the GPU */
                    IDirect3DDevice9_SetPixelShader(g.dev, ps);
                    IDirect3DDevice9_SetPixelShaderConstantF(g.dev, 0, &K[0][0], RC_NK);
                    if (gvs_draw(x->gpu, rs_, r, zscale)) { g.draws++; g.tris += x->gpu->nidx / 3; }
                    return;
                }
                IDirect3DDevice9_SetVertexDeclaration(g.dev, g.decl_rc);
                IDirect3DDevice9_SetVertexShader(g.dev, g.vs_rc);
                IDirect3DDevice9_SetPixelShader(g.dev, ps);
                IDirect3DDevice9_SetPixelShaderConstantF(g.dev, 0, &K[0][0], RC_NK);
                IDirect3DDevice9_SetStreamSource(g.dev, 0, g.vb, (UINT)voff, (UINT)sizeof(Nv2aVertex));
                IDirect3DDevice9_SetStreamSource(g.dev, 1, g.xvb, (UINT)xoff, (UINT)sizeof(Nv2aVertexExt));
                {   HRESULT dh = IDirect3DDevice9_DrawPrimitive(g.dev, D3DPT_TRIANGLELIST, 0, count / 3);
                    if (g.log) { static int nd; if (nd++ < 8) logf_("DrawPrimitive rc hr=0x%08lX voff %lld xoff %lld count %u\n", (unsigned long)dh, (long long)voff, (long long)xoff, count); }
                }
                g.draws++;
                g.tris += count / 3;
                return;
            }
        }
        /* no shader or no buffer: fall through to the stage-0 draw */
    }
    if (gpu) return;                                     /* no CPU vertices to fall back on */
    {
        IDirect3DTexture9 *t = NULL;
        int self = 0;
        float K[4] = { 0, 0, 0, 0 };
        if (b->texture) t = tex_for(b->texture, r, &self);
        if (t) {
            K[0] = 1.0f / (float)b->texture->width; K[1] = 1.0f / (float)b->texture->height;
            settex(0, t);
            apply_sampler(0, b->texture);
        }
        K[2] = (float)(rs_->alpha_ref & 0xFF);
        if (getenv("RECOMP_D3D9_FF")) {          /* diagnostic: fixed function, pre-transformed vertices */
            HRESULT dh;
            IDirect3DDevice9_SetVertexShader(g.dev, NULL);
            IDirect3DDevice9_SetPixelShader(g.dev, NULL);
            IDirect3DDevice9_SetVertexDeclaration(g.dev, NULL);
            IDirect3DDevice9_SetFVF(g.dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            { HRESULT h3 = IDirect3DDevice9_SetStreamSource(g.dev, 0, g.vb, (UINT)voff, (UINT)sizeof(Nv2aVertex));
              D3DVIEWPORT9 vq; HRESULT h4; vq.X = vq.Y = 0; vq.Width = r->w; vq.Height = r->h; vq.MinZ = 0.0f; vq.MaxZ = 1.0f; h4 = IDirect3DDevice9_SetViewport(g.dev, &vq);
              if (g.log) { static int nq; if (nq++ < 3) logf_("FF: SetStreamSource hr=0x%08lX SetViewport 0x%08lX vb %p size %u\n", (unsigned long)h3, (unsigned long)h4, (void *)g.vb, VB_BYTES); } }
            dh = IDirect3DDevice9_DrawPrimitive(g.dev, D3DPT_TRIANGLELIST, 0, count / 3);
            if (g.log && FAILED(dh)) { static int nu; if (nu++ < 2) {
                HRESULT hu = IDirect3DDevice9_DrawPrimitiveUP(g.dev, D3DPT_TRIANGLELIST, count / 3, b->vertices, (UINT)sizeof(Nv2aVertex));
                logf_("DrawPrimitiveUP hr=0x%08lX\n", (unsigned long)hu);
            } }
            if (g.log && FAILED(dh)) { static int nz; if (nz++ < 2) {
                IDirect3DSurface9 *rt0 = NULL, *ds0 = NULL; D3DSURFACE_DESC d1, d2; D3DVIEWPORT9 gv;
                memset(&d1, 0, sizeof d1); memset(&d2, 0, sizeof d2);
                IDirect3DDevice9_GetRenderTarget(g.dev, 0, &rt0);
                IDirect3DDevice9_GetDepthStencilSurface(g.dev, &ds0);
                IDirect3DDevice9_GetViewport(g.dev, &gv);
                if (rt0) { IDirect3DSurface9_GetDesc(rt0, &d1); IDirect3DSurface9_Release(rt0); }
                if (ds0) { IDirect3DSurface9_GetDesc(ds0, &d2); IDirect3DSurface9_Release(ds0); }
                logf_("FF fail: rt %ux%u fmt %d ms %d | ds %ux%u fmt %d ms %d | vp %lu,%lu %lux%lu\n", d1.Width, d1.Height, d1.Format, d1.MultiSampleType, d2.Width, d2.Height, d2.Format, d2.MultiSampleType,
                      (unsigned long)gv.X, (unsigned long)gv.Y, (unsigned long)gv.Width, (unsigned long)gv.Height);
            } }
            if (g.log) { static int nd; if (nd++ < 4) logf_("DrawPrimitive FF hr=0x%08lX\n", (unsigned long)dh); }
            return;
        }
        IDirect3DDevice9_SetVertexDeclaration(g.dev, g.decl_s);
        IDirect3DDevice9_SetVertexShader(g.dev, g.vs_s);
        IDirect3DDevice9_SetPixelShader(g.dev, get_ps_simple(t != NULL, atest, afunc));
        IDirect3DDevice9_SetPixelShaderConstantF(g.dev, 0, K, 1);
        IDirect3DDevice9_SetStreamSource(g.dev, 0, g.vb, (UINT)voff, (UINT)sizeof(Nv2aVertex));
        IDirect3DDevice9_SetStreamSource(g.dev, 1, NULL, 0, 0);
        {   HRESULT dh = IDirect3DDevice9_DrawPrimitive(g.dev, D3DPT_TRIANGLELIST, 0, count / 3);
            if (g.log) { static int nd; if (nd++ < 8) logf_("DrawPrimitive simple hr=0x%08lX voff %lld count %u textured %d\n", (unsigned long)dh, (long long)voff, count, t != NULL); }
        }
        g.draws++;
        g.tris += count / 3;
    }
}

uint64_t s_us_draw, s_us_fetch, s_us_draws_n;
static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    uint64_t t0 = qpc_us();
    be_draw_impl(s, b);
    s_us_draw += qpc_us() - t0;
    s_us_draws_n++;
}

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs_, uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    RenderTarget *r;
    DWORD f = 0;

    if (!init_window_and_device() || g.lost) return;
    r = get_rt(s);
    if (!r) return;
    r->uses++;
    if ((flags & 3u) && rs_ && rs_->zeta_va) ensure_depth(r, rs_);
    bind_rt(r, r->depth != NULL);
    if (flags & 0xF0u) f |= D3DCLEAR_TARGET;
    if (r->depth) {
        if (flags & 1u) f |= D3DCLEAR_ZBUFFER;
        if (flags & 2u) f |= D3DCLEAR_STENCIL;
    }
    if (!f) return;
    if (g.log) { static int nc; if (nc++ < 12) logf_("clear rt %08X flags %X argb %08X zst %08X\n", s->color_va, flags, argb, zstencil); }
    if (getenv("RECOMP_D3D9_REDCLEAR")) argb = 0xFF400000u;
    IDirect3DDevice9_Clear(g.dev, 0, NULL, f, (D3DCOLOR)argb, (zstencil >> 8) / 16777215.0f, zstencil & 0xFF);
}

/* Image blit of a whole render surface into a texture's address: keep a GPU copy as a render surface with the
 * destination's address, so a draw that samples that texture finds it instead of the never-written guest bytes. */
static void be_blit(uint32_t src_va, uint32_t dst_va, uint32_t w, uint32_t h)
{
    int i;
    RenderTarget *s = NULL, *d;
    Nv2aSurface fake;

    if (!g.dev || g.lost) return;
    for (i = 0; i < g.nrt; i++)
        if (g.rt[i].va == src_va && g.rt[i].w == w && g.rt[i].h == h) { s = &g.rt[i]; break; }
    if (!s) { logf_("blit 0x%08X -> 0x%08X %ux%u: no such source surface\n", src_va, dst_va, w, h); return; }
    memset(&fake, 0, sizeof fake);
    fake.color_va = dst_va; fake.width = w; fake.height = h; fake.pitch = w * 4;
    fake.bytes_per_pixel = 4; fake.aa_sx = fake.aa_sy = 1;
    d = get_rt(&fake);
    if (!d) return;
    s = NULL;
    for (i = 0; i < g.nrt; i++)
        if (g.rt[i].va == src_va && g.rt[i].w == w && g.rt[i].h == h) { s = &g.rt[i]; break; }
    if (!s || s == d) return;
    IDirect3DDevice9_StretchRect(g.dev, s->surf, NULL, d->surf, NULL, D3DTEXF_NONE);
    d->uses = 1;    /* keep it out of the eviction of unused entries */
    logf_("blit 0x%08X -> 0x%08X %ux%u flip %u\n", src_va, dst_va, w, h, g.flips);
}

/* ---- present ---- */
static int s_asp_busy;

/* RECOMP_FB_DUMP=<prefix> with RECOMP_FB_WINDOW_DUMP_EVERY=<n>: every n-th flip write the game image as <prefix>d3d_<k>.bmp */
static void dump_rt_bmp(RenderTarget *r, const char *path)
{
    IDirect3DSurface9 *sys = NULL;
    D3DLOCKED_RECT lr;
    FILE *f;
    uint32_t x, y, rb = (r->w * 3 + 3) & ~3u, isz = rb * r->h, v;
    uint8_t hdr[54] = { 'B', 'M' }, *row;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(g.dev, r->w, r->h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL))) return;
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(g.dev, r->surf, sys)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        f = fopen(path, "wb");
        row = (uint8_t *)malloc(rb);
        if (f && row) {
            v = 54 + isz; memcpy(hdr + 2, &v, 4);
            v = 54; memcpy(hdr + 10, &v, 4);
            v = 40; memcpy(hdr + 14, &v, 4);
            memcpy(hdr + 18, &r->w, 4); memcpy(hdr + 22, &r->h, 4);
            hdr[26] = 1; hdr[28] = 24; memcpy(hdr + 34, &isz, 4);
            fwrite(hdr, 1, 54, f);
            for (y = r->h; y-- > 0; ) {
                const uint32_t *px = (const uint32_t *)((const uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch);
                memset(row, 0, rb);
                for (x = 0; x < r->w; x++) { row[x * 3] = (uint8_t)px[x]; row[x * 3 + 1] = (uint8_t)(px[x] >> 8); row[x * 3 + 2] = (uint8_t)(px[x] >> 16); }
                fwrite(row, 1, rb, f);
            }
        }
        if (f) fclose(f);
        free(row);
        IDirect3DSurface9_UnlockRect(sys);
    }
    IDirect3DSurface9_Release(sys);
}

static void pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* Draw r's image into the window: 4:3 inside the client area (or filling it with F1 "Stretch image"). 0: not presented. */
static int present_rt(RenderTarget *r)
{
    RECT cr, src, dst;
    int cw, ch, vw, vh;
    IDirect3DSurface9 *bb = NULL;
    HRESULT hr;

    if (!r || !r->surf || g.lost) return 0;
    if (!GetClientRect(g.hwnd, &cr)) return 0;
    cw = cr.right - cr.left; ch = cr.bottom - cr.top;
    if (cw <= 0 || ch <= 0) return 0;                       /* minimised */
    if (cw > (int)g.pp.BackBufferWidth) cw = (int)g.pp.BackBufferWidth;
    if (ch > (int)g.pp.BackBufferHeight) ch = (int)g.pp.BackBufferHeight;
    vh = ch; vw = (ch * 4 + 1) / 3;                         /* largest 4:3 rectangle that fits */
    if (vw > cw) { vw = cw; vh = (cw * 3 + 2) / 4; }
    if (s_stretch) { vw = cw; vh = ch; }
    dst.left = (cw - vw) / 2; dst.top = (ch - vh) / 2; dst.right = dst.left + vw; dst.bottom = dst.top + vh;
    src.left = src.top = 0; src.right = cw; src.bottom = ch;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(g.dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return 0;
    if (vw < cw || vh < ch) IDirect3DDevice9_ColorFill(g.dev, bb, &src, D3DCOLOR_XRGB(0, 0, 0));
    IDirect3DDevice9_StretchRect(g.dev, r->surf, NULL, bb, &dst, D3DTEXF_LINEAR);
    IDirect3DSurface9_Release(bb);
    hr = IDirect3DDevice9_Present(g.dev, &src, NULL, NULL, NULL);
    if (hr == D3DERR_DEVICELOST) g.lost = 1;
    s_last = r;
    return SUCCEEDED(hr);
}

/* WM_SIZE (resize, maximise, F11): redraw the last image at the new size straight away. */
static void aspect_on_size(void)
{
    if (!g.dev || !s_last || s_asp_busy || g.lost) return;
    s_asp_busy = 1;
    present_rt(s_last);
    s_asp_busy = 0;
}

static void be_flip(void)
{
    RenderTarget *best = NULL;
    int i;

    if (!init_window_and_device()) return;
    gvs_pump();                                  /* shader objects for programs the worker has compiled */
    if (g.lost) {
        HRESULT hr = IDirect3DDevice9_TestCooperativeLevel(g.dev);
        if (hr == D3DERR_DEVICENOTRESET) dev_reset();
        else Sleep(10);
    } else if (g.reset_pending) {
        dev_reset();
    }
    for (i = 0; i < g.nrt; i++) {
        RenderTarget *r = &g.rt[i];
        if (!r->uses) continue;
        if (!best || (uint64_t)r->w * r->h > (uint64_t)best->w * best->h
            || ((uint64_t)r->w * r->h == (uint64_t)best->w * best->h && r->uses > best->uses))
            best = r;
    }
    s_last = NULL;
    if (g.in_scene) { IDirect3DDevice9_EndScene(g.dev); g.in_scene = 0; }
    if (best) {
        static const char *dp; static int dn = -1; static uint32_t dk;
        if (dn < 0) { dp = getenv("RECOMP_FB_DUMP"); dn = getenv("RECOMP_FB_WINDOW_DUMP_EVERY") ? atoi(getenv("RECOMP_FB_WINDOW_DUMP_EVERY")) : 0; }
        if (dp && dn > 0 && (g.flips % (uint32_t)dn) == 0) {
            char pth[640];
            snprintf(pth, sizeof pth, "%sd3d_%05u.bmp", dp, dk++);
            dump_rt_bmp(best, pth);
        }
        s_asp_busy = 1;
        present_rt(best);
        s_asp_busy = 0;
    }
    for (i = 0; i < g.nrt; i++) g.rt[i].uses = 0;
    g.flips++;
#ifndef NHL_RELEASE
    {   /* FPS in the title bar, once a second */
        static ULONGLONG t_last;
        static uint32_t f_last, d_last;
        ULONGLONG now = GetTickCount64();
        if (!t_last) { t_last = now; f_last = g.flips; d_last = g.draws; }
        else if (now - t_last >= 1000) {
            char tb[128];
            double secs = (double)(now - t_last) / 1000.0;
            snprintf(tb, sizeof tb, "NHL Hitz Pro - D3D9 | FPS: %.1f | draws/frame: %.0f", (g.flips - f_last) / secs,
                     (g.flips - f_last) ? (double)(g.draws - d_last) / (g.flips - f_last) : 0.0);
            SetWindowTextA(g.hwnd, tb);
            t_last = now; f_last = g.flips; d_last = g.draws;
        }
    }
#endif
    pump_messages();
    quit_tick();
    /* Paused: the flip does not return, so the game stops here until the overlay closes. */
    pad_unblock_tick();
    pad_open_poll();
    while (s_q_active) { Sleep(8); pump_messages(); quit_tick(); pad_overlay_poll(); }
    if (g.log && (g.flips % 300) == 0)
        logf_("flips=%u t=%.1fs draws=%u tris=%u textures=%d specs=%u specfail=%u gpuvs: draws=%u verts=%u programs=%u failed=%u\n", g.flips,
              (GetTickCount() - g.t0) / 1000.0, g.draws, g.tris, s_ntex, s_nspec, s_spec_fail, s_gdraws, s_gverts, s_ngspec, s_gspec_fail);
    if (g.log && (g.flips % 300) == 0)
        logf_("time in be_draw %llu ms of which attribute decode %llu ms (draws %llu)\n", (unsigned long long)(s_us_draw / 1000), (unsigned long long)(s_us_fetch / 1000), (unsigned long long)s_us_draws_n);
}

static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip };


/* ---- the graphics thread ----
 * A single-producer single-consumer ring of records. The executor thread parses the push buffer and copies
 * everything a draw reads (state, vertices or the GPU vertex block, combiner, texture description) into the
 * ring; the graphics thread replays the records through be_clear/be_draw/be_flip/be_blit unchanged. Textures
 * are still read from guest memory at replay time, which is safe because a flip waits for the ring to drain:
 * the guest never gets to rewrite a texture while a frame that uses it is still queued. */
enum { Q_PAD, Q_CLEAR, Q_DRAW, Q_FLIP, Q_BLIT };
#define Q_BYTES (24u << 20)
typedef struct { uint32_t type, size; } QHdr;
typedef struct { QHdr h; Nv2aSurface s; Nv2aRenderState rs; uint32_t flags, argb, zst; } QClear;
typedef struct { QHdr h; uint32_t a, b, c, d; } QBlit;
typedef struct {
    QHdr h; Nv2aSurface s; Nv2aRenderState rs; Nv2aTexture tex; Nv2aBatchExt x; Nv2aCombiner rc; Nv2aGpuVs gv;
    uint32_t count, has_tex, has_x, has_rc, has_gv, nverts, next, block_bytes;
} QDraw;

static uint8_t *q_buf;
static volatile LONG64 q_head, q_tail;          /* bytes since start; head is the producer's, tail the consumer's */
static volatile LONG q_cons_sleep, q_prod_sleep;
static volatile LONG q_flips_posted, q_flips_done;
static HANDLE q_ev_data, q_ev_space, q_ev_done;

static size_t q_al(size_t n) { return (n + 15u) & ~(size_t)15u; }

/* A sleeping consumer is woken when a worthwhile amount is queued (a syscall per record cost the executor
 * a tenth of its time), and always at the end of a frame or when the ring is full; it also wakes by itself
 * after a millisecond. */
static void q_wake_consumer(int force)
{
    if (q_cons_sleep && (force || q_head - q_tail >= (64 << 10))) SetEvent(q_ev_data);
}

/* Room for n bytes at the producer's head, without wrapping inside the record. */
static uint8_t *q_reserve(size_t n)
{
    for (;;) {
        LONG64 head = q_head, tail = q_tail;
        size_t off = (size_t)(head % Q_BYTES);
        size_t pad = (off + n > Q_BYTES) ? Q_BYTES - off : 0;
        if ((size_t)(Q_BYTES - (head - tail)) >= pad + n) {
            if (pad) {
                QHdr *p = (QHdr *)(q_buf + off);
                p->type = Q_PAD; p->size = (uint32_t)pad;
                InterlockedExchange64(&q_head, head + (LONG64)pad);
                q_wake_consumer(1);
                off = 0;
            }
            return q_buf + off;
        }
        q_wake_consumer(1);
        InterlockedExchange(&q_prod_sleep, 1);
        if ((size_t)(Q_BYTES - (q_head - q_tail)) < pad + n)
            WaitForSingleObject(q_ev_space, 1);
        InterlockedExchange(&q_prod_sleep, 0);
    }
}

static void q_commit(size_t n, int force)
{
    InterlockedExchange64(&q_head, q_head + (LONG64)n);
    q_wake_consumer(force);
}

static void ab_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags, uint32_t argb, uint32_t zst)
{
    QClear *r = (QClear *)q_reserve(q_al(sizeof *r));
    r->h.type = Q_CLEAR; r->h.size = (uint32_t)q_al(sizeof *r);
    r->s = *s; r->rs = *rs; r->flags = flags; r->argb = argb; r->zst = zst;
    q_commit(r->h.size, 0);
}

static void ab_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aBatchExt *x = (nv2a_pbx_current)();
    int gpu = x && x->gpu && x->gpu->block;
    size_t vb = 0, eb = 0, bb = 0, total;
    QDraw *r;
    uint8_t *p;

    if (!b || !b->count || !b->state) return;
    if (!gpu && !b->vertices) return;
    if (!gpu) {
        vb = q_al((size_t)b->count * sizeof(Nv2aVertex));
        if (x && x->ext) eb = q_al((size_t)b->count * sizeof(Nv2aVertexExt));
    } else
        bb = q_al(x->gpu->block_bytes);
    total = q_al(sizeof *r) + vb + eb + bb;
    r = (QDraw *)q_reserve(total);
    p = (uint8_t *)r + q_al(sizeof *r);
    r->h.type = Q_DRAW; r->h.size = (uint32_t)total;
    r->s = *s; r->rs = *b->state; r->count = b->count;
    r->has_tex = b->texture != NULL;
    if (r->has_tex) r->tex = *b->texture;
    r->has_x = x != NULL; r->has_rc = 0; r->has_gv = gpu; r->nverts = (uint32_t)(vb != 0); r->next = (uint32_t)(eb != 0);
    r->block_bytes = 0;
    if (x) {
        r->x = *x;
        if (x->rc) { r->rc = *x->rc; r->has_rc = 1; }
    }
    if (vb) { memcpy(p, b->vertices, (size_t)b->count * sizeof(Nv2aVertex)); p += vb; }
    if (eb) { memcpy(p, x->ext, (size_t)b->count * sizeof(Nv2aVertexExt)); p += eb; }
    if (gpu) {   /* the program text stays behind: the probe already made the shader, and the replay finds it by hash */
        const Nv2aGpuVs *g0 = x->gpu;
        r->gv.nslots = 0; r->gv.hash = g0->hash; r->gv.input_mask = g0->input_mask; r->gv.nidx = g0->nidx;
        r->gv.aa_sx = g0->aa_sx; r->gv.aa_sy = g0->aa_sy; r->gv.fog_mode = g0->fog_mode;
        r->gv.fog_px = g0->fog_px; r->gv.fog_py = g0->fog_py; r->gv.block_bytes = g0->block_bytes;
        r->block_bytes = g0->block_bytes;
        memcpy(p, g0->block, g0->block_bytes);
    }
    q_commit(total, 0);
}

static void ab_flip(void)
{
    QHdr *h = (QHdr *)q_reserve(16);
    LONG want = InterlockedIncrement(&q_flips_posted);
    h->type = Q_FLIP; h->size = 16;
    q_commit(16, 1);
    while ((LONG)(q_flips_done - want) < 0)
        WaitForSingleObject(q_ev_done, 1);
}

static void ab_blit(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    QBlit *r = (QBlit *)q_reserve(q_al(sizeof *r));
    r->h.type = Q_BLIT; r->h.size = (uint32_t)q_al(sizeof *r);
    r->a = a; r->b = b; r->c = c; r->d = d;
    q_commit(r->h.size, 1);
}

static void be_blit(uint32_t src_va, uint32_t dst_va, uint32_t w, uint32_t h);

static void q_replay_draw(QDraw *r)
{
    Nv2aBatch b;
    uint8_t *p = (uint8_t *)r + q_al(sizeof *r);

    memset(&b, 0, sizeof b);
    b.count = r->count; b.state = &r->rs; b.texture = r->has_tex ? &r->tex : NULL;
    if (r->nverts) { b.vertices = (const Nv2aVertex *)p; p += q_al((size_t)r->count * sizeof(Nv2aVertex)); }
    if (r->has_x) {
        r->x.ext = NULL; r->x.rc = r->has_rc ? &r->rc : NULL; r->x.gpu = NULL;
        if (r->next) { r->x.ext = (const Nv2aVertexExt *)p; p += q_al((size_t)r->count * sizeof(Nv2aVertexExt)); }
        if (r->has_gv) { r->gv.block = p; r->gv.verify_out = NULL; r->gv.verify = 0; r->x.gpu = &r->gv; }
        s_wx = &r->x;
    } else
        s_wx = NULL;
    be_draw(&r->s, &b);
    s_wx = NULL;
}

static DWORD WINAPI gfx_thread_main(LPVOID unused)
{
    int spin = 0;
    (void)unused;
    init_window_and_device();
    MemoryBarrier();
    InterlockedExchange(&s_dev_ready, 1);
    for (;;) {
        LONG64 tail = q_tail, head = q_head;
        QHdr *h;
        if (tail == head) {
            if (++spin < 20000) { YieldProcessor(); continue; }
            InterlockedExchange(&q_cons_sleep, 1);
            if (q_head == q_tail) WaitForSingleObject(q_ev_data, 1);
            InterlockedExchange(&q_cons_sleep, 0);
            spin = 0;
            continue;
        }
        spin = 0;
        h = (QHdr *)(q_buf + (size_t)(tail % Q_BYTES));
        switch (h->type) {
        case Q_CLEAR: { QClear *c = (QClear *)h; be_clear(&c->s, &c->rs, c->flags, c->argb, c->zst); } break;
        case Q_DRAW:  q_replay_draw((QDraw *)h); break;
        case Q_BLIT:  { QBlit *c = (QBlit *)h; be_blit(c->a, c->b, c->c, c->d); } break;
        case Q_FLIP:
            be_flip();
            InterlockedIncrement(&q_flips_done);
            SetEvent(q_ev_done);
            break;
        default: break;
        }
        InterlockedExchange64(&q_tail, tail + (LONG64)h->size);
        if (q_prod_sleep) SetEvent(q_ev_space);
    }
    return 0;
}

static const Nv2aBackend s_backend_async = { ab_clear, ab_draw, ab_flip };

static int gfx_thread_start(void)
{
    const char *e = getenv("RECOMP_GFX_THREAD");
    if ((e && e[0] == '0') || getenv("RECOMP_GPUVS_VERIFY")) return 0;
    q_buf = (uint8_t *)VirtualAlloc(NULL, Q_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    q_ev_data = CreateEventA(NULL, FALSE, FALSE, NULL);
    q_ev_space = CreateEventA(NULL, FALSE, FALSE, NULL);
    q_ev_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!q_buf || !q_ev_data || !q_ev_space || !q_ev_done) return 0;
    s_async = 1;
    if (!CreateThread(NULL, 0, gfx_thread_main, NULL, 0, NULL)) { s_async = 0; return 0; }
    return 1;
}


/* Called from main.c when RECOMP_D3D11=1 (the D3D9 build keeps the entry point's name so main.c is shared). */
void backend_d3d11_register(void)
{
    if (gfx_thread_start()) {
        nv2a_backend_register(&s_backend_async);
        nv2a_blit_register(ab_blit);
        fprintf(stderr, "[D3D9] back end registered (graphics thread)\n");
        return;
    }
    nv2a_backend_register(&s_backend);
    nv2a_blit_register(be_blit);
    fprintf(stderr, "[D3D9] back end registered (software rasteriser off)\n");
}
