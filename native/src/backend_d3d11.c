/*
 * Direct3D 11 render back end for the NV2A executor (nv2a_backend.h).
 *
 * Project code, not toolkit. Registered from main.c only when RECOMP_D3D11=1;
 * the default stays the executor's own software rasteriser.
 *
 * Everything here runs on the NV2A poll thread, so the window and the device
 * are created lazily on the first callback and window messages are pumped in
 * flip().
 *
 * Environment (all optional):
 *   RECOMP_D3D11_VSYNC=1       present with vsync (default: no vsync, to measure)
 *   RECOMP_D3D11_LOG=1         log every distinct render surface and state seen
 *   RECOMP_FB_DUMP=<prefix>    with RECOMP_FB_WINDOW_DUMP_EVERY=<n>: dump the
 *                              presented image as <prefix>d3d_<k>.bmp
 *
 * Render targets: a title renders into several surfaces (two display buffers,
 * offscreen targets). Each distinct colour address gets its own D3D11 texture;
 * flip() presents the largest one used since the previous flip.
 */
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>      /* types only (RECOMP_FLIPTIME_PROF); dbghelp.dll is loaded at run time */
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <pdh.h>          /* title-bar GPU %: types only; pdh.dll is loaded at run time */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"
#include "nv2a_pbx.h"        /* project: combiner state for batches that have it */

/* keyboard.h (toolkit input library): keys reach the title through these. */
extern void xbox_InputKeySet(int vk, int down);
extern void xbox_InputKeysClear(void);

#define MAX_RT 16

typedef struct {
    uint32_t va, w, h, zeta_va;
    ID3D11Texture2D        *tex;
    ID3D11RenderTargetView *rtv;
    uint32_t uses;               /* clears + draws since the last flip */
    ID3D11Texture2D        *dtex;    /* depth/stencil, created on first use */
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *srv;   /* when sampled as a texture */
} RenderTarget;

/* Small cache of D3D11 state objects keyed by a memcmp-able struct. */
#define MAX_STATES 256
typedef struct {
    int n;
    struct { uint8_t key[48]; void *obj; } e[MAX_STATES];
} StateCache;

static struct {
    int inited, failed;
    HWND hwnd;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain *swap;
    ID3D11Texture2D *back;
    uint32_t swap_w, swap_h;
    RenderTarget rt[MAX_RT];
    int nrt;
    int vsync, log;
    uint32_t last_clear;
    ID3D11VertexShader *vs;
    ID3D11PixelShader  *ps;
    ID3D11InputLayout  *layout;
    ID3D11Buffer       *vb, *cb;
    uint32_t vb_off;
    /* register-combiner path (d3d11_rc.hlsl): opt-in: RECOMP_D3D11_RC=1 turns it on */
    ID3D11VertexShader *vs_rc;
    ID3D11PixelShader  *ps_rc;
    ID3D11InputLayout  *layout_rc;
    ID3D11Buffer       *xvb, *cb_rc;
    ID3D11ShaderResourceView *white;
    uint32_t xvb_off, rc_draws;
    int rc_ok;
    StateCache blend, depth, raster;
    uint32_t draws, tris, zlog;
    uint32_t flips;
    int      hash_every_use;
    DWORD t0;
    const char *dump_prefix;
    uint32_t dump_every, dump_n;
} g;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[D3D11] ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

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

static void quit_draw(void);
static void quit_close(void);
extern void xbox_time_pause_begin(void);   /* kernel_hal.c override */
extern void xbox_time_pause_end(void);
extern void xa2_set_muted(int muted);      /* apu_xaudio2.c override */

static LRESULT CALLBACK quit_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
        int i;
        for (i = 0; i < 2; i++)
            if (PtInRect(&s_q_btn[i], pt) && s_q_sel != i) { s_q_sel = i; quit_draw(); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
        if (PtInRect(&s_q_btn[1], pt)) TerminateProcess(GetCurrentProcess(), 0);
        if (PtInRect(&s_q_btn[0], pt)) quit_close();
        return 0;
    }
    }
    return DefWindowProcA(h, m, w, l);
}

static void quit_draw(void)
{
    RECT cr;
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
    ft = CreateFontA(-(dh * 17 / 100), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Impact");
    fb = CreateFontA(-(dh * 15 / 100), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Impact");
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
        s_q_hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, "",
                                   WS_POPUP, 0, 0, 1, 1, g.hwnd, NULL, wc.hInstance, NULL);
        if (!s_q_hwnd) return;
    }
    xbox_InputKeysClear();            /* nothing the game saw as held stays held */
    s_q_sel = 0; s_q_active = 1;
    xbox_time_pause_begin();          /* the game's clocks stop; present() holds below */
    xa2_set_muted(1);
    quit_draw();
    ShowWindow(s_q_hwnd, SW_SHOWNOACTIVATE);
}

static void quit_close(void)
{
    if (s_q_active) { xa2_set_muted(0); xbox_time_pause_end(); }
    s_q_active = 0;
    if (s_q_hwnd) ShowWindow(s_q_hwnd, SW_HIDE);
}

/* Per frame: follow the game window when it moves or is resized. */
static void quit_tick(void)
{
    RECT cr;
    POINT o = { 0, 0 };
    if (!s_q_active || !g.hwnd) return;
    if (IsIconic(g.hwnd)) return;
    GetClientRect(g.hwnd, &cr);
    ClientToScreen(g.hwnd, &o);
    if (cr.right != s_q_cw || cr.bottom != s_q_ch || o.x != s_q_ox || o.y != s_q_oy) quit_draw();
}

/* A key message for the game window; 1 when the overlay took it. */
static int quit_key(WPARAM w, LPARAM l, int down)
{
    int repeat = (l & (1L << 30)) != 0;
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
        if (quit_key(w, l, 1)) return 0;   /* ESC quit overlay */
        if (!(l & (1L << 30)))          /* not an auto-repeat */
            keylog((int)w, 1);
        xbox_InputKeySet((int)w, 1);
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (w == VK_F11 && f11_on()) return 0;
        if (w == VK_RETURN && m == WM_SYSKEYUP && f11_on()) return 0;   /* the Alt+Enter release */
        if (quit_key(w, l, 0)) return 0;
        keylog((int)w, 0);
        xbox_InputKeySet((int)w, 0);
        return 0;
    case WM_SETCURSOR:              /* borderless fullscreen: no mouse pointer over the game */
        if (s_fs && !s_q_active && LOWORD(l) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

static int init_pipeline(void);

static int init_window_and_device(void)
{
    WNDCLASSA wc;
    D3D_FEATURE_LEVEL fl, want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                     D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr;
    UINT flags = 0;

    if (g.inited) return !g.failed;
    g.inited = 1;
    g.vsync = getenv("RECOMP_D3D11_VSYNC") && getenv("RECOMP_D3D11_VSYNC")[0] == '1';
    g.log   = getenv("RECOMP_D3D11_LOG") != NULL;
    g.hash_every_use = getenv("RECOMP_D3D11_HASH_EVERY_USE") != NULL;
    g.dump_prefix = getenv("RECOMP_FB_DUMP");
    if (getenv("RECOMP_FB_WINDOW_DUMP_EVERY"))
        g.dump_every = (uint32_t)atoi(getenv("RECOMP_FB_WINDOW_DUMP_EVERY"));
    g.t0 = GetTickCount();

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));   /* resources/nhl_hitz_pro.rc */
    wc.lpszClassName = "NhlD3D11Backend";
    RegisterClassA(&wc);
    g.hwnd = CreateWindowExA(0, wc.lpszClassName, 
#ifdef NHL_RELEASE
        "NHL Hitz Pro",
#else
        "NHL Hitz Pro - D3D11 back end",
#endif

                             WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 656, 519,
                             NULL, NULL, wc.hInstance, NULL);
    if (!g.hwnd) { logf_("CreateWindow failed (%lu)\n", GetLastError()); g.failed = 1; return 0; }
    if (f11_on() && want_fullscreen_at_start()) toggle_fullscreen(g.hwnd);

    if (getenv("RECOMP_D3D11_DEBUG")) flags |= D3D11_CREATE_DEVICE_DEBUG;
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, want, 3,
                           D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx);
    if (FAILED(hr)) {
        logf_("D3D11CreateDevice failed 0x%08lX\n", (unsigned long)hr);
        g.failed = 1;
        return 0;
    }
    logf_("device up, feature level 0x%X\n", (unsigned)fl);
    if (!init_pipeline()) { g.failed = 1; return 0; }
    return 1;
}

/* (Re)create the swap chain for a w x h presentation. */
static int s_fs;                    /* borderless fullscreen is on (defined with toggle_fullscreen) */

/* The window's DPI as a percentage of 96 (the process is per-monitor DPI aware). */
static int dpi_pct(HWND h)
{
    typedef UINT (WINAPI *get_dpi_fn)(HWND);
    HMODULE u32 = GetModuleHandleA("user32.dll");
    get_dpi_fn f = u32 ? (get_dpi_fn)(void *)GetProcAddress(u32, "GetDpiForWindow") : NULL;
    UINT d = f ? f(h) : 96;
    return d ? (int)(d * 100 / 96) : 100;
}

static int ensure_swap(uint32_t w, uint32_t h)
{
    HRESULT hr;

    if (g.swap && g.swap_w == w && g.swap_h == h) return 1;
    if (g.swap) {
        int pct = aspect_on() ? dpi_pct(g.hwnd) : 100;
        RECT r = { 0, 0, (LONG)(w * pct / 100), (LONG)(h * pct / 100) };
        if (g.back) { ID3D11Texture2D_Release(g.back); g.back = NULL; }
        hr = IDXGISwapChain_ResizeBuffers(g.swap, 0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) { logf_("ResizeBuffers 0x%08lX\n", (unsigned long)hr); return 0; }
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        if (!s_fs)                  /* fullscreen keeps the monitor-sized window */
            SetWindowPos(g.hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                         SWP_NOMOVE | SWP_NOZORDER);
    } else {
        IDXGIDevice *dxd = NULL;
        IDXGIAdapter *ad = NULL;
        IDXGIFactory *fac = NULL;
        DXGI_SWAP_CHAIN_DESC sd;
        int pct = aspect_on() ? dpi_pct(g.hwnd) : 100;
        RECT r = { 0, 0, (LONG)(w * pct / 100), (LONG)(h * pct / 100) };

        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        if (!s_fs)                  /* fullscreen keeps the monitor-sized window */
            SetWindowPos(g.hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                         SWP_NOMOVE | SWP_NOZORDER);
        memset(&sd, 0, sizeof sd);
        sd.BufferDesc.Width = w;
        sd.BufferDesc.Height = h;
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 1;
        sd.OutputWindow = g.hwnd;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = ID3D11Device_QueryInterface(g.dev, &IID_IDXGIDevice, (void **)&dxd);
        if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dxd, &ad);
        if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&fac);
        if (SUCCEEDED(hr)) hr = IDXGIFactory_CreateSwapChain(fac, (IUnknown *)g.dev, &sd, &g.swap);
        if (SUCCEEDED(hr)) IDXGIFactory_MakeWindowAssociation(fac, g.hwnd, DXGI_MWA_NO_ALT_ENTER);
        if (fac) IDXGIFactory_Release(fac);
        if (ad) IDXGIAdapter_Release(ad);
        if (dxd) IDXGIDevice_Release(dxd);
        if (FAILED(hr)) { logf_("CreateSwapChain 0x%08lX\n", (unsigned long)hr); g.failed = 1; return 0; }
    }
    hr = IDXGISwapChain_GetBuffer(g.swap, 0, &IID_ID3D11Texture2D, (void **)&g.back);
    if (FAILED(hr)) { logf_("GetBuffer 0x%08lX\n", (unsigned long)hr); g.failed = 1; return 0; }
    g.swap_w = w;
    g.swap_h = h;
    logf_("swap chain %ux%u\n", w, h);
    return 1;
}

static RenderTarget *get_rt(const Nv2aSurface *s)
{
    RenderTarget *r;
    int i;
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    if (s->bytes_per_pixel != 4) return NULL;      /* 16-bit surfaces: not handled yet */
    for (i = 0; i < g.nrt; i++)
        if (g.rt[i].va == s->color_va && g.rt[i].w == s->width && g.rt[i].h == s->height)
            return &g.rt[i];
    if (g.nrt >= MAX_RT) {
        /* Drop the least used entry. */
        int worst = 0;
        for (i = 1; i < g.nrt; i++) if (g.rt[i].uses < g.rt[worst].uses) worst = i;
        ID3D11RenderTargetView_Release(g.rt[worst].rtv);
        ID3D11Texture2D_Release(g.rt[worst].tex);
        if (g.rt[worst].srv) ID3D11ShaderResourceView_Release(g.rt[worst].srv);
        if (g.rt[worst].dsv) ID3D11DepthStencilView_Release(g.rt[worst].dsv);
        if (g.rt[worst].dtex) ID3D11Texture2D_Release(g.rt[worst].dtex);
        g.rt[worst] = g.rt[--g.nrt];
    }
    r = &g.rt[g.nrt];
    memset(r, 0, sizeof *r);
    memset(&td, 0, sizeof td);
    td.Width = s->width;
    td.Height = s->height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(g.dev, &td, NULL, &r->tex);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateRenderTargetView(g.dev, (ID3D11Resource *)r->tex, NULL, &r->rtv);
    if (FAILED(hr)) {
        logf_("render target %ux%u failed 0x%08lX\n", s->width, s->height, (unsigned long)hr);
        if (r->tex) ID3D11Texture2D_Release(r->tex);
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

/* ---- shaders, buffers and state objects ----------------------------------
 *
 * One vertex format, taken straight from Nv2aVertex (28 bytes). The vertex
 * shader turns surface pixels into clip space and puts w = 1/rhw back so the
 * rasteriser interpolates colour and texel UVs perspective-correct.
 */
#define VB_BYTES (8u * 1024u * 1024u)

typedef struct {
    float    surf_w, surf_h, zscale, aref;     /* aref 0..255 */
    uint32_t atest, afunc, textured, pad0;
    float    inv_w, inv_h, pad1, pad2;         /* 1/texture size: UVs arrive in texels */
} DrawConsts;

static const char k_hlsl[] =
    "cbuffer C : register(b0) { float2 surf; float zscale; float aref;\n"
    "                           uint atest; uint afunc; uint textured; uint pad0;\n"
    "                           float2 isz; float2 pad1; };\n"
    "Texture2D tex0 : register(t0);\n"
    "SamplerState smp0 : register(s0);\n"
    "struct VI { float3 p : POSITION; float rhw : RHW; float4 c : COLOR;\n"
    "            float2 uv : TEXCOORD0; };\n"
    "struct VO { float4 pos : SV_Position; float4 c : COLOR; float2 uv : TEXCOORD0; };\n"
    "VO vs_main(VI i) {\n"
    "  VO o;\n"
    "  float w = (i.rhw > 1e-12) ? 1.0 / i.rhw : 1.0;\n"
    "  float cx = i.p.x / surf.x * 2.0 - 1.0;\n"
    "  float cy = 1.0 - i.p.y / surf.y * 2.0;\n"
    "  float cz = saturate(i.p.z * zscale);\n"
    "  o.pos = float4(cx * w, cy * w, cz * w, w);\n"
    "  o.c = i.c; o.uv = i.uv;\n"
    "  return o;\n"
    "}\n"
    "float4 ps_main(VO i) : SV_Target {\n"
    "  float4 c = i.c;\n"
    "  if (textured) c *= tex0.Sample(smp0, i.uv * isz);\n"
    "  if (atest) {\n"
    "    int a = (int)(c.a * 255.0 + 0.5); int r = (int)(aref + 0.5);\n"
    "    bool ok = true;\n"
    "    if (afunc == 0) ok = false;\n"
    "    else if (afunc == 1) ok = a < r;\n"
    "    else if (afunc == 2) ok = a == r;\n"
    "    else if (afunc == 3) ok = a <= r;\n"
    "    else if (afunc == 4) ok = a > r;\n"
    "    else if (afunc == 5) ok = a != r;\n"
    "    else if (afunc == 6) ok = a >= r;\n"
    "    if (!ok) discard;\n"
    "  }\n"
    "  return c;\n"
    "}\n";

static uint32_t s_cmp_n, s_cmp_vs_n;       /* RECOMP_FLIPTIME: compiles, of which vertex shaders */
static double   s_cmp_ms, s_cmp_vs_ms;

/* RECOMP_D3D11_SHADERCACHE=1: on-disk cache of compiled shader bytecode. One file per
 * (source, entry, target, compiler settings) in <exe dir>\shadercache (or
 * RECOMP_D3D11_SHADERCACHE_DIR). A file is: magic, format, two independent 64-bit hashes of
 * the key text, source length, blob size, blob checksum, blob. Anything that does not match
 * exactly is ignored and recompiled; files are written to a temp name and renamed. */
#define SC_MAGIC  0x31434853u            /* "SHC1" */
#define SC_FORMAT 3u                     /* bump when the file layout or compile flags change */
typedef struct { uint32_t magic, format; uint64_t h1, h2; uint32_t srclen, size; uint64_t sum; } ScHdr;

static uint32_t s_sc_hit, s_sc_miss, s_sc_store;
static double   s_sc_ms;

static uint64_t sc_hash(const void *p, size_t n, uint64_t h)
{
    const unsigned char *s = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) { h ^= s[i]; h *= 1099511628211ull; }
    return h;
}

static int sc_enabled(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_D3D11_SHADERCACHE") != NULL;
    return on;
}

static const char *sc_dir(void)
{
    static char dir[MAX_PATH];
    static int done;
    if (!done) {
        const char *e = getenv("RECOMP_D3D11_SHADERCACHE_DIR");
        if (e && e[0])
            snprintf(dir, sizeof dir, "%s", e);
        else {
            char *sl;
            GetModuleFileNameA(NULL, dir, sizeof dir);
            sl = strrchr(dir, '\\');
            if (sl) *sl = 0;
            strncat(dir, "\\shadercache", sizeof dir - strlen(dir) - 1);
        }
        CreateDirectoryA(dir, NULL);
        done = 1;
    }
    return dir;
}

static void sc_key(const char *src, size_t len, const char *entry, const char *target,
                   uint64_t *h1, uint64_t *h2, char *path, size_t pathcap)
{
    char tag[96];
    int tl = snprintf(tag, sizeof tag, "|%s|%s|opt3|d3dcompiler%d|f%u", entry, target,
                      (int)D3D_COMPILER_VERSION, SC_FORMAT);
    *h1 = sc_hash(tag, (size_t)tl, sc_hash(src, len, 14695981039346656037ull));
    *h2 = sc_hash(src, len, sc_hash(tag, (size_t)tl, 0x9E3779B97F4A7C15ull));
    snprintf(path, pathcap, "%s\\%s_%016llx.bin", sc_dir(), target, (unsigned long long)*h1);
}

static ID3DBlob *sc_load(const char *path, uint64_t h1, uint64_t h2, size_t srclen)
{
    FILE *f = fopen(path, "rb");
    ScHdr h;
    ID3DBlob *blob = NULL;

    if (!f) return NULL;
    if (fread(&h, sizeof h, 1, f) == 1 && h.magic == SC_MAGIC && h.format == SC_FORMAT
        && h.h1 == h1 && h.h2 == h2 && h.srclen == (uint32_t)srclen && h.size && h.size < (64u << 20)
        && SUCCEEDED(D3DCreateBlob(h.size, &blob))) {
        void *p = ID3D10Blob_GetBufferPointer(blob);
        if (fread(p, 1, h.size, f) != h.size || sc_hash(p, h.size, 14695981039346656037ull) != h.sum) {
            ID3D10Blob_Release(blob);
            blob = NULL;
        }
    }
    fclose(f);
    return blob;
}

static void sc_store(const char *path, uint64_t h1, uint64_t h2, size_t srclen, ID3DBlob *blob)
{
    static volatile LONG seq;
    char tmp[MAX_PATH + 32];
    ScHdr h;
    FILE *f;
    size_t n = ID3D10Blob_GetBufferSize(blob);
    const void *p = ID3D10Blob_GetBufferPointer(blob);

    snprintf(tmp, sizeof tmp, "%s.%lu.%ld.tmp", path, GetCurrentThreadId(), (long)InterlockedIncrement(&seq));
    f = fopen(tmp, "wb");
    if (!f) return;
    h.magic = SC_MAGIC; h.format = SC_FORMAT; h.h1 = h1; h.h2 = h2;
    h.srclen = (uint32_t)srclen; h.size = (uint32_t)n;
    h.sum = sc_hash(p, n, 14695981039346656037ull);
    if (fwrite(&h, sizeof h, 1, f) == 1 && fwrite(p, 1, n, f) == n && fclose(f) == 0) {
        if (MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
            InterlockedIncrement((volatile LONG *)&s_sc_store);
        else
            DeleteFileA(tmp);
    } else {
        fclose(f);
        DeleteFileA(tmp);
    }
}

static ID3DBlob *compile_hlsl_src(const char *src, size_t len, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    LARGE_INTEGER q0, q1, qf;
    HRESULT hr;
    uint64_t k1 = 0, k2 = 0;
    char cpath[MAX_PATH + 64];
    double ms;
    int use_cache = sc_enabled();

    QueryPerformanceCounter(&q0);
    QueryPerformanceFrequency(&qf);
    if (use_cache) {
        sc_key(src, len, entry, target, &k1, &k2, cpath, sizeof cpath);
        code = sc_load(cpath, k1, k2, len);
        if (code) {
            QueryPerformanceCounter(&q1);
            ms = 1000.0 * (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
            InterlockedIncrement((volatile LONG *)&s_sc_hit);
            s_sc_ms += ms;
            s_cmp_n++;
            s_cmp_ms += ms;
            if (target[0] == 'v') { s_cmp_vs_n++; s_cmp_vs_ms += ms; }
            return code;
        }
        InterlockedIncrement((volatile LONG *)&s_sc_miss);
    }
    hr = D3DCompile(src, len, "d3d11_backend.hlsl", NULL, NULL,
                    entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    QueryPerformanceCounter(&q1);
    ms = 1000.0 * (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
    s_cmp_n++;
    s_cmp_ms += ms;
    if (target[0] == 'v') {
        s_cmp_vs_n++;
        s_cmp_vs_ms += ms;
    }
    if (FAILED(hr)) {
        logf_("D3DCompile %s failed 0x%08lX: %s\n", entry, (unsigned long)hr,
              err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        code = NULL;
    } else if (use_cache && code) {
        sc_store(cpath, k1, k2, len, code);
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static ID3DBlob *compile_hlsl(const char *entry, const char *target)
{
    return compile_hlsl_src(k_hlsl, sizeof k_hlsl - 1, entry, target);
}

/* ---- register-combiner path ---- */
#include "d3d11_rc_hlsl.h"

typedef struct {
    uint32_t hdr[4], hdr2[4];
    float    fogc[4];
    float    uvs[4][4];
    uint32_t stg[16][4];
} RcConsts;

#define XVB_BYTES ((VB_BYTES / (uint32_t)sizeof(Nv2aVertex)) * (uint32_t)sizeof(Nv2aVertexExt))

/* One pixel shader per distinct combiner program (the words become literals; see SPEC in the
   hlsl). The generic dynamic shader (g.ps_rc) is the fallback if a specialisation fails. */
typedef struct { uint64_t key; ID3D11PixelShader *ps; } RcSpec;
#define RC_SPEC_MAX 4096
static RcSpec s_spec[RC_SPEC_MAX];
static uint32_t s_nspec, s_spec_fail;
static uint64_t s_spec_ms;

static ID3D11PixelShader *rc_get_ps(const RcConsts *c)
{
    uint32_t n = c->hdr[0] & 255u, i;
    uint64_t h = 1469598103934665603ull;
    char *src;
    size_t cap, len;
    ID3DBlob *blob;
    ID3D11PixelShader *ps = NULL;
    DWORD t0;

    if (n > 8u) n = 8u;
    for (i = 0; i < 4; i++) { h ^= c->hdr[i]; h *= 1099511628211ull; }
    for (i = 0; i < n; i++) {
        uint32_t k;
        for (k = 0; k < 4; k++) { h ^= c->stg[2 * i][k]; h *= 1099511628211ull; }
    }
    for (i = 0; i < s_nspec; i++)
        if (s_spec[i].key == h) return s_spec[i].ps ? s_spec[i].ps : g.ps_rc;
    if (s_nspec >= RC_SPEC_MAX) return g.ps_rc;
    cap = sizeof k_hlsl_rc + 2048;
    src = (char *)malloc(cap);
    if (!src) return g.ps_rc;
    len = (size_t)snprintf(src, 2048, "#define SPEC 1\n#define SPEC_H0 %uu\n#define SPEC_H1 %uu\n"
                           "#define SPEC_H2 %uu\n#define SPEC_H3 %uu\n#define SPEC_N %uu\n#define SPEC_WORDS ",
                           c->hdr[0], c->hdr[1], c->hdr[2], c->hdr[3], n);
    for (i = 0; i < 8; i++)
        len += (size_t)snprintf(src + len, 2048 - len, "%suint4(%uu,%uu,%uu,%uu)", i ? "," : "",
                                i < n ? c->stg[2 * i][0] : 0u, i < n ? c->stg[2 * i][1] : 0u,
                                i < n ? c->stg[2 * i][2] : 0u, i < n ? c->stg[2 * i][3] : 0u);
    len += (size_t)snprintf(src + len, 2048 - len, "\n");
    memcpy(src + len, k_hlsl_rc, sizeof k_hlsl_rc - 1);
    len += sizeof k_hlsl_rc - 1;
    t0 = GetTickCount();
    blob = compile_hlsl_src(src, len, "ps_rc", "ps_4_0");
    free(src);
    if (blob) {
        if (FAILED(ID3D11Device_CreatePixelShader(g.dev, ID3D10Blob_GetBufferPointer(blob),
                                                  ID3D10Blob_GetBufferSize(blob), NULL, &ps)))
            ps = NULL;
        ID3D10Blob_Release(blob);
    }
    if (!ps) s_spec_fail++;
    s_spec[s_nspec].key = h;
    s_spec[s_nspec].ps = ps;
    s_nspec++;
    s_spec_ms += GetTickCount() - t0;
    return ps ? ps : g.ps_rc;
}

static void init_rc_pipeline(void)
{
    static const D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "RHW",      0, DXGI_FORMAT_R32_FLOAT,       0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,  0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 64, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 6, DXGI_FORMAT_R32_FLOAT,          1, 80, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3DBlob *vsb, *psb;
    D3D11_BUFFER_DESC bd;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Texture2D *wt = NULL;
    uint32_t white = 0xFFFFFFFFu;
    HRESULT hr;

    if (!getenv("RECOMP_D3D11_RC")) { logf_("combiner path off (set RECOMP_D3D11_RC=1 to enable)\n"); return; }
    vsb = compile_hlsl_src(k_hlsl_rc, sizeof k_hlsl_rc - 1, "vs_rc", "vs_4_0");
    psb = compile_hlsl_src(k_hlsl_rc, sizeof k_hlsl_rc - 1, "ps_rc", "ps_4_0");
    if (!vsb || !psb) { logf_("combiner shaders did not compile: path off\n"); return; }
    hr = ID3D11Device_CreateVertexShader(g.dev, ID3D10Blob_GetBufferPointer(vsb),
                                         ID3D10Blob_GetBufferSize(vsb), NULL, &g.vs_rc);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreatePixelShader(g.dev, ID3D10Blob_GetBufferPointer(psb),
                                            ID3D10Blob_GetBufferSize(psb), NULL, &g.ps_rc);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(g.dev, el, 10, ID3D10Blob_GetBufferPointer(vsb),
                                            ID3D10Blob_GetBufferSize(vsb), &g.layout_rc);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    if (FAILED(hr)) { logf_("combiner shader/layout creation 0x%08lX: path off\n", (unsigned long)hr); return; }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = XVB_BYTES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(g.dev, &bd, NULL, &g.xvb);
    if (SUCCEEDED(hr)) {
        bd.ByteWidth = sizeof(RcConsts);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = ID3D11Device_CreateBuffer(g.dev, &bd, NULL, &g.cb_rc);
    }
    memset(&td, 0, sizeof td);
    td.Width = td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = &white; sd.SysMemPitch = 4; sd.SysMemSlicePitch = 0;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateTexture2D(g.dev, &td, &sd, &wt);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)wt, NULL, &g.white);
    if (wt) ID3D11Texture2D_Release(wt);
    if (FAILED(hr)) { logf_("combiner buffers 0x%08lX: path off\n", (unsigned long)hr); return; }
    g.rc_ok = 1;
    logf_("combiner path ready (ps_4_0)\n");
}

#include "d3d11_gvs.inc"

static int init_pipeline(void)
{
    static const D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "RHW",      0, DXGI_FORMAT_R32_FLOAT,       0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,  0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3DBlob *vsb, *psb;
    D3D11_BUFFER_DESC bd;
    HRESULT hr;

    vsb = compile_hlsl("vs_main", "vs_4_0");
    psb = compile_hlsl("ps_main", "ps_4_0");
    if (!vsb || !psb) return 0;
    hr = ID3D11Device_CreateVertexShader(g.dev, ID3D10Blob_GetBufferPointer(vsb),
                                         ID3D10Blob_GetBufferSize(vsb), NULL, &g.vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreatePixelShader(g.dev, ID3D10Blob_GetBufferPointer(psb),
                                            ID3D10Blob_GetBufferSize(psb), NULL, &g.ps);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(g.dev, el, 4, ID3D10Blob_GetBufferPointer(vsb),
                                            ID3D10Blob_GetBufferSize(vsb), &g.layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    if (FAILED(hr)) { logf_("shader/layout creation 0x%08lX\n", (unsigned long)hr); return 0; }

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VB_BYTES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(g.dev, &bd, NULL, &g.vb);
    if (SUCCEEDED(hr)) {
        bd.ByteWidth = sizeof(DrawConsts);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = ID3D11Device_CreateBuffer(g.dev, &bd, NULL, &g.cb);
    }
    if (FAILED(hr)) { logf_("buffer creation 0x%08lX\n", (unsigned long)hr); return 0; }
    init_rc_pipeline();
    return 1;
}

static void *cache_get(StateCache *c, const void *key, size_t n)
{
    int i;
    for (i = 0; i < c->n; i++)
        if (!memcmp(c->e[i].key, key, n)) return c->e[i].obj;
    return NULL;
}

static void cache_put(StateCache *c, const void *key, size_t n, void *obj)
{
    if (c->n >= MAX_STATES) { logf_("state cache full\n"); return; }
    memset(c->e[c->n].key, 0, sizeof c->e[c->n].key);
    memcpy(c->e[c->n].key, key, n);
    c->e[c->n].obj = obj;
    c->n++;
}

static D3D11_BLEND blend_rgb(uint32_t gl)
{
    switch (gl) {
    case 0:      return D3D11_BLEND_ZERO;
    case 1:      return D3D11_BLEND_ONE;
    case 0x300:  return D3D11_BLEND_SRC_COLOR;
    case 0x301:  return D3D11_BLEND_INV_SRC_COLOR;
    case 0x302:  return D3D11_BLEND_SRC_ALPHA;
    case 0x303:  return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304:  return D3D11_BLEND_DEST_ALPHA;
    case 0x305:  return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306:  return D3D11_BLEND_DEST_COLOR;
    case 0x307:  return D3D11_BLEND_INV_DEST_COLOR;
    case 0x308:  return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND blend_alpha(D3D11_BLEND b)       /* alpha channel takes no _COLOR factors */
{
    switch (b) {
    case D3D11_BLEND_SRC_COLOR:      return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR:  return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR:     return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    default:                         return b;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t gl)
{
    switch (gl) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_STENCIL_OP stencil_op(uint32_t gl)
{
    switch (gl) {
    case 0:      return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

static D3D11_COMPARISON_FUNC cmp_func(uint32_t gl)
{
    return (D3D11_COMPARISON_FUNC)((gl & 7u) + 1u);     /* GL_NEVER..GL_ALWAYS = 0x200..0x207 */
}

static ID3D11BlendState *get_blend(const Nv2aRenderState *rs)
{
    uint32_t key[5] = { 0 };
    ID3D11BlendState *o;
    D3D11_BLEND_DESC d;
    UINT8 mask = 0;

    if (rs->color_mask & 0x00010000u) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (rs->color_mask & 0x00000100u) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (rs->color_mask & 0x00000001u) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (rs->color_mask & 0x01000000u) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    key[0] = rs->blend_enable ? 1 : 0;
    if (key[0]) { key[1] = rs->blend_src; key[2] = rs->blend_dst; key[3] = rs->blend_eq; }
    key[4] = mask;
    o = (ID3D11BlendState *)cache_get(&g.blend, key, sizeof key);
    if (o) return o;
    memset(&d, 0, sizeof d);
    d.RenderTarget[0].BlendEnable = key[0] ? TRUE : FALSE;
    d.RenderTarget[0].SrcBlend = blend_rgb(key[1]);
    d.RenderTarget[0].DestBlend = blend_rgb(key[2]);
    d.RenderTarget[0].BlendOp = blend_op(key[3]);
    d.RenderTarget[0].SrcBlendAlpha = blend_alpha(d.RenderTarget[0].SrcBlend);
    d.RenderTarget[0].DestBlendAlpha = blend_alpha(d.RenderTarget[0].DestBlend);
    d.RenderTarget[0].BlendOpAlpha = d.RenderTarget[0].BlendOp;
    d.RenderTarget[0].RenderTargetWriteMask = mask;
    if (!key[0]) {
        d.RenderTarget[0].SrcBlend = d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlend = d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        d.RenderTarget[0].BlendOp = d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }
    if (FAILED(ID3D11Device_CreateBlendState(g.dev, &d, &o))) { logf_("CreateBlendState failed\n"); return NULL; }
    cache_put(&g.blend, key, sizeof key, o);
    return o;
}

static ID3D11DepthStencilState *get_depth(const Nv2aRenderState *rs, int has_depth)
{
    uint32_t key[11] = { 0 };
    ID3D11DepthStencilState *o;
    D3D11_DEPTH_STENCIL_DESC d;

    key[0] = has_depth;
    if (has_depth) {
        key[1] = rs->depth_test_enable ? 1 : 0;
        key[2] = rs->depth_func;
        key[3] = rs->depth_write ? 1 : 0;
        key[4] = rs->stencil_enable ? 1 : 0;
        if (key[4]) {
            key[5] = rs->stencil_func; key[6] = rs->stencil_func_mask & 0xFF;
            key[7] = rs->stencil_write_mask & 0xFF; key[8] = rs->stencil_fail;
            key[9] = rs->stencil_zfail; key[10] = rs->stencil_zpass;
        }
    }
    o = (ID3D11DepthStencilState *)cache_get(&g.depth, key, sizeof key);
    if (o) return o;
    memset(&d, 0, sizeof d);
    if (has_depth) {
        /* The NV2A's z test and z write are independent; D3D11's enable gates
         * both, so "write without test" becomes enable + ALWAYS. */
        d.DepthEnable = (key[1] || key[3]) ? TRUE : FALSE;
        d.DepthFunc = key[1] ? cmp_func(key[2]) : D3D11_COMPARISON_ALWAYS;
        d.DepthWriteMask = key[3] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.StencilEnable = key[4] ? TRUE : FALSE;
        d.StencilReadMask = (UINT8)key[6];
        d.StencilWriteMask = (UINT8)key[7];
        d.FrontFace.StencilFunc = d.BackFace.StencilFunc = cmp_func(key[5]);
        d.FrontFace.StencilFailOp = d.BackFace.StencilFailOp = stencil_op(key[8]);
        d.FrontFace.StencilDepthFailOp = d.BackFace.StencilDepthFailOp = stencil_op(key[9]);
        d.FrontFace.StencilPassOp = d.BackFace.StencilPassOp = stencil_op(key[10]);
    }
    if (!key[4]) {
        d.FrontFace.StencilFunc = d.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        d.FrontFace.StencilFailOp = d.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        d.FrontFace.StencilDepthFailOp = d.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        d.FrontFace.StencilPassOp = d.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    }
    if (FAILED(ID3D11Device_CreateDepthStencilState(g.dev, &d, &o))) { logf_("CreateDepthStencilState failed\n"); return NULL; }
    cache_put(&g.depth, key, sizeof key, o);
    return o;
}

static ID3D11RasterizerState *get_raster(const Nv2aRenderState *rs, int depth_clip)
{
    /* RECOMP_D3D11_CULL=1 enables culling; the software renderer does not
     * cull and the winding mapping below is unverified, so it is off by default. */
    static int cull_ok = -1;
    uint32_t key[3] = { 0 };
    ID3D11RasterizerState *o;
    D3D11_RASTERIZER_DESC d;

    key[2] = depth_clip ? 1u : 0u;   /* GPU vertex shaders: z < 0 must be clipped like the CPU near clip */
    if (cull_ok < 0) cull_ok = getenv("RECOMP_D3D11_CULL") && getenv("RECOMP_D3D11_CULL")[0] == '1';
    if (cull_ok && rs->cull_enable && (rs->cull_face == 0x404 || rs->cull_face == 0x405)) {
        key[0] = rs->cull_face; key[1] = rs->front_face;
    }
    o = (ID3D11RasterizerState *)cache_get(&g.raster, key, sizeof key);
    if (o) return o;
    memset(&d, 0, sizeof d);
    d.FillMode = D3D11_FILL_SOLID;
    d.CullMode = !key[0] ? D3D11_CULL_NONE : (key[0] == 0x404 ? D3D11_CULL_FRONT : D3D11_CULL_BACK);
    d.FrontCounterClockwise = (key[1] == 0x901) ? TRUE : FALSE;
    d.DepthClipEnable = key[2] ? TRUE : FALSE;
    if (FAILED(ID3D11Device_CreateRasterizerState(g.dev, &d, &o))) { logf_("CreateRasterizerState failed\n"); return NULL; }
    cache_put(&g.raster, key, sizeof key, o);
    return o;
}

/* ---- textures -------------------------------------------------------------
 *
 * Textures are decoded from guest memory by nv2a_backend_decode_texture() into
 * 0xAARRGGBB (== DXGI B8G8R8A8_UNORM) and cached by (offset, format, size,
 * pitch). Titles rewrite textures in place, so every use hashes the texture's
 * bytes and re-decodes + re-uploads when the hash changed. A texture whose
 * address is a render surface we drew into is sampled straight from that
 * surface instead (render-to-texture).
 */
extern ptrdiff_t xbox_GetMemoryOffset(void);

#define MAX_TEX 768

typedef struct {
    uint32_t off, color, w, h, pitch;
    uint64_t hash;
    uint32_t hash_flip;              /* flip count when the texture was last checked */
    uint64_t sample;                 /* cheap sampled hash */
    uint64_t full_us;                /* time of the last full hash */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    uint32_t last_use;
} TexEntry;

static TexEntry s_tex[MAX_TEX];
static int s_ntex;
static uint32_t *s_scratch;
static size_t s_scratch_cap;
static StateCache s_samp;
static uint64_t s_dec_us, s_hash_us, s_dec_n, s_up_n;

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

/* RECOMP_D3D11_TEXDUMP=<hex guest offset as shown by FRAME_TRACE>: write the decoded texture (rgb and alpha) to <RECOMP_FB_DUMP>tex_<off>_<n>[_a].bmp each time it is (re)decoded. */
static void dump_bgra(const uint32_t *px, uint32_t w, uint32_t h, const char *path, int alpha_only)
{
    FILE *f = fopen(path, "wb");
    uint32_t x, y, rb = (w * 3 + 3) & ~3u, isz = rb * h, v;
    uint8_t hdr[54] = { 'B', 'M' }, *row = (uint8_t *)malloc(rb);
    if (!f || !row) { if (f) fclose(f); free(row); return; }
    v = 54 + isz; memcpy(hdr + 2, &v, 4);
    v = 54; memcpy(hdr + 10, &v, 4);
    v = 40; memcpy(hdr + 14, &v, 4);
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24; memcpy(hdr + 34, &isz, 4);
    fwrite(hdr, 1, 54, f);
    for (y = h; y-- > 0; ) {
        memset(row, 0, rb);
        for (x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * w + x];
            if (alpha_only) row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = (uint8_t)(p >> 24);
            else { row[x * 3] = (uint8_t)p; row[x * 3 + 1] = (uint8_t)(p >> 8); row[x * 3 + 2] = (uint8_t)(p >> 16); }
        }
        fwrite(row, 1, rb, f);
    }
    fclose(f); free(row);
}

/* TEXDUMP accepts a comma-separated list of hex offsets. */
static int td_match(const char *list, uint32_t off)
{
    const char *s = list;
    while (*s) {
        char *e;
        unsigned long v = strtoul(s, &e, 16);
        if (e != s && (uint32_t)v == off) return 1;
        s = (*e == ',') ? e + 1 : (*e ? e + 1 : e);
    }
    return 0;
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
    /* Hash once per presented frame unless RECOMP_D3D11_HASH_EVERY_USE=1: titles
     * rewrite textures between frames, rarely between two draws of one frame. */
    if (e && !g.hash_every_use && e->hash_flip == g.flips && e->srv) { e->last_use = g.flips; return e; }
    t0 = qpc_us();
    span = tex_span(t);
    if (e && span && !g.hash_every_use) {
        /* Full hashes of 300+ textures every presented frame cost more than the
         * rendering. Check a sample every frame; take the full hash only when the
         * sample moved or the last full hash is older than 500 ms. */
        uint64_t sh = sample_span((const uint8_t *)xbox_GetMemoryOffset() + t->offset, span);
        if (sh == e->sample && t0 - e->full_us < 500000) {
            s_hash_us += qpc_us() - t0;
            e->hash_flip = g.flips; e->last_use = g.flips;
            return e;
        }
    }
    if (span) h = hash_span((const uint8_t *)xbox_GetMemoryOffset() + t->offset, span);
    s_hash_us += qpc_us() - t0;
    if (e && e->hash == h) {
        e->sample = span ? sample_span((const uint8_t *)xbox_GetMemoryOffset() + t->offset, span) : 0;
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
    t0 = qpc_us();
    if (!nv2a_backend_decode_texture(t, s_scratch)) {
        if (g.log) logf_("texture decode failed: off 0x%08X fmt 0x%X %ux%u pitch %u\n",
                         t->offset, t->color, t->width, t->height, t->pitch);
        return NULL;
    }
    s_dec_us += qpc_us() - t0;
    s_dec_n++;
    {
        static const char *td_off; static int td_init, td_n;
        if (!td_init) { td_init = 1; td_off = getenv("RECOMP_D3D11_TEXDUMP"); }
        if (td_off && g.dump_prefix && td_match(td_off, t->offset) && td_n < 40) {
            char pth[640];
            snprintf(pth, sizeof pth, "%stex_%08X_%02d.bmp", g.dump_prefix, t->offset, td_n);
            dump_bgra(s_scratch, t->width, t->height, pth, 0);
            snprintf(pth, sizeof pth, "%stex_%08X_%02d_a.bmp", g.dump_prefix, t->offset, td_n);
            dump_bgra(s_scratch, t->width, t->height, pth, 1);
            {   /* the raw guest bytes too, to find the same texture in a xemu RAM dump */
                size_t rs = tex_span(t);
                FILE *rf;
                snprintf(pth, sizeof pth, "%stex_%08X_%02d.bin", g.dump_prefix, t->offset, td_n);
                rf = rs ? fopen(pth, "wb") : NULL;
                if (rf) { fwrite((const uint8_t *)xbox_GetMemoryOffset() + t->offset, 1, rs, rf); fclose(rf); }
            }
            td_n++;
        }
    }

    if (!e) {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd;
        HRESULT hr;
        if (s_ntex < MAX_TEX) e = &s_tex[s_ntex++];
        else {                                           /* evict the least recently used */
            int w = 0;
            for (i = 1; i < MAX_TEX; i++) if (s_tex[i].last_use < s_tex[w].last_use) w = i;
            e = &s_tex[w];
            ID3D11ShaderResourceView_Release(e->srv);
            ID3D11Texture2D_Release(e->tex);
        }
        memset(e, 0, sizeof *e);
        memset(&td, 0, sizeof td);
        td.Width = t->width; td.Height = t->height; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.pSysMem = s_scratch; sd.SysMemPitch = t->width * 4; sd.SysMemSlicePitch = 0;
        hr = ID3D11Device_CreateTexture2D(g.dev, &td, &sd, &e->tex);
        if (SUCCEEDED(hr))
            hr = ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)e->tex, NULL, &e->srv);
        if (FAILED(hr)) {
            logf_("texture %ux%u failed 0x%08lX\n", t->width, t->height, (unsigned long)hr);
            if (e->tex) ID3D11Texture2D_Release(e->tex);
            if (e == &s_tex[s_ntex - 1]) s_ntex--;
            else memset(e, 0, sizeof *e);
            return NULL;
        }
        e->off = t->offset; e->color = t->color; e->w = t->width; e->h = t->height;
        e->pitch = t->pitch;
        if (g.log && s_ntex <= 40)
            logf_("new texture off 0x%08X fmt 0x%X %ux%u pitch %u\n", t->offset, t->color,
                  t->width, t->height, t->pitch);
    } else {
        ID3D11DeviceContext_UpdateSubresource(g.ctx, (ID3D11Resource *)e->tex, 0, NULL,
                                              s_scratch, t->width * 4, 0);
    }
    s_up_n++;
    e->hash = h;
    e->sample = span ? sample_span((const uint8_t *)xbox_GetMemoryOffset() + t->offset, span) : 0;
    e->full_us = t0;
    e->hash_flip = g.flips;
    e->last_use = g.flips;
    return e;
}

static ID3D11SamplerState *get_sampler(const Nv2aTexture *t)
{
    uint32_t key[3] = { 0 };
    ID3D11SamplerState *o;
    D3D11_SAMPLER_DESC d;
    uint32_t mn = (t->filter >> 16) & 0xFF, mg = (t->filter >> 24) & 0x0F;
    D3D11_TEXTURE_ADDRESS_MODE am[2];
    int k;

    key[0] = t->addr_u; key[1] = t->addr_v;
    key[2] = (mn == 1 && mg == 1) ? 0 : 1;               /* NEAREST both: point, else linear */
    o = (ID3D11SamplerState *)cache_get(&s_samp, key, sizeof key);
    if (o) return o;
    for (k = 0; k < 2; k++) {
        switch (key[k]) {
        case 1:  am[k] = D3D11_TEXTURE_ADDRESS_WRAP; break;
        case 2:  am[k] = D3D11_TEXTURE_ADDRESS_MIRROR; break;
        case 4:  am[k] = D3D11_TEXTURE_ADDRESS_BORDER; break;
        default: am[k] = D3D11_TEXTURE_ADDRESS_CLAMP; break;     /* 3 clamp, 5 clamp-to-edge */
        }
    }
    memset(&d, 0, sizeof d);
    d.Filter = key[2] ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.AddressU = am[0]; d.AddressV = am[1]; d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxAnisotropy = 1;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(g.dev, &d, &o))) { logf_("CreateSamplerState failed\n"); return NULL; }
    cache_put(&s_samp, key, sizeof key, o);
    return o;
}

static void dump_tex(ID3D11Texture2D *srct, const char *name, int alpha_only);
extern void nv2a_blit_register(void (*fn)(uint32_t, uint32_t, uint32_t, uint32_t));

/* A render surface we own whose guest address is this texture's: sample it directly. */
static ID3D11ShaderResourceView *rt_as_texture(const Nv2aTexture *t, RenderTarget **out)
{
    int i;
    for (i = 0; i < g.nrt; i++)
        if (g.rt[i].va == t->offset && g.rt[i].w == t->width && g.rt[i].h == t->height) {
            {   /* RECOMP_D3D11_RTDUMP=2: also write each RT the moment it is first sampled (up to 8 per RT per dump frame) */
                static int on = -1;
                static uint32_t dfl[16], dn[16];
                if (on < 0) on = getenv("RECOMP_D3D11_RTDUMP") && getenv("RECOMP_D3D11_RTDUMP")[0] == '2';
                if (on && g.dump_prefix && g.dump_every && (g.flips % g.dump_every) == 0 && i < 16) {
                    if (dfl[i] != g.flips + 1) { dfl[i] = g.flips + 1; dn[i] = 0; }
                    if (dn[i] < 8) {
                        char nm[96];
                        snprintf(nm, sizeof nm, "s%u_%08X_%u", g.flips, g.rt[i].va, dn[i]++);
                        dump_tex(g.rt[i].tex, nm, 0);
                        strcat(nm, "_a");
                        dump_tex(g.rt[i].tex, nm, 1);
                    }
                }
            }
            if (!g.rt[i].srv &&
                FAILED(ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)g.rt[i].tex,
                                                             NULL, &g.rt[i].srv)))
                return NULL;
            *out = &g.rt[i];
            return g.rt[i].srv;
        }
    return NULL;
}

static struct { uint32_t va, w, h; ID3D11Texture2D *dtex; ID3D11DepthStencilView *dsv; } zpool[8];
static int zpool_n, zpool_want;

/* RECOMP_D3D11_ZTEX=1 (2 = also dump): a texture whose guest address is the zeta buffer reads the depth
 * buffer. On the NV2A the zeta surface is ordinary memory: a Z24S8 word (Z << 8 | S) per
 * pixel, so a 32-bit linear colour texture at that address sees byte 0 = stencil,
 * byte 1..3 = Z low, mid, high. Our depth lives only in a D3D depth buffer and that guest
 * memory stays zero, so such reads (the entrance cutscene's depth-of-field pass samples
 * zeta 0x00C04000 as LIN_A8B8G8R8) saw Z = 0. Rebuild the bytes from the D3D depth
 * buffer into a colour texture and sample that instead. */
/* RECOMP_D3D11_CLIPPLANE=1: honour texture-stage mode 5 (CLIPPLANE) in the combiner shader --
 * the pixel is killed when a texture coordinate of that stage fails its SET_SHADER_CLIP_PLANE_MODE
 * test (xemu psh.c, and the software path's rc_stage_fetch). Off: the stage reads zero and
 * nothing is killed (old behaviour). */
static int clipplane_enabled(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_D3D11_CLIPPLANE") ? atoi(getenv("RECOMP_D3D11_CLIPPLANE")) : 0;
    return on;
}

static int ztex_enabled(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_D3D11_ZTEX") ? atoi(getenv("RECOMP_D3D11_ZTEX")) : 0;
    return on;
}

static const char k_hlsl_ztex[] =
"Texture2D<float> zd : register(t0);\n"
"Texture2D<uint2> zs : register(t1);\n"
"float4 vs_z(uint id : SV_VertexID) : SV_Position {\n"
"  float2 p = float2((id << 1) & 2, id & 2);\n"
"  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
"float4 zbytes(float4 pos) {\n"
"  int3 q = int3(int2(pos.xy), 0);\n"
"  uint z = (uint)(saturate(zd.Load(q)) * 16777215.0 + 0.5);\n"
"  uint s = zs.Load(q).g;\n"
"  return float4(s, z & 255u, (z >> 8) & 255u, z >> 16) / 255.0; }\n"
"float4 ps_z_abgr(float4 pos : SV_Position) : SV_Target { return zbytes(pos); }\n"
"float4 ps_z_argb(float4 pos : SV_Position) : SV_Target { float4 b = zbytes(pos); return float4(b.b, b.g, b.r, b.a); }\n";

static ID3D11ShaderResourceView *zeta_as_texture(const Nv2aTexture *t)
{
    static ID3D11VertexShader *vs;
    static ID3D11PixelShader *ps[2];
    static int init_failed;
    static struct {
        ID3D11Texture2D *dtex; uint32_t fmt;
        ID3D11ShaderResourceView *dsrv, *ssrv, *srv;
        ID3D11Texture2D *ctex; ID3D11RenderTargetView *rtv;
    } zc[4];
    static int nzc, nlog;
    RenderTarget *zr = NULL;
    int i, argb;
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *sv[2];

    if (!ztex_enabled() || init_failed) return NULL;
    if (t->color != 0x3F && t->color != 0x12) return NULL;   /* LIN_A8B8G8R8, LIN_A8R8G8B8 */
    if (t->pitch && t->pitch != t->width * 4) return NULL;
    for (i = 0; i < g.nrt; i++)
        if (g.rt[i].dtex && g.rt[i].zeta_va &&
            (g.rt[i].zeta_va & 0x03FFFFFFu) == (t->offset & 0x03FFFFFFu) &&
            g.rt[i].w == t->width && g.rt[i].h == t->height) { zr = &g.rt[i]; break; }
    if (!zr) return NULL;
    argb = t->color == 0x12;
    if (!vs) {
        ID3DBlob *b0 = compile_hlsl_src(k_hlsl_ztex, sizeof k_hlsl_ztex - 1, "vs_z", "vs_4_0");
        ID3DBlob *b1 = compile_hlsl_src(k_hlsl_ztex, sizeof k_hlsl_ztex - 1, "ps_z_abgr", "ps_4_0");
        ID3DBlob *b2 = compile_hlsl_src(k_hlsl_ztex, sizeof k_hlsl_ztex - 1, "ps_z_argb", "ps_4_0");
        if (b0 && b1 && b2 &&
            SUCCEEDED(ID3D11Device_CreateVertexShader(g.dev, ID3D10Blob_GetBufferPointer(b0), ID3D10Blob_GetBufferSize(b0), NULL, &vs)) &&
            SUCCEEDED(ID3D11Device_CreatePixelShader(g.dev, ID3D10Blob_GetBufferPointer(b1), ID3D10Blob_GetBufferSize(b1), NULL, &ps[0])) &&
            SUCCEEDED(ID3D11Device_CreatePixelShader(g.dev, ID3D10Blob_GetBufferPointer(b2), ID3D10Blob_GetBufferSize(b2), NULL, &ps[1]))) {
        } else {
            init_failed = 1;
            logf_("ZTEX: shader setup failed\n");
        }
        if (b0) ID3D10Blob_Release(b0);
        if (b1) ID3D10Blob_Release(b1);
        if (b2) ID3D10Blob_Release(b2);
        if (init_failed) return NULL;
    }
    for (i = 0; i < nzc; i++)
        if (zc[i].dtex == zr->dtex && zc[i].fmt == t->color) break;
    if (i == nzc) {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        if (nzc >= 4) return NULL;
        memset(&zc[i], 0, sizeof zc[i]);
        memset(&sd, 0, sizeof sd);
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        if (FAILED(ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)zr->dtex, &sd, &zc[i].dsrv))) {
            if (nlog++ < 3) logf_("ZTEX: zeta 0x%08X is not readable (created before RECOMP_D3D11_ZTEX?)\n", zr->zeta_va);
            return NULL;
        }
        sd.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        if (FAILED(ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)zr->dtex, &sd, &zc[i].ssrv))) {
            ID3D11ShaderResourceView_Release(zc[i].dsrv);
            return NULL;
        }
        memset(&td, 0, sizeof td);
        td.Width = t->width; td.Height = t->height; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(g.dev, &td, NULL, &zc[i].ctex)) ||
            FAILED(ID3D11Device_CreateRenderTargetView(g.dev, (ID3D11Resource *)zc[i].ctex, NULL, &zc[i].rtv)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)zc[i].ctex, NULL, &zc[i].srv))) {
            logf_("ZTEX: %ux%u copy texture failed\n", t->width, t->height);
            return NULL;
        }
        ID3D11Texture2D_AddRef(zr->dtex);
        zc[i].dtex = zr->dtex; zc[i].fmt = t->color;
        nzc++;
        logf_("ZTEX: zeta 0x%08X %ux%u sampled as fmt 0x%X (flip %u)\n",
                         zr->zeta_va, t->width, t->height, t->color, g.flips);
    }
    /* Copy now: the depth buffer as it stands at this draw. Unbind it as depth first. */
    {
        ID3D11ShaderResourceView *nos[2] = { NULL, NULL };
        ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 2, nos);
    }
    ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 1, &zc[i].rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(g.ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g.ctx, NULL, 0);
    ID3D11DeviceContext_RSSetState(g.ctx, NULL);
    vp.TopLeftX = vp.TopLeftY = 0.0f;
    vp.Width = (float)t->width; vp.Height = (float)t->height;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g.ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(g.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g.ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g.ctx, ps[argb], NULL, 0);
    sv[0] = zc[i].dsrv; sv[1] = zc[i].ssrv;
    ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 2, sv);
    ID3D11DeviceContext_Draw(g.ctx, 3, 0);
    sv[0] = sv[1] = NULL;
    ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 2, sv);
    if (ztex_enabled() == 2 && g.dump_prefix && g.dump_every && (g.flips % g.dump_every) == 0) {
        char nm[64];   /* RECOMP_D3D11_ZTEX=2: also write the rebuilt bytes on dump frames */
        snprintf(nm, sizeof nm, "ztex_%u", g.flips);
        dump_tex(zc[i].ctex, nm, 0);
        strcat(nm, "_a");
        dump_tex(zc[i].ctex, nm, 1);
    }
    return zc[i].srv;
}

/* Depth/stencil surface for a render target, created on first use. */
static void ensure_depth(RenderTarget *r, const Nv2aRenderState *rs)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    if (!rs || !rs->zeta_va) return;
    if (r->dsv && r->zeta_va == rs->zeta_va) return;
    if (r->dsv) { ID3D11DepthStencilView_Release(r->dsv); r->dsv = NULL; }
    if (r->dtex) { ID3D11Texture2D_Release(r->dtex); r->dtex = NULL; }
    /* RECOMP_D3D11_SHAREZ=1: the title has ONE zeta buffer that every pass (display buffer and
     * the offscreen render targets) tests against; it clears it with the display buffer only.
     * Give all render targets of one size and zeta address the same D3D depth buffer. */
    {
        static int share = -1;
        if (share < 0) share = getenv("RECOMP_D3D11_SHAREZ") && getenv("RECOMP_D3D11_SHAREZ")[0] == '1';
        if (share) {
            int k;
            for (k = 0; k < zpool_n; k++)
                if (zpool[k].va == rs->zeta_va && zpool[k].w == r->w && zpool[k].h == r->h) {
                    r->dtex = zpool[k].dtex; r->dsv = zpool[k].dsv;
                    ID3D11Texture2D_AddRef(r->dtex); ID3D11DepthStencilView_AddRef(r->dsv);
                    r->zeta_va = rs->zeta_va;
                    return;
                }
            zpool_want = 1;
        }
    }
    memset(&td, 0, sizeof td);
    td.Width = r->w; td.Height = r->h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (ztex_enabled()) {   /* typeless so zeta_as_texture can read it back */
        td.Format = DXGI_FORMAT_R24G8_TYPELESS;
        td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }
    hr = ID3D11Device_CreateTexture2D(g.dev, &td, NULL, &r->dtex);
    if (SUCCEEDED(hr)) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = ID3D11Device_CreateDepthStencilView(g.dev, (ID3D11Resource *)r->dtex, &dd, &r->dsv);
    }
    if (FAILED(hr)) {
        logf_("depth buffer %ux%u failed 0x%08lX\n", r->w, r->h, (unsigned long)hr);
        if (r->dtex) { ID3D11Texture2D_Release(r->dtex); r->dtex = NULL; }
        return;
    }
    r->zeta_va = rs->zeta_va;
    if (zpool_want && zpool_n < 8) {
        zpool[zpool_n].va = rs->zeta_va; zpool[zpool_n].w = r->w; zpool[zpool_n].h = r->h;
        zpool[zpool_n].dtex = r->dtex; zpool[zpool_n].dsv = r->dsv;
        ID3D11Texture2D_AddRef(r->dtex); ID3D11DepthStencilView_AddRef(r->dsv);
        zpool_n++;
    }
    zpool_want = 0;
    if (g.log) logf_("depth buffer for surface 0x%08X: zeta 0x%08X\n", r->va, r->zeta_va);
}

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    RenderTarget *r;

    if (!init_window_and_device()) return;
    r = get_rt(s);
    if (!r) return;
    r->uses++;
    if (g.log && (flags & 0xF0u) && argb != g.last_clear) {
        logf_("clear colour 0x%08X flags 0x%X surface 0x%08X flip %u\n", argb, flags, s->color_va, g.flips);
        g.last_clear = argb;
    }
    if (flags & 0xF0u) {
        float c[4];
        c[0] = ((argb >> 16) & 0xFF) / 255.0f;
        c[1] = ((argb >> 8) & 0xFF) / 255.0f;
        c[2] = (argb & 0xFF) / 255.0f;
        c[3] = ((argb >> 24) & 0xFF) / 255.0f;
        ID3D11DeviceContext_ClearRenderTargetView(g.ctx, r->rtv, c);
    }
    if ((flags & 3u) && rs && rs->zeta_va) {
        ensure_depth(r, rs);
        if (r->dsv) {
            UINT f = 0;
            if (flags & 1u) f |= D3D11_CLEAR_DEPTH;
            if (flags & 2u) f |= D3D11_CLEAR_STENCIL;
            ID3D11DeviceContext_ClearDepthStencilView(g.ctx, r->dsv, f,
                                                      (zstencil >> 8) / 16777215.0f,
                                                      (UINT8)(zstencil & 0xFF));
        }
    }
}

static uint32_t s_sched_di;
static uint64_t s_draw_us;   /* time inside be_draw, for RECOMP_FLIPTIME */
static void be_draw_impl(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs;
    RenderTarget *r;
    D3D11_VIEWPORT vp;
    D3D11_MAPPED_SUBRESOURCE m;
    DrawConsts dc;
    float bf[4], zscale;
    uint32_t count, bytes, i, stride = (uint32_t)sizeof(Nv2aVertex);
    ID3D11RenderTargetView *rtv;
    UINT off = 0, str = stride;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11SamplerState *smp = NULL;
    ID3D11ShaderResourceView *nosrv = NULL;
    float isz_w = 0.0f, isz_h = 0.0f;
    const Nv2aBatchExt *x = NULL;
    ID3D11ShaderResourceView *rc_srv[4] = { NULL, NULL, NULL, NULL };
    ID3D11SamplerState *rc_smp[4] = { NULL, NULL, NULL, NULL };
    RcConsts rcc;
    int gpu = 0;                      /* GPU vertex-program batch: no CPU vertices (d3d11_gvs.inc) */

    if (!b || !b->count || !b->state || !init_window_and_device()) return;
    {   const Nv2aBatchExt *x0 = nv2a_pbx_current();
        gpu = g.rc_ok && x0 && x0->gpu;
    }
    if (!gpu && !b->vertices) return;
    {   /* RECOMP_D3D11_DLOG=<fromflip>,<toflip>: diagnostic. On dump flips (flips % dump_every == 0) in [fromflip,toflip) print every draw of the frame (index counted before any bisect drop): target, count, textures, blend, depth, combiner. */
        static int don = -1; static uint32_t dl0, dl1, dlast = 0xFFFFFFFFu, ddi;
        if (don < 0) {
            const char *e = getenv("RECOMP_D3D11_DLOG"); unsigned a = 0, c = 0;
            don = (e && sscanf(e, "%u,%u", &a, &c) == 2) ? 1 : 0; dl0 = a; dl1 = c;
        }
        if (don && g.dump_every) {
            uint32_t di2;
            if (g.flips != dlast) { dlast = (uint32_t)g.flips; ddi = 0; }
            di2 = ddi++;
            if ((g.flips % g.dump_every) == 0 && g.flips >= dl0 && g.flips < dl1) {
                const Nv2aBatchExt *xd = nv2a_pbx_current();
                const Nv2aRenderState *rd = b->state;
                int st;
                fprintf(stderr, "[DLOG] f %u d %u rt %08X %ux%u n %u gpu %d bl %u %X/%X eq %X bc %08X z %u/%X/%u at %u/%X/%u cm %X | t", (unsigned)g.flips, di2, s->color_va, s->width, s->height, b->count, gpu,
                        rd->blend_enable, rd->blend_src, rd->blend_dst, rd->blend_eq, rd->blend_color, rd->depth_test_enable, rd->depth_func, rd->depth_write,
                        rd->alpha_test_enable, rd->alpha_func, rd->alpha_ref, rd->color_mask);
                if (b->texture) fprintf(stderr, " T%08X/%ux%u/%02X", b->texture->offset, b->texture->width, b->texture->height, b->texture->color);
                if (xd) for (st = 0; st < 4; st++) if (xd->tex_valid[st]) fprintf(stderr, " %d:%08X/%ux%u/%02X", st, xd->tex[st].offset, xd->tex[st].width, xd->tex[st].height, xd->tex[st].color);
                if (xd && xd->rc) {
                    const Nv2aCombiner *rcd = xd->rc;
                    fprintf(stderr, " | rc ctl %08X prog %05X fin %08X %08X fc %08X %08X", rcd->control, rcd->stage_program, rcd->final0, rcd->final1, rcd->final_c0, rcd->final_c1);
                    for (st = 0; st < (int)(rcd->control & 0xFF) && st < 8; st++)
                        fprintf(stderr, " | s%d %08X %08X %08X %08X c %08X %08X", st, rcd->color_icw[st], rcd->alpha_icw[st], rcd->color_ocw[st], rcd->alpha_ocw[st], rcd->factor0[st], rcd->factor1[st]);
                }
                if (b->vertices) fprintf(stderr, " | v0 %.1f,%.1f z %g diff %08X", b->vertices[0].x, b->vertices[0].y, b->vertices[0].z, (unsigned)b->vertices[0].diffuse);
                if (b->vertices) { uint32_t vi, mx[4] = {0, 0, 0, 0}; for (vi = 0; vi < b->count; vi++) { uint32_t dv = (uint32_t)b->vertices[vi].diffuse; int k; for (k = 0; k < 4; k++) if (((dv >> (8 * k)) & 0xFF) > mx[k]) mx[k] = (dv >> (8 * k)) & 0xFF; } fprintf(stderr, " dmax %02X%02X%02X%02X", mx[3], mx[2], mx[1], mx[0]); }
                fputc(10, stderr);
            }
        }
    }
    {   /* RECOMP_D3D11_SKIPSCHED=<chunk>: bisect aid. Per dump interval k=(flips/dump_every)%9, drop the frame's draws [k*chunk,(k+1)*chunk) (k=8: none). */
        static int chunk = -1;
        if (chunk < 0) chunk = getenv("RECOMP_D3D11_SKIPSCHED") ? atoi(getenv("RECOMP_D3D11_SKIPSCHED")) : 0;
        if (chunk > 0 && g.dump_every) {
            uint32_t seg = (g.flips / g.dump_every) % 9, di = s_sched_di++;
            static uint32_t base = 0xFFFFFFFFu;
            if (base == 0xFFFFFFFFu) base = getenv("RECOMP_D3D11_SKIPBASE") ? (uint32_t)atoi(getenv("RECOMP_D3D11_SKIPBASE")) : 0;
            if (seg < 8 && di >= base + seg * (uint32_t)chunk && di < base + (seg + 1) * (uint32_t)chunk) return;
        }
    }
    {   /* RECOMP_D3D11_CUTOFF=<start>,<step>: bisect aid. Per dump interval seg=(flips/dump_every)%9 (seg<8) drop every draw of the frame with index >= start+seg*step, so the picture shows the frame up to that draw (seg 8: nothing dropped). */
        static int con = -1; static uint32_t cs, cst, lastflip = 0xFFFFFFFFu, cdi;
        if (con < 0) {
            const char *e = getenv("RECOMP_D3D11_CUTOFF"); unsigned a = 0, st2 = 0;
            con = (e && sscanf(e, "%u,%u", &a, &st2) == 2) ? 1 : 0; cs = a; cst = st2;
        }
        if (con && g.dump_every) {
            uint32_t seg = (g.flips / g.dump_every) % 9, di;
            if (g.flips != lastflip) { lastflip = (uint32_t)g.flips; cdi = 0; }
            di = cdi++;
            if (seg < 8 && di >= cs + seg * cst) return;
        }
    }
    {   /* RECOMP_D3D11_SKIPTEX=<hex>[,<hex>...]: experiment, drop draws that sample these texture offsets */
        static uint32_t sk[8]; static int nsk = -1;
        if (nsk < 0) {
            const char *e = getenv("RECOMP_D3D11_SKIPTEX");
            nsk = 0;
            while (e && *e && nsk < 8) { char *end; sk[nsk++] = (uint32_t)strtoul(e, &end, 16); if (end == e) { nsk--; break; } e = (*end == ',') ? end + 1 : end; }
        }
        if (nsk) {
            const Nv2aBatchExt *x1 = nv2a_pbx_current();
            int k, st, hit = 0, k0 = 0, k1 = nsk;
            static int rot = -1;
            if (rot < 0) rot = getenv("RECOMP_D3D11_SKIPROT") ? atoi(getenv("RECOMP_D3D11_SKIPROT")) : 0;
            if (rot && g.dump_every) {   /* SKIPROT=1: per dump interval kk=(flips/dump_every)%(nsk+1) drop only draws sampling sk[kk] (kk==nsk: none) */
                int kk = (int)((g.flips / g.dump_every) % (uint32_t)(nsk + 1));
                k0 = kk; k1 = kk + 1;
                if (kk >= nsk) k1 = k0;
            }
            for (k = k0; k < k1 && !hit; k++) {
                if (b->texture && b->texture->offset == sk[k]) hit = 1;
                if (x1) for (st = 0; st < 4; st++) if (x1->tex_valid[st] && x1->tex[st].offset == sk[k]) hit = 1;
            }
            {   /* RECOMP_D3D11_SKIPTEX_ADD=1: only drop additive (ONE/ONE) draws */
                static int oa = -1;
                if (oa < 0) oa = getenv("RECOMP_D3D11_SKIPTEX_ADD") && getenv("RECOMP_D3D11_SKIPTEX_ADD")[0] == '1';
                if (hit && oa && !(b->state->blend_enable && b->state->blend_src == 1 && b->state->blend_dst == 1)) hit = 0;
            }
            if (hit) { static int nh; if (nh++ < 5) fprintf(stderr, "[D3D11] SKIPTEX dropped draw (flip %u)\n", (unsigned)g.flips); return; }
        }
    }
    rs = b->state;
    {   /* RECOMP_D3D11_VTXLOG=<hex>: diagnostic, log the CPU-transformed vertices of draws sampling this texture offset: all with a screen bbox over 150 px, plus the first 5 */
        static uint32_t vt = 1; static int nl;
        if (vt == 1) { const char *e = getenv("RECOMP_D3D11_VTXLOG"); vt = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        if (vt && b->texture && b->texture->offset == vt && b->vertices && nl < 6000 && g.dump_every && (g.flips % g.dump_every) == 0) {
            uint32_t q, n = b->count - b->count % 3; float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, w0 = 1e30f, w1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
            for (q = 0; q < n; q++) {
                const Nv2aVertex *v = &b->vertices[q];
                if (v->x < x0) x0 = v->x; if (v->x > x1) x1 = v->x;
                if (v->y < y0) y0 = v->y; if (v->y > y1) y1 = v->y;
                if (v->rhw < w0) w0 = v->rhw; if (v->rhw > w1) w1 = v->rhw;
                if (v->z < z0) z0 = v->z; if (v->z > z1) z1 = v->z;
            }
            {
                nl++;
                fprintf(stderr, "[D3D11] VTXLOG flip %u n %u bbox x %.1f..%.1f y %.1f..%.1f rhw %g..%g z %g..%g v0 (%.1f,%.1f,%g,%g) diff %08X uv %.1f,%.1f\n", (unsigned)g.flips, n, x0, x1, y0, y1, w0, w1, z0, z1,
                        b->vertices[0].x, b->vertices[0].y, b->vertices[0].z, b->vertices[0].rhw, (unsigned)b->vertices[0].diffuse, b->vertices[0].u, b->vertices[0].v);
                { uint32_t m2; fprintf(stderr, "[D3D11] VTXLOG   cols"); for (m2 = 0; m2 < n && m2 < 12; m2++) fprintf(stderr, " %08X@(%.0f,%.0f,%.3f)", (unsigned)b->vertices[m2].diffuse, b->vertices[m2].x, b->vertices[m2].y, b->vertices[m2].rhw); fputc(10, stderr); }
            }
        }
    }
    {   /* RECOMP_D3D11_SKIPADD=1|2: experiment, drop purely additive (ONE,ONE) draws; 2 = only when (flips/dump_every) is odd, for A/B in one run */
        static int sa = -1;
        if (sa < 0) sa = getenv("RECOMP_D3D11_SKIPADD") ? atoi(getenv("RECOMP_D3D11_SKIPADD")) : 0;
        if (sa && rs->blend_enable && rs->blend_src == 1 && rs->blend_dst == 1 &&
            (sa == 1 || (g.dump_every && ((g.flips / g.dump_every) & 1)))) {
            static int nhit; if (nhit < 5 || (++nhit % 20000) == 0) fprintf(stderr, "[D3D11] SKIPADD dropped draw (flip %u, %ux%u)\n", (unsigned)g.flips, 0u, 0u);
            nhit++; return;
        }
    }
    r = get_rt(s);
    if (!r) return;
    r->uses++;
    ensure_depth(r, rs);

    count = b->count - b->count % 3;
    if (count > (VB_BYTES / stride) / 3 * 3) count = (VB_BYTES / stride) / 3 * 3;
    if (!count) return;
    bytes = count * stride;

    /* z as the title produced it: scaled to 0..1 by the clip range's top. */
    zscale = (rs->depth_max > 1.0f) ? 1.0f / rs->depth_max : 1.0f;
    if (g.log && g.zlog < 16 && !gpu) {
        float zmin = 1e30f, zmax = -1e30f;
        for (i = 0; i < count; i++) {
            if (b->vertices[i].z < zmin) zmin = b->vertices[i].z;
            if (b->vertices[i].z > zmax) zmax = b->vertices[i].z;
        }
        logf_("batch %u verts: z %.3f..%.3f clip %.3f..%.3f ztest %u func 0x%X zwrite %u zeta 0x%08X blend %u (0x%X,0x%X) tex %s\n",
              count, zmin, zmax, rs->depth_min, rs->depth_max, rs->depth_test_enable,
              rs->depth_func, rs->depth_write, rs->zeta_va, rs->blend_enable, rs->blend_src,
              rs->blend_dst, b->texture ? "yes" : "no");
        g.zlog++;
    }

    if (!gpu) {
    if (g.vb_off + bytes > VB_BYTES) g.vb_off = 0, i = D3D11_MAP_WRITE_DISCARD;
    else i = g.vb_off ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;
    if (FAILED(ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)g.vb, 0, (D3D11_MAP)i, 0, &m))) return;
    memcpy((uint8_t *)m.pData + g.vb_off, b->vertices, bytes);
    ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)g.vb, 0);
    }

    if (b->texture) {
        RenderTarget *src = NULL;
        srv = rt_as_texture(b->texture, &src);
        if (src == r) srv = NULL;
        if (!srv && !src) srv = zeta_as_texture(b->texture);
        if (!srv && !src) {
            TexEntry *e = get_texture(b->texture);
            if (e) srv = e->srv;
        }
        if (srv) {
            smp = get_sampler(b->texture);
            isz_w = 1.0f / (float)b->texture->width;
            isz_h = 1.0f / (float)b->texture->height;
        }
    }

    if (g.rc_ok) x = nv2a_pbx_current();
    if (x && (x->ext || x->gpu) && x->rc) {
        static const Nv2aTexture dummy = { 0, 1, 1, 0, 0, 1, 1, 1, 0 };
        const Nv2aCombiner *rc = x->rc;
        uint32_t st, k;
        memset(&rcc, 0, sizeof rcc);
        rcc.hdr[0] = rc->control; rcc.hdr[1] = rc->stage_program;
        rcc.hdr[2] = rc->final0;  rcc.hdr[3] = rc->final1;
        rcc.hdr2[0] = rc->final_c0; rcc.hdr2[1] = rc->final_c1;
        if (clipplane_enabled()) {
            rcc.hdr2[2] = x->clip_plane_mode; rcc.hdr2[3] = 1;
            {   static uint32_t lm = 0xFFFFFFFFu; static int nlog;
                uint32_t cs2;
                for (cs2 = 0; cs2 < 4; cs2++)
                    if (((rc->stage_program >> (cs2 * 5)) & 0x1F) == 5 && (x->clip_plane_mode != lm) && nlog < 8) {
                        lm = x->clip_plane_mode; nlog++;
                        fprintf(stderr, "[D3D11] CLIPPLANE stage %u mode %08X prog %05X (flip %u)\n", cs2, lm, rc->stage_program, (unsigned)g.flips);
                    }
            }
        }
        for (k = 0; k < 4; k++) rcc.fogc[k] = x->fog_color[k];
        for (st = 0; st < 8; st++) {
            rcc.stg[2 * st][0] = rc->color_icw[st]; rcc.stg[2 * st][1] = rc->alpha_icw[st];
            rcc.stg[2 * st][2] = rc->color_ocw[st]; rcc.stg[2 * st][3] = rc->alpha_ocw[st];
            rcc.stg[2 * st + 1][0] = rc->factor0[st]; rcc.stg[2 * st + 1][1] = rc->factor1[st];
        }
        for (st = 0; st < 4; st++) {
            ID3D11ShaderResourceView *sv = NULL;
            if (x->tex_valid[st] && !x->tex_cube[st] && ((rc->stage_program >> (st * 5)) & 0x1F) != 0) {
                RenderTarget *src = NULL;
                sv = rt_as_texture(&x->tex[st], &src);
                if (src == r) sv = NULL;
                if (!sv && !src) sv = zeta_as_texture(&x->tex[st]);
                if (!sv && !src) {
                    TexEntry *e = get_texture(&x->tex[st]);
                    if (e) sv = e->srv;
                }
            }
            if (sv) {
                rc_smp[st] = get_sampler(&x->tex[st]);
                rcc.uvs[st][2] = rc_smp[st] ? 1.0f : 0.0f;
                if (x->tex_sized[st]) { rcc.uvs[st][0] = rcc.uvs[st][1] = 1.0f; }
                else {
                    rcc.uvs[st][0] = 1.0f / (float)x->tex[st].width;
                    rcc.uvs[st][1] = 1.0f / (float)x->tex[st].height;
                }
            }
            if (!sv || !rc_smp[st]) {
                sv = g.white;
                rc_smp[st] = get_sampler(&dummy);
                rcc.uvs[st][0] = rcc.uvs[st][1] = 1.0f; rcc.uvs[st][2] = 0.0f;
            }
            rc_srv[st] = sv;
        }
    } else {
        x = NULL;
    }

    memset(&dc, 0, sizeof dc);
    dc.textured = (srv && smp) ? 1 : 0;
    dc.inv_w = isz_w; dc.inv_h = isz_h;
    dc.surf_w = (float)r->w; dc.surf_h = (float)r->h; dc.zscale = zscale;
    dc.aref = (float)(rs->alpha_ref & 0xFF);
    dc.atest = rs->alpha_test_enable ? 1 : 0;
    dc.afunc = rs->alpha_func & 7u;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &dc, sizeof dc);
        ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)g.cb, 0);
    }

    {   /* blend constant: 0xAARRGGBB; the constant-alpha factors broadcast alpha */
        uint32_t c = rs->blend_color;
        int ca = (rs->blend_src == 0x8003 || rs->blend_src == 0x8004 ||
                  rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004) &&
                 !(rs->blend_src == 0x8001 || rs->blend_src == 0x8002 ||
                   rs->blend_dst == 0x8001 || rs->blend_dst == 0x8002);
        float a = ((c >> 24) & 0xFF) / 255.0f;
        bf[0] = ca ? a : ((c >> 16) & 0xFF) / 255.0f;
        bf[1] = ca ? a : ((c >> 8) & 0xFF) / 255.0f;
        bf[2] = ca ? a : (c & 0xFF) / 255.0f;
        bf[3] = a;
    }

    rtv = r->rtv;
    {
        ID3D11ShaderResourceView *nos4[4] = { NULL, NULL, NULL, NULL };
        (void)nosrv;
        ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 4, nos4);   /* never sample what we render to */
    }
    {   /* RECOMP_D3D11_NOZRT=<va>[,<va>...] (diagnostic): draw to these render targets with no depth buffer */
        static char *nz = (char *)-1;
        int usez = (rs->zeta_va && r->dsv) ? 1 : 0;
        if (nz == (char *)-1) nz = getenv("RECOMP_D3D11_NOZRT");
        if (nz && usez) {
            char hx[12]; snprintf(hx, sizeof hx, "%08X", s->color_va);
            if (strstr(nz, hx)) usez = 0;
        }
        ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 1, &rtv, usez ? r->dsv : NULL);
        ID3D11DeviceContext_OMSetBlendState(g.ctx, get_blend(rs), bf, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetDepthStencilState(g.ctx, get_depth(rs, usez), rs->stencil_ref & 0xFF);
    }
    ID3D11DeviceContext_RSSetState(g.ctx, get_raster(rs, gpu));
    vp.TopLeftX = vp.TopLeftY = 0.0f;
    vp.Width = (float)r->w; vp.Height = (float)r->h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g.ctx, 1, &vp);
    if (x && x->gpu) {                    /* the vertex program runs on the GPU */
        if (SUCCEEDED(ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)g.cb_rc, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, &rcc, sizeof rcc);
            ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)g.cb_rc, 0);
            gvs_draw(x->gpu, &rcc, rc_srv, rc_smp);
        }
        return;
    }
    if (x) {
        D3D11_MAPPED_SUBRESOURCE xm;
        ID3D11Buffer *vbs[2];
        UINT strs[2], offs[2];
        uint32_t xb = count * (uint32_t)sizeof(Nv2aVertexExt), xmode;
        if (g.xvb_off + xb > XVB_BYTES) g.xvb_off = 0, xmode = D3D11_MAP_WRITE_DISCARD;
        else xmode = g.xvb_off ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;
        if (SUCCEEDED(ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)g.xvb, 0, (D3D11_MAP)xmode, 0, &xm))) {
            memcpy((uint8_t *)xm.pData + g.xvb_off, x->ext, xb);
            ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)g.xvb, 0);
            if (SUCCEEDED(ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)g.cb_rc, 0,
                                                  D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, &rcc, sizeof rcc);
                ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)g.cb_rc, 0);
                ID3D11Buffer *pcb[2];
                vbs[0] = g.vb; vbs[1] = g.xvb;
                strs[0] = stride; strs[1] = (UINT)sizeof(Nv2aVertexExt);
                offs[0] = g.vb_off; offs[1] = g.xvb_off;
                pcb[0] = g.cb; pcb[1] = g.cb_rc;
                ID3D11DeviceContext_IASetInputLayout(g.ctx, g.layout_rc);
                ID3D11DeviceContext_IASetPrimitiveTopology(g.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D11DeviceContext_IASetVertexBuffers(g.ctx, 0, 2, vbs, strs, offs);
                ID3D11DeviceContext_VSSetShader(g.ctx, g.vs_rc, NULL, 0);
                ID3D11DeviceContext_VSSetConstantBuffers(g.ctx, 0, 1, &g.cb);
                ID3D11DeviceContext_PSSetShader(g.ctx, rc_get_ps(&rcc), NULL, 0);
                ID3D11DeviceContext_PSSetConstantBuffers(g.ctx, 0, 2, pcb);
                ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 4, rc_srv);
                ID3D11DeviceContext_PSSetSamplers(g.ctx, 0, 4, rc_smp);
                ID3D11DeviceContext_Draw(g.ctx, count, 0);
                g.xvb_off += xb;
                g.vb_off += bytes;
                g.draws++;
                g.rc_draws++;
                g.tris += count / 3;
                return;
            }
        }
        /* a buffer would not map: fall through to the stage-0 draw */
    }
    ID3D11DeviceContext_IASetInputLayout(g.ctx, g.layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(g.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(g.ctx, 0, 1, &g.vb, &str, &off);
    ID3D11DeviceContext_VSSetShader(g.ctx, g.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g.ctx, 0, 1, &g.cb);
    ID3D11DeviceContext_PSSetShader(g.ctx, g.ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g.ctx, 0, 1, &g.cb);
    if (dc.textured) {
        ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 1, &srv);
        ID3D11DeviceContext_PSSetSamplers(g.ctx, 0, 1, &smp);
    }
    ID3D11DeviceContext_Draw(g.ctx, count, g.vb_off / stride);
    g.vb_off += bytes;
    g.draws++;
    g.tris += count / 3;
}

static void pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static void dump_tex(ID3D11Texture2D *srct, const char *name, int alpha_only)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    char path[600];
    FILE *f;
    uint32_t x, y;
    uint64_t sum[3] = {0, 0, 0}, nonblack = 0;
    uint8_t *row;

    ID3D11Texture2D_GetDesc(srct, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g.dev, &td, NULL, &st))) return;
    ID3D11DeviceContext_CopyResource(g.ctx, (ID3D11Resource *)st, (ID3D11Resource *)srct);
    if (FAILED(ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        ID3D11Texture2D_Release(st);
        return;
    }
    if (name) snprintf(path, sizeof path, "%s%s.bmp", g.dump_prefix, name);
    else snprintf(path, sizeof path, "%sd3d_%05u.bmp", g.dump_prefix, g.dump_n++);
    f = fopen(path, "wb");
    row = (uint8_t *)malloc((size_t)td.Width * 3 + 4);
    if (f && row) {
        uint32_t rowbytes = (td.Width * 3 + 3) & ~3u, imgsize = rowbytes * td.Height;
        uint8_t hdr[54] = { 'B', 'M' };
        uint32_t v;
        v = 54 + imgsize; memcpy(hdr + 2, &v, 4);
        v = 54;           memcpy(hdr + 10, &v, 4);
        v = 40;           memcpy(hdr + 14, &v, 4);
        v = td.Width;     memcpy(hdr + 18, &v, 4);
        v = td.Height;    memcpy(hdr + 22, &v, 4);
        hdr[26] = 1; hdr[28] = 24;
        memcpy(hdr + 34, &imgsize, 4);
        fwrite(hdr, 1, 54, f);
        for (y = td.Height; y-- > 0; ) {
            const uint8_t *p = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
            memset(row, 0, rowbytes);
            for (x = 0; x < td.Width; x++) {          /* source B,G,R,A -> BMP B,G,R */
                if (alpha_only) row[x * 3 + 0] = row[x * 3 + 1] = row[x * 3 + 2] = p[x * 4 + 3];
                else {
                row[x * 3 + 0] = p[x * 4 + 0];
                row[x * 3 + 1] = p[x * 4 + 1];
                row[x * 3 + 2] = p[x * 4 + 2];
                }
                sum[0] += p[x * 4 + 2]; sum[1] += p[x * 4 + 1]; sum[2] += p[x * 4 + 0];
                if (p[x * 4] | p[x * 4 + 1] | p[x * 4 + 2]) nonblack++;
            }
            fwrite(row, 1, rowbytes, f);
        }
        {
            uint64_t n = (uint64_t)td.Width * td.Height;
            logf_("dump %s flip %u mean RGB %.1f,%.1f,%.1f nonblack %.1f%%\n", path, (unsigned)g.flips,
                  (double)sum[0] / n, (double)sum[1] / n, (double)sum[2] / n,
                  100.0 * (double)nonblack / n);
        }
    }
    if (f) fclose(f);
    free(row);
    ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)st, 0);
    ID3D11Texture2D_Release(st);
}

/* ---- title-bar GPU %: process GPU utilisation from the Windows "GPU Engine"
 * performance counters (the same data Task Manager shows), read through pdh.dll
 * loaded at run time so there is no link dependency. A background thread, started
 * lazily by the first be_flip, samples once a second; be_flip only reads the result,
 * so nothing here can stall the executor. s_gpu_pct: -1 not yet known, -2 n/a. ---- */
#ifndef PDH_MORE_DATA
#define PDH_MORE_DATA ((PDH_STATUS)0x800007D2L)
#endif
#ifndef PDH_CSTATUS_VALID_DATA
#define PDH_CSTATUS_VALID_DATA 0x00000000L
#endif
static volatile LONG s_gpu_pct = -1;
static volatile LONG s_gpu_started;

typedef PDH_STATUS (WINAPI *pfn_open)(LPCWSTR, DWORD_PTR, PDH_HQUERY *);
typedef PDH_STATUS (WINAPI *pfn_addeng)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER *);
typedef PDH_STATUS (WINAPI *pfn_collect)(PDH_HQUERY);
typedef PDH_STATUS (WINAPI *pfn_fmtarr)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PDH_FMT_COUNTERVALUE_ITEM_W *);
typedef PDH_STATUS (WINAPI *pfn_close)(PDH_HQUERY);

static DWORD WINAPI gpu_pct_thread(LPVOID unused)
{
    HMODULE m;
    pfn_open p_open; pfn_addeng p_add; pfn_collect p_col; pfn_fmtarr p_fmt; pfn_close p_close;
    PDH_HQUERY q = NULL;
    PDH_HCOUNTER h = NULL;
    wchar_t path[96];
    PDH_FMT_COUNTERVALUE_ITEM_W *buf = NULL;
    DWORD bufsz = 0;
    int fails = 0;
    (void)unused;

    m = LoadLibraryA("pdh.dll");
    if (!m) goto na;
    p_open  = (pfn_open)GetProcAddress(m, "PdhOpenQueryW");
    p_add   = (pfn_addeng)GetProcAddress(m, "PdhAddEnglishCounterW");
    p_col   = (pfn_collect)GetProcAddress(m, "PdhCollectQueryData");
    p_fmt   = (pfn_fmtarr)GetProcAddress(m, "PdhGetFormattedCounterArrayW");
    p_close = (pfn_close)GetProcAddress(m, "PdhCloseQuery");
    if (!p_open || !p_add || !p_col || !p_fmt || !p_close) goto na;
    if (p_open(NULL, 0, &q) != ERROR_SUCCESS) goto na;
    /* Every engine instance of this process; the trailing '_' stops pid 123
     * matching pid 1234. */
    _snwprintf(path, 96, L"\\GPU Engine(pid_%lu_*)\\Utilization Percentage",
               (unsigned long)GetCurrentProcessId());
    path[95] = 0;
    if (p_add(q, path, 0, &h) != ERROR_SUCCESS) goto na;
    p_col(q);                                   /* rate counters need two samples */
    for (;;) {
        DWORD sz = 0, cnt = 0, i;
        double sum[16]; wchar_t typ[16][24]; int ntyp = 0, k;
        double best = 0.0;
        PDH_STATUS st;

        Sleep(1000);
        if (p_col(q) != ERROR_SUCCESS) { if (++fails > 10) goto na; continue; }
        st = p_fmt(h, PDH_FMT_DOUBLE, &sz, &cnt, NULL);
        if (st == PDH_MORE_DATA && sz) {
            if (sz > bufsz) {
                free(buf);
                buf = (PDH_FMT_COUNTERVALUE_ITEM_W *)malloc(sz);
                bufsz = buf ? sz : 0;
            }
            if (!buf) continue;
            st = p_fmt(h, PDH_FMT_DOUBLE, &sz, &cnt, buf);
        } else if (st == ERROR_SUCCESS) {
            cnt = 0;                                /* no instances (yet): 0 % */
        } else {
            if (++fails > 10) goto na;
            continue;
        }
        if (st != ERROR_SUCCESS && cnt) { if (++fails > 10) goto na; continue; }
        fails = 0;
        for (i = 0; i < cnt; i++) {
            const wchar_t *nm = buf[i].szName, *e;
            wchar_t key[24];
            int j, found = -1;
            if (buf[i].FmtValue.CStatus != ERROR_SUCCESS &&
                buf[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
            e = wcsstr(nm, L"engtype_");
            if (!e) continue;
            _snwprintf(key, 24, L"%ls", e + 8);
            key[23] = 0;
            for (j = 0; j < ntyp; j++)
                if (wcscmp(typ[j], key) == 0) { found = j; break; }
            if (found < 0 && ntyp < 16) {
                found = ntyp++;
                wcscpy(typ[found], key);
                sum[found] = 0.0;
            }
            if (found >= 0) sum[found] += buf[i].FmtValue.doubleValue;
        }
        /* Like Task Manager: the busiest engine type (3D, Copy, Compute...). */
        for (k = 0; k < ntyp; k++) if (sum[k] > best) best = sum[k];
        if (best > 100.0) best = 100.0;
        InterlockedExchange(&s_gpu_pct, (LONG)(best + 0.5));
    }
na:
    if (q && p_close) p_close(q);
    InterlockedExchange(&s_gpu_pct, -2);
    return 0;
}

static void dump_back(void)
{
    dump_tex(g.back, NULL, 0);
}

/* RECOMP_D3D11_RTDUMP=1: with the window dump, also write every render target used this frame
 * (rt<flip>_<va>_<w>x<h>.bmp, and _a.bmp with alpha as grey) so offscreen passes can be viewed. */
static void dump_rts(void)
{
    int i;
    char nm[96];
    for (i = 0; i < g.nrt; i++) {
        if (!g.rt[i].uses) continue;
        snprintf(nm, sizeof nm, "rt%u_%08X_%ux%u", g.flips, g.rt[i].va, g.rt[i].w, g.rt[i].h);
        dump_tex(g.rt[i].tex, nm, 0);
        strcat(nm, "_a");
        dump_tex(g.rt[i].tex, nm, 1);
    }
}

/* RECOMP_FLIPTIME helper: CPU time (ms) of every thread of this process, by thread id. */
#define TS_MAX 128
typedef struct { DWORD tid; uint64_t cpu100ns; } ThreadCpu;
static int thread_cpu_snapshot(ThreadCpu *out)
{
    int n = 0;
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();

    if (sn == INVALID_HANDLE_VALUE) return 0;
    te.dwSize = sizeof te;
    if (Thread32First(sn, &te)) do {
        if (te.th32OwnerProcessID == pid && n < TS_MAX) {
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            FILETIME cr, ex, kr, us;
            if (h) {
                if (GetThreadTimes(h, &cr, &ex, &kr, &us)) {
                    out[n].tid = te.th32ThreadID;
                    out[n].cpu100ns = (((uint64_t)kr.dwHighDateTime << 32) | kr.dwLowDateTime)
                                    + (((uint64_t)us.dwHighDateTime << 32) | us.dwLowDateTime);
                    n++;
                }
                CloseHandle(h);
            }
        }
    } while (Thread32Next(sn, &te));
    CloseHandle(sn);
    return n;
}

/* RECOMP_FLIPTIME helper: a poor man's sampling profiler. A thread suspends every
 * other thread of the process every few ms, reads RIP and stores (tid, rip); the
 * flip thread resolves the samples of a long interval to function names (DbgHelp,
 * loaded at run time) and prints the top ones per busy thread. */
#define SMP_MAX 60000
typedef struct { DWORD tid; uint64_t rip; } Sample;
static Sample s_smp[SMP_MAX];
static volatile LONG s_smp_n;
static volatile LONG s_smp_run;
typedef BOOL (WINAPI *PFN_SymInit)(HANDLE, PCSTR, BOOL);
typedef DWORD (WINAPI *PFN_SymSetOpt)(DWORD);
typedef BOOL (WINAPI *PFN_SymFromAddr)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
static PFN_SymFromAddr s_SymFromAddr;

static DWORD WINAPI sampler_main(LPVOID arg)
{
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    (void)arg;
    for (;;) {
        HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        if (sn != INVALID_HANDLE_VALUE) {
            te.dwSize = sizeof te;
            if (Thread32First(sn, &te)) do {
                if (te.th32OwnerProcessID == pid && te.th32ThreadID != me) {
                    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                          FALSE, te.th32ThreadID);
                    if (h) {
                        if (SuspendThread(h) != (DWORD)-1) {
                            CONTEXT cx;
                            memset(&cx, 0, sizeof cx);
                            cx.ContextFlags = CONTEXT_CONTROL;
                            if (GetThreadContext(h, &cx)) {
                                LONG k = s_smp_n;
                                if (k < SMP_MAX) {
                                    s_smp[k].tid = te.th32ThreadID;
                                    s_smp[k].rip = cx.Rip;
                                    s_smp_n = k + 1;
                                }
                            }
                            ResumeThread(h);
                        }
                        CloseHandle(h);
                    }
                }
            } while (Thread32Next(sn, &te));
            CloseHandle(sn);
        }
        Sleep(10);
    }
    return 0;
}

static void sampler_start(void)
{
    HMODULE dh = LoadLibraryA("dbghelp.dll");
    PFN_SymInit si; PFN_SymSetOpt so;
    if (s_smp_run) return;
    s_smp_run = 1;
    if (dh) {
        si = (PFN_SymInit)GetProcAddress(dh, "SymInitialize");
        so = (PFN_SymSetOpt)GetProcAddress(dh, "SymSetOptions");
        s_SymFromAddr = (PFN_SymFromAddr)GetProcAddress(dh, "SymFromAddr");
        if (so) so(0x2 /*SYMOPT_UNDNAME*/ | 0x10 /*SYMOPT_LOAD_LINES off*/ | 0x4 /*DEFERRED_LOADS*/);
        if (si) si(GetCurrentProcess(), NULL, TRUE);
    }
    CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
}

/* Print the top functions of every thread that has samples in [from, to). */
static void sampler_report(LONG from, LONG to, const ThreadCpu *busy, int nbusy)
{
    int a;
    for (a = 0; a < nbusy; a++) {
        struct { uint64_t fn; int n; char name[96]; } top[6];
        int nt = 0;
        LONG i;
        for (i = from; i < to; i++) {
            union { SYMBOL_INFO si; char pad[sizeof(SYMBOL_INFO) + 128]; } u;
            DWORD64 disp = 0, fn = s_smp[i].rip;
            int t;
            if (s_smp[i].tid != busy[a].tid) continue;
            memset(&u, 0, sizeof u);
            u.si.SizeOfStruct = sizeof(SYMBOL_INFO);
            u.si.MaxNameLen = 127;
            if (s_SymFromAddr && s_SymFromAddr(GetCurrentProcess(), s_smp[i].rip, &disp, &u.si))
                fn = u.si.Address;
            for (t = 0; t < nt; t++) if (top[t].fn == fn) break;
            if (t == nt) {
                if (nt == 6) continue;
                top[nt].fn = fn; top[nt].n = 0;
                snprintf(top[nt].name, sizeof top[nt].name, "%s",
                         (s_SymFromAddr && u.si.NameLen) ? u.si.Name : "?");
                nt++;
            }
            top[t].n++;
        }
        {
            int t, tot = 0;
            for (i = from; i < to; i++) if (s_smp[i].tid == busy[a].tid) tot++;
            logf_("FLIPT   thread %lu samples %d:", (unsigned long)busy[a].tid, tot);
            for (t = 0; t < nt; t++) logf_("  [%d] %s @%llX", top[t].n, top[t].name, (unsigned long long)top[t].fn);
            logf_("\n");
        }
    }
}

/* RECOMP_ASPECT_4_3=1: keep the game's 4:3 shape whatever the window size. The swap chain
 * follows the window's client size (ResizeBuffers when it changes, polled every flip and on
 * WM_SIZE), and the game image is drawn, filtered, into the largest centred 4:3 rectangle, with
 * black bars elsewhere. Off: the 640x480 back buffer is copied and DXGI stretches it to the
 * window (old behaviour). The FB dumps (frame_d3d_*) still write the game image itself. */
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
    toggle_fullscreen(h);
    save_fullscreen_setting(s_fs);
}

/* RECOMP_ASPECT_4_3, windowed: the window itself keeps a 4:3 client area, so there are no
 * bars; they appear only in F11 fullscreen, where the monitor's shape is fixed. */
static int aspect_window(void)
{
    return aspect_on() && !s_fs;
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

static ID3D11RenderTargetView *s_asp_rtv;
static ID3D11VertexShader *s_asp_vs;
static ID3D11PixelShader *s_asp_ps;
static ID3D11SamplerState *s_asp_smp;
static int s_asp_fail, s_asp_busy;
static RenderTarget *s_asp_last;     /* the last presented game image (valid between flips) */

static const char k_hlsl_aspect[] =
"Texture2D t0 : register(t0);\n"
"SamplerState s0 : register(s0);\n"
"struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs_a(uint id : SV_VertexID) { V o; float2 p = float2((id << 1) & 2, id & 2);\n"
"  o.uv = p; o.p = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
"float4 ps_a(V i) : SV_Target { return float4(t0.Sample(s0, i.uv).rgb, 1.0); }\n";

static int aspect_init(void)
{
    ID3DBlob *b0, *b1;
    D3D11_SAMPLER_DESC d;

    if (s_asp_vs) return 1;
    if (s_asp_fail) return 0;
    s_asp_fail = 1;
    b0 = compile_hlsl_src(k_hlsl_aspect, sizeof k_hlsl_aspect - 1, "vs_a", "vs_4_0");
    b1 = compile_hlsl_src(k_hlsl_aspect, sizeof k_hlsl_aspect - 1, "ps_a", "ps_4_0");
    memset(&d, 0, sizeof d);
    d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    if (b0 && b1 &&
        SUCCEEDED(ID3D11Device_CreateVertexShader(g.dev, ID3D10Blob_GetBufferPointer(b0), ID3D10Blob_GetBufferSize(b0), NULL, &s_asp_vs)) &&
        SUCCEEDED(ID3D11Device_CreatePixelShader(g.dev, ID3D10Blob_GetBufferPointer(b1), ID3D10Blob_GetBufferSize(b1), NULL, &s_asp_ps)) &&
        SUCCEEDED(ID3D11Device_CreateSamplerState(g.dev, &d, &s_asp_smp)))
        s_asp_fail = 0;
    if (b0) ID3D10Blob_Release(b0);
    if (b1) ID3D10Blob_Release(b1);
    if (s_asp_fail) { logf_("4:3: shader setup failed: stretching instead\n"); s_asp_vs = NULL; }
    return !s_asp_fail;
}

/* Draw r's image 4:3 into the window-sized back buffer and present. 0: not presented. */
static int present_aspect(RenderTarget *r)
{
    static int nfail;
    RECT cr;
    uint32_t cw, ch, vw, vh;
    D3D11_VIEWPORT vp;
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ID3D11ShaderResourceView *nos = NULL;
    HRESULT hr;

    if (!g.swap && !ensure_swap(r->w, r->h)) return 0;   /* first frame: window sized to the image */
    if (!aspect_init()) return 0;
    if (!GetClientRect(g.hwnd, &cr)) return 0;
    cw = (uint32_t)(cr.right - cr.left); ch = (uint32_t)(cr.bottom - cr.top);
    if (!cw || !ch) return 0;                              /* minimised */
    if (cw != g.swap_w || ch != g.swap_h || !g.back) {
        ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 0, NULL, NULL);
        if (s_asp_rtv) { ID3D11RenderTargetView_Release(s_asp_rtv); s_asp_rtv = NULL; }
        if (g.back) { ID3D11Texture2D_Release(g.back); g.back = NULL; }
        ID3D11DeviceContext_Flush(g.ctx);
        hr = IDXGISwapChain_ResizeBuffers(g.swap, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) { if (nfail++ < 5) logf_("4:3: ResizeBuffers %ux%u 0x%08lX\n", cw, ch, (unsigned long)hr); return 0; }
        hr = IDXGISwapChain_GetBuffer(g.swap, 0, &IID_ID3D11Texture2D, (void **)&g.back);
        if (FAILED(hr)) { g.back = NULL; if (nfail++ < 5) logf_("4:3: GetBuffer 0x%08lX\n", (unsigned long)hr); return 0; }
        g.swap_w = cw; g.swap_h = ch;
        logf_("4:3: swap chain %ux%u\n", cw, ch);
    }
    if (!s_asp_rtv && FAILED(ID3D11Device_CreateRenderTargetView(g.dev, (ID3D11Resource *)g.back, NULL, &s_asp_rtv)))
        { s_asp_rtv = NULL; return 0; }
    if (!r->srv && FAILED(ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)r->tex, NULL, &r->srv)))
        { r->srv = NULL; return 0; }
    vh = ch; vw = (ch * 4u + 1u) / 3u;                    /* largest 4:3 rectangle that fits */
    if (vw > cw) { vw = cw; vh = (cw * 3u + 2u) / 4u; }
    ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 1, &s_asp_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(g.ctx, s_asp_rtv, black);
    ID3D11DeviceContext_OMSetBlendState(g.ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g.ctx, NULL, 0);
    ID3D11DeviceContext_RSSetState(g.ctx, NULL);
    vp.TopLeftX = (float)((cw - vw) / 2u); vp.TopLeftY = (float)((ch - vh) / 2u);
    vp.Width = (float)vw; vp.Height = (float)vh;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g.ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(g.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g.ctx, s_asp_vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(g.ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g.ctx, s_asp_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 1, &r->srv);
    ID3D11DeviceContext_PSSetSamplers(g.ctx, 0, 1, &s_asp_smp);
    ID3D11DeviceContext_Draw(g.ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 1, &nos);
    ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 0, NULL, NULL);
    IDXGISwapChain_Present(g.swap, g.vsync ? 1 : 0, 0);
    s_asp_last = r;
    return 1;
}

/* WM_SIZE (resize, maximise, F11): redraw the last image at the new size straight away. */
static void aspect_on_size(void)
{
    if (!g.swap || !s_asp_last || s_asp_busy) return;
    s_asp_busy = 1;
    present_aspect(s_asp_last);
    s_asp_busy = 0;
}

static void be_flip(void)
{
    RenderTarget *best = NULL;
    int i;

    s_sched_di = 0;

    if (!init_window_and_device()) return;
    for (i = 0; i < g.nrt; i++) {
        RenderTarget *r = &g.rt[i];
        if (!r->uses) continue;
        if (!best || (uint64_t)r->w * r->h > (uint64_t)best->w * best->h
            || ((uint64_t)r->w * r->h == (uint64_t)best->w * best->h && r->uses > best->uses))
            best = r;
    }
    s_asp_last = NULL;
    if (best && aspect_on() && !s_asp_fail) {
        s_asp_busy = 1;
        if (g.dump_prefix && g.dump_every && (g.flips % g.dump_every) == 0)
            dump_tex(best->tex, NULL, 0);                 /* the game image, as without the switch */
        if (g.dump_prefix && g.dump_every && (g.flips % g.dump_every) == 0 && getenv("RECOMP_D3D11_RTDUMP") && getenv("RECOMP_D3D11_RTDUMP")[0] == '1')
            dump_rts();
        present_aspect(best);
        s_asp_busy = 0;
    } else if (best && ensure_swap(best->w, best->h) && g.back) {
        ID3D11DeviceContext_CopyResource(g.ctx, (ID3D11Resource *)g.back,
                                         (ID3D11Resource *)best->tex);
        if (g.dump_prefix && g.dump_every && (g.flips % g.dump_every) == 0)
            dump_back();
        if (g.dump_prefix && g.dump_every && (g.flips % g.dump_every) == 0 && getenv("RECOMP_D3D11_RTDUMP") && getenv("RECOMP_D3D11_RTDUMP")[0] == '1')
            dump_rts();
        IDXGISwapChain_Present(g.swap, g.vsync ? 1 : 0, 0);
    }
    {   /* RECOMP_FLIPTIME=<ms>: log every flip-to-flip interval longer than this, with the
         * draws and triangles it carried (diagnostic for stalls). */
        static int thr = -2; static LARGE_INTEGER pf, last; static uint32_t d_last, t_last; static uint64_t du_last, hu_last, eu_last, dn_last, un_last; static ThreadCpu cur[TS_MAX], prev[TS_MAX]; static int cur_n, prev_n; static LONG smp_mark; static uint32_t cn_last, cvn_last; static double cm_last, cvm_last;
        LARGE_INTEGER now;
        if (thr == -2) { const char *e = getenv("RECOMP_FLIPTIME"); thr = e ? atoi(e) : -1; QueryPerformanceFrequency(&pf); }
        if (thr >= 0) {
            QueryPerformanceCounter(&now);
            cur_n = thread_cpu_snapshot(cur);
            if (getenv("RECOMP_FLIPTIME_PROF") && !s_smp_run) sampler_start();
            if (last.QuadPart) {
                double ms = 1000.0 * (double)(now.QuadPart - last.QuadPart) / (double)pf.QuadPart;
                if (ms > thr)
                    logf_("FLIPT flip %u dt %.1f ms draws %u tris %u nrt %d | in be_draw %.1f ms (tex hash %.1f decode %.1f ms, %u decodes, %u uploads)\n",
                          (unsigned)g.flips, ms, (unsigned)(g.draws - d_last), (unsigned)(g.tris - t_last), g.nrt,
                          (double)(s_draw_us - du_last) / 1000.0, (double)(s_hash_us - hu_last) / 1000.0,
                          (double)(s_dec_us - eu_last) / 1000.0, (unsigned)(s_dec_n - dn_last), (unsigned)(s_up_n - un_last));
                if (ms > thr) {
                    logf_("FLIPT   D3DCompile this interval: %u shaders %.1f ms (vertex shaders %u, %.1f ms)\n",
                          (unsigned)(s_cmp_n - cn_last), s_cmp_ms - cm_last, (unsigned)(s_cmp_vs_n - cvn_last),
                          s_cmp_vs_ms - cvm_last);
                    if (sc_enabled())
                        logf_("FLIPT   shader cache totals: %u hits (%.1f ms), %u misses, %u stored\n",
                              (unsigned)s_sc_hit, s_sc_ms, (unsigned)s_sc_miss, (unsigned)s_sc_store);
                }
                if (ms > thr) {
                    int a, p, nb = 0;
                    ThreadCpu busy[16];
                    for (a = 0; a < cur_n; a++) {
                        uint64_t prev0 = 0;
                        for (p = 0; p < prev_n; p++) if (prev[p].tid == cur[a].tid) { prev0 = prev[p].cpu100ns; break; }
                        if (prev0 && cur[a].cpu100ns - prev0 > 200000)       /* > 20 ms of CPU */
                        {
                            logf_("FLIPT   thread %lu cpu %.1f ms\n", (unsigned long)cur[a].tid,
                                  (double)(cur[a].cpu100ns - prev0) / 10000.0);
                            if (nb < 16) busy[nb++] = cur[a];
                        }
                    }
                    if (s_smp_run) sampler_report(smp_mark, s_smp_n, busy, nb);
                }
            }
            last = now; d_last = g.draws; t_last = g.tris;
            memcpy(prev, cur, sizeof(ThreadCpu) * (size_t)cur_n); prev_n = cur_n;
            smp_mark = s_smp_n;
            cn_last = s_cmp_n; cvn_last = s_cmp_vs_n; cm_last = s_cmp_ms; cvm_last = s_cmp_vs_ms;
            if (s_smp_n > SMP_MAX - 2000) { s_smp_n = 0; smp_mark = 0; }
            du_last = s_draw_us; hu_last = s_hash_us; eu_last = s_dec_us; dn_last = s_dec_n; un_last = s_up_n;
        }
    }
    for (i = 0; i < g.nrt; i++) g.rt[i].uses = 0;
    g.flips++;
#ifndef NHL_RELEASE
    {
        /* FPS in the title bar, once a second: presented flips per second, which is
         * what the toolkit's own window title and its "[GPU] fps" line count. */
        static ULONGLONG t_last, cpu_last;
        static uint32_t f_last, d_last;
        static DWORD ncpu;
        ULONGLONG now = GetTickCount64();
        if (!t_last) {
            FILETIME c, e, k, u;
            t_last = now; f_last = g.flips; d_last = g.draws;
            ncpu = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
            if (!ncpu) ncpu = 1;
            if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
                cpu_last = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                           (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
            if (!InterlockedExchange(&s_gpu_started, 1)) {
                HANDLE th = CreateThread(NULL, 0, gpu_pct_thread, NULL, 0, NULL);
                if (th) CloseHandle(th); else s_gpu_pct = -2;
            }
        }
        else if (now - t_last >= 1000) {
            char tb[160], gb[16];
            double secs = (double)(now - t_last) / 1000.0;
            int cpu_pct = -1;
            LONG gp = s_gpu_pct;
            FILETIME c, e, k, u;
            /* CPU %: this process's kernel+user time over wall time, divided by the
             * logical processor count (100 % = every logical processor busy). */
            if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
                ULONGLONG cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                                (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
                double v = (double)(cpu - cpu_last) / (secs * 1e7 * (double)ncpu) * 100.0;
                cpu_pct = (int)(v + 0.5);
                if (cpu_pct > 100) cpu_pct = 100;
                cpu_last = cpu;
            }
            if (gp >= 0) snprintf(gb, sizeof gb, "%ld%%", (long)gp);
            else snprintf(gb, sizeof gb, "%s", gp == -2 ? "n/a" : "...");
            snprintf(tb, sizeof tb, "NHL Hitz Pro - D3D11 | FPS: %.1f | CPU: %d%% | GPU: %s | draws/frame: %.0f",
                     (g.flips - f_last) / secs, cpu_pct, gb,
                     (g.flips - f_last) ? (double)(g.draws - d_last) / (g.flips - f_last) : 0.0);
            SetWindowTextA(g.hwnd, tb);
            if (g.log) logf_("title: %s\n", tb);
            t_last = now; f_last = g.flips; d_last = g.draws;
        }
    }
#endif
    pump_messages();
    quit_tick();
    /* Paused: the flip does not return, so the game stops here until the overlay closes. */
    while (s_q_active) { Sleep(8); pump_messages(); quit_tick(); }
    if (g.log && (g.flips % 300) == 0)
        logf_("flips=%u t=%.1fs rcdraws=%u specs=%u specfail=%u spec_ms=%llu draws=%u tris=%u textures=%d decodes=%llu uploads=%llu decode_ms=%llu hash_ms=%llu\n",
              g.flips, (GetTickCount() - g.t0) / 1000.0, g.rc_draws, s_nspec, s_spec_fail, (unsigned long long)s_spec_ms, g.draws, g.tris, s_ntex,
              (unsigned long long)s_dec_n, (unsigned long long)s_up_n,
              (unsigned long long)(s_dec_us / 1000), (unsigned long long)(s_hash_us / 1000));
    if (g.log && s_gstate > 0 && (g.flips % 300) == 0)
        logf_("gpuvs: draws=%lu verts=%lu MB=%lu programs=%u failed=%u compile_ms=%u mapfail=%lu\n",
              s_gdraws, s_gverts, s_gbytes >> 20, s_ngspec, s_gspec_fail, s_gspec_ms, s_gmapfail);
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    uint64_t t0 = qpc_us();
    be_draw_impl(s, b);
    s_draw_us += qpc_us() - t0;
    {   /* RECOMP_D3D11_SNAP=<fromflip>,<toflip>,<step>[,<rtva hex>[,<first>]]: diagnostic. On dump flips in [fromflip,toflip)
         * write render target <rtva> (default 809AC000) after every <step>-th draw of the frame, as
         * snap_<flip>_<draw>.bmp, so one frame can be watched being built (draw index as in DLOG);
         * from draw <first> on, after every draw (<first> < 0: that many draws before the end of the previous flip). */
        static int on = -1; static unsigned f0, f1, stp, va; static int first = 0x7FFFFFFF; static uint32_t lastf = 0xFFFFFFFFu, di, prevn;
        if (on < 0) {
            const char *e = getenv("RECOMP_D3D11_SNAP");
            va = 0x809AC000u;
            on = (e && sscanf(e, "%u,%u,%u,%x,%d", &f0, &f1, &stp, &va, &first) >= 3 && stp) ? 1 : 0;
        }
        if (on && b && b->count && g.dump_prefix && g.dump_every) {
            uint32_t k = 0;
            if (g.flips != lastf) { lastf = (uint32_t)g.flips; prevn = di; di = 0; }
            k = di++;
            if ((g.flips % g.dump_every) == 0 && g.flips >= f0 && g.flips < f1 &&
                ((k % stp) == stp - 1 || (int64_t)k >= (first < 0 ? (int64_t)prevn + first : (int64_t)first))) {
                int i;
                for (i = 0; i < g.nrt; i++)
                    if (g.rt[i].va == va && g.rt[i].tex) {
                        char nm[64];
                        snprintf(nm, sizeof nm, "snap_%u_%04u", (unsigned)g.flips, (unsigned)k);
                        dump_tex(g.rt[i].tex, nm, 0);
                        break;
                    }
            }
        }
    }
}

static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip };

/* Image blit of a whole render surface into a texture's address (RECOMP_NV_BLIT=1): keep
 * a GPU copy as a render surface with the destination's address, so a draw that samples
 * that texture finds it (rt_as_texture) instead of the never-written guest bytes. */
static void be_blit(uint32_t src_va, uint32_t dst_va, uint32_t w, uint32_t h)
{
    int i;
    RenderTarget *s = NULL, *d;
    Nv2aSurface fake;

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
    if (!s) return;
    ID3D11DeviceContext_CopyResource(g.ctx, (ID3D11Resource *)d->tex, (ID3D11Resource *)s->tex);
    d->uses = 1;    /* keep it out of the eviction of unused entries */
    logf_("blit 0x%08X -> 0x%08X %ux%u flip %u\n", src_va, dst_va, w, h, g.flips);
}

/* Called from main.c when RECOMP_D3D11=1. */
void backend_d3d11_register(void)
{
    nv2a_backend_register(&s_backend);
    nv2a_blit_register(be_blit);
    fprintf(stderr, "[D3D11] back end registered (software rasteriser off)\n");
}
