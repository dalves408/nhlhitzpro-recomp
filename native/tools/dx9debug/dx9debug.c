/* dx9debug.exe: everything the NHL Hitz Pro DirectX 9 / 32-bit Windows XP port needs to know about a PC.
 * Writes dx9debug_<computer name>.txt next to the exe. Changes nothing, installs nothing.
 *
 * Built WITHOUT the C runtime (see build.bat) so the exe itself starts on Windows XP: only Win32 calls that exist
 * on XP SP2 are linked directly; every newer function is looked up with GetProcAddress and reported.
 *
 * Sections: Windows, CPU and memory (with the address-space tests the game's memory layout needs), display,
 * Direct3D 9 (caps, formats, real device), shader compiler, shader tests that draw and check pixels (the exact
 * constructs the game's generated shaders use), speed tests, audio, controllers, video codecs, Windows API
 * availability (which Vista/7 functions an XP build would have to replace), files and timers. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <stdarg.h>

#pragma warning(disable : 4996 4244 4018)
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "d3d9.lib")

/* ---- tiny runtime (no CRT) ---- */
#pragma function(memset, memcpy)
void *__cdecl memset(void *d, int c, size_t n) { __stosb((unsigned char *)d, (unsigned char)c, n); return d; }
void *__cdecl memcpy(void *d, const void *s, size_t n) { __movsb((unsigned char *)d, (const unsigned char *)s, n); return d; }
int _fltused;

static HANDLE g_out, g_con;

static void P(const char *fmt, ...)
{
    char buf[2048];
    DWORD n, w;
    va_list ap;
    va_start(ap, fmt);
    n = (DWORD)wvsprintfA(buf, fmt, ap);
    va_end(ap);
    if (g_con != INVALID_HANDLE_VALUE) WriteFile(g_con, buf, n, &w, NULL);
    if (g_out != INVALID_HANDLE_VALUE) WriteFile(g_out, buf, n, &w, NULL);
}

static void section(const char *s) { P("\r\n== %s ==\r\n", s); }

/* "123.4" for a millisecond or fraction value (wsprintf has no floats) */
static const char *fx(double v, char *b)
{
    int neg = v < 0, whole, hund;
    if (neg) v = -v;
    whole = (int)v;
    hund = (int)((v - whole) * 100.0);
    wsprintfA(b, "%s%d.%02d", neg ? "-" : "", whole, hund);
    return b;
}

/* 64-bit to double without the CRT's helper (values here are far below 2^62) */
static double i64d(LONGLONG v)
{
    DWORD hi = (DWORD)((ULONGLONG)v >> 32), lo = (DWORD)v;
    return (double)(LONG)hi * 4294967296.0 + (double)(LONG)(lo >> 1) * 2.0 + (double)(LONG)(lo & 1);
}

static double qpc_ms(LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER f)
{
    return i64d(b.QuadPart - a.QuadPart) * 1000.0 / i64d(f.QuadPart);
}

static HMODULE load_sys(const char *name) { return LoadLibraryA(name); }

static int dll_present(const char *name, char *ver, int vn)
{
    char path[MAX_PATH];
    HMODULE m = LoadLibraryExA(name, NULL, LOAD_LIBRARY_AS_DATAFILE);
    DWORD h, sz;
    if (!m) return 0;
    FreeLibrary(m);
    path[0] = 0;
    SearchPathA(NULL, name, NULL, sizeof path, path, NULL);
    if (ver) {
        ver[0] = 0;
        sz = GetFileVersionInfoSizeA(path, &h);
        if (sz && sz < 65536) {
            static BYTE buf[65536];
            VS_FIXEDFILEINFO *fi;
            UINT l;
            if (GetFileVersionInfoA(path, 0, sz, buf) && VerQueryValueA(buf, "\\", (void **)&fi, &l) && fi)
                wsprintfA(ver, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                          HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        }
    }
    (void)vn;
    return 1;
}

static void list_dlls(const char *label, const char *const *names, int n)
{
    int i;
    char ver[64];
    P("%s\r\n", label);
    for (i = 0; i < n; i++) {
        if (dll_present(names[i], ver, sizeof ver)) P("  %-24s present %s\r\n", names[i], ver);
        else P("  %-24s not found\r\n", names[i]);
    }
}

static int reg_str(HKEY root, const char *key, const char *val, char *out, DWORD n)
{
    HKEY k;
    DWORD ty = 0, sz = n;
    int ok = 0;
    if (RegOpenKeyExA(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    if (RegQueryValueExA(k, val, NULL, &ty, (BYTE *)out, &sz) == ERROR_SUCCESS && (ty == REG_SZ || ty == REG_EXPAND_SZ)) ok = 1;
    RegCloseKey(k);
    return ok;
}

static int reg_dword(HKEY root, const char *key, const char *val, DWORD *out)
{
    HKEY k;
    DWORD ty = 0, sz = sizeof *out;
    int ok = 0;
    if (RegOpenKeyExA(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    if (RegQueryValueExA(k, val, NULL, &ty, (BYTE *)out, &sz) == ERROR_SUCCESS && ty == REG_DWORD) ok = 1;
    RegCloseKey(k);
    return ok;
}

static FARPROC kproc(const char *dll, const char *fn)
{
    HMODULE m = GetModuleHandleA(dll);
    if (!m) m = LoadLibraryA(dll);
    return m ? GetProcAddress(m, fn) : NULL;
}

/* ---- Windows ---- */
static void probe_windows(void)
{
    OSVERSIONINFOEXA v;
    char s[256], cn[64];
    DWORD cl = sizeof cn, d;
    typedef BOOL (WINAPI *Wow_t)(HANDLE, PBOOL);
    Wow_t wow = (Wow_t)kproc("kernel32.dll", "IsWow64Process");
    BOOL is64 = FALSE;
    SYSTEM_INFO si;
    section("Windows");
    SecureZeroMemory(&v, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    if (!GetVersionExA((OSVERSIONINFOA *)&v)) { v.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA); GetVersionExA((OSVERSIONINFOA *)&v); }
    P("version %u.%u build %u, %s (service pack %u.%u)\r\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber,
      v.szCSDVersion[0] ? v.szCSDVersion : "no service pack", (unsigned)v.wServicePackMajor, (unsigned)v.wServicePackMinor);
    {
        typedef LONG (WINAPI *Rtl_t)(OSVERSIONINFOEXW *);
        Rtl_t rtl = (Rtl_t)kproc("ntdll.dll", "RtlGetVersion");
        OSVERSIONINFOEXW w;
        SecureZeroMemory(&w, sizeof w);
        w.dwOSVersionInfoSize = sizeof w;
        if (rtl && rtl(&w) == 0) P("true version (RtlGetVersion): %u.%u build %u\r\n", w.dwMajorVersion, w.dwMinorVersion, w.dwBuildNumber);
    }
    if (reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", s, sizeof s)) P("product: %s\r\n", s);
    if (wow) wow(GetCurrentProcess(), &is64);
    GetSystemInfo(&si);
    P("this 32-bit program runs %s; processor architecture %u\r\n", is64 ? "under 64-bit Windows (WOW64)" : "on a 32-bit Windows", (unsigned)si.wProcessorArchitecture);
    if (GetComputerNameA(cn, &cl)) P("computer name: %s\r\n", cn);
    P("remote desktop session: %d\r\n", GetSystemMetrics(SM_REMOTESESSION));
    P("system locale %04X, user locale %04X, ANSI code page %u\r\n", (unsigned)GetSystemDefaultLCID(), (unsigned)GetUserDefaultLCID(), GetACP());
    if (reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\DirectX", "Version", s, sizeof s)) P("DirectX version (registry): %s\r\n", s);
    if (reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Internet Explorer", "Version", s, sizeof s)) P("Internet Explorer: %s\r\n", s);
    if (reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\MediaPlayer\\PlayerUpgrade", "PlayerVersion", s, sizeof s)) P("Windows Media Player: %s\r\n", s);
    if (reg_dword(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\SecurityProviders\\SCHANNEL\\Protocols\\TLS 1.2\\Client", "Enabled", &d))
        P("TLS 1.2 client key present, Enabled=%u\r\n", d);
    else P("TLS 1.2 client key: not present (an installer download over https would likely fail)\r\n");
    P("boot options (SYSTEM\\CurrentControlSet\\Control SystemStartOptions): ");
    if (reg_str(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control", "SystemStartOptions", s, sizeof s)) P("%s\r\n", s); else P("(none readable)\r\n");
}

/* ---- CPU and memory ---- */
static void probe_cpu(void)
{
    int r[4], i, maxl, maxe;
    char vendor[13], brand[49];
    SYSTEM_INFO si;
    MEMORYSTATUSEX ms;
    DWORD mhz = 0;
    section("CPU and memory");
    __cpuid(r, 0);
    maxl = r[0];
    *(int *)&vendor[0] = r[1]; *(int *)&vendor[4] = r[3]; *(int *)&vendor[8] = r[2]; vendor[12] = 0;
    SecureZeroMemory(brand, sizeof brand);
    __cpuid(r, (int)0x80000000);
    maxe = r[0];
    if (maxe >= (int)0x80000004)
        for (i = 0; i < 3; i++) { __cpuid(r, (int)0x80000002 + i); memcpy(brand + i * 16, r, 16); }
    P("CPU: %s (%s)\r\n", brand[0] ? brand : "?", vendor);
    if (maxl >= 1) {
        __cpuid(r, 1);
        P("family/model/stepping: %d/%d/%d\r\n", ((r[0] >> 8) & 15) + ((r[0] >> 20) & 255), ((r[0] >> 4) & 15) | ((r[0] >> 12) & 0xF0), r[0] & 15);
        P("instruction sets: SSE %s, SSE2 %s, SSE3 %s, SSSE3 %s, SSE4.1 %s, SSE4.2 %s, POPCNT %s, AVX (CPU flag) %s\r\n",
          (r[3] >> 25) & 1 ? "yes" : "NO", (r[3] >> 26) & 1 ? "yes" : "NO", r[2] & 1 ? "yes" : "no", (r[2] >> 9) & 1 ? "yes" : "no",
          (r[2] >> 19) & 1 ? "yes" : "no", (r[2] >> 20) & 1 ? "yes" : "no", (r[2] >> 23) & 1 ? "yes" : "no", (r[2] >> 28) & 1 ? "yes" : "no");
    }
    if (maxe >= (int)0x80000001) {
        __cpuid(r, (int)0x80000001);
        P("64-bit capable CPU (long mode): %s, NX: %s\r\n", (r[3] >> 29) & 1 ? "yes" : "no", (r[3] >> 20) & 1 ? "yes" : "no");
    }
    GetSystemInfo(&si);
    P("logical processors: %u, page size %u, allocation granularity %u KB\r\n", si.dwNumberOfProcessors, si.dwPageSize, si.dwAllocationGranularity / 1024);
    P("address space of a program: %lx..%lx\r\n", (unsigned long)(ULONG_PTR)si.lpMinimumApplicationAddress, (unsigned long)(ULONG_PTR)si.lpMaximumApplicationAddress);
    if (reg_dword(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "~MHz", &mhz)) P("CPU clock (registry): %u MHz\r\n", mhz);
    SecureZeroMemory(&ms, sizeof ms);
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms))
        P("RAM: %u MB total, %u MB free; page file %u MB total, %u MB free; address space of this program %u MB, %u MB free\r\n",
          (unsigned)(ms.ullTotalPhys >> 20), (unsigned)(ms.ullAvailPhys >> 20), (unsigned)(ms.ullTotalPageFile >> 20),
          (unsigned)(ms.ullAvailPageFile >> 20), (unsigned)(ms.ullTotalVirtual >> 20), (unsigned)(ms.ullAvailVirtual >> 20));

    /* the game maps 64 MB of Xbox RAM at a fixed address and needs room for textures, render queue and buffers */
    {
        void *p;
        MEMORY_BASIC_INFORMATION mbi;
        unsigned char *a = (unsigned char *)si.lpMinimumApplicationAddress;
        unsigned largest = 0, largest_at = 0, total_free = 0, nfree = 0;
        static const unsigned sizes_mb[] = { 64, 128, 256, 512, 768, 1024 };
        P("memory layout tests (this program, not the game):\r\n");
        p = VirtualAlloc((void *)0x00010000, 64u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        P("  reserve+commit 64 MB at 0x00010000 (the game's Xbox RAM base): %s\r\n", p ? "ok" : "FAILED (this probe's own image sits at 0x00400000 inside that range; the game exe would be linked at a higher base)");
        if (p) VirtualFree(p, 0, MEM_RELEASE);
        p = VirtualAlloc((void *)0x00010000, 4u << 20, MEM_RESERVE, PAGE_READWRITE);
        P("  reserve 4 MB at 0x00010000 (below the exe image): %s\r\n", p ? "ok" : "FAILED (the low address range is not free)");
        if (p) VirtualFree(p, 0, MEM_RELEASE);
        p = VirtualAlloc((void *)0x10000000, 64u << 20, MEM_RESERVE, PAGE_READWRITE);
        P("  reserve 64 MB at 0x10000000 (a possible fixed base for the guest RAM in a 32-bit build): %s\r\n", p ? "ok" : "FAILED");
        if (p) VirtualFree(p, 0, MEM_RELEASE);
        while ((ULONG_PTR)a < (ULONG_PTR)si.lpMaximumApplicationAddress) {
            if (!VirtualQuery(a, &mbi, sizeof mbi)) break;
            if (mbi.State == MEM_FREE) {
                unsigned mb = (unsigned)(mbi.RegionSize >> 20);
                total_free += mb; nfree++;
                if (mb > largest) { largest = mb; largest_at = (unsigned)(ULONG_PTR)mbi.BaseAddress; }
            }
            a += mbi.RegionSize;
        }
        P("  free address space: %u MB in %u pieces, largest contiguous piece %u MB at 0x%08X\r\n", total_free, nfree, largest, largest_at);
        for (i = 0; i < (int)(sizeof sizes_mb / sizeof sizes_mb[0]); i++) {
            p = VirtualAlloc(NULL, (SIZE_T)sizes_mb[i] << 20, MEM_RESERVE, PAGE_READWRITE);
            P("  reserve %4u MB anywhere: %s\r\n", sizes_mb[i], p ? "ok" : "no");
            if (p) VirtualFree(p, 0, MEM_RELEASE);
        }
        {   /* is the exe large-address-aware? (header flag of this program) */
            IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)dos + dos->e_lfanew);
            P("  this exe large-address-aware flag: %s (4 GB space needs /LARGEADDRESSAWARE and, on 32-bit Windows, the /3GB boot option)\r\n",
              (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) ? "set" : "not set");
        }
    }
}

/* ---- display ---- */
static void probe_display(void)
{
    DISPLAY_DEVICEA dd;
    DEVMODEA dm;
    int i;
    section("Display");
    for (i = 0; i < 8; i++) {
        SecureZeroMemory(&dd, sizeof dd);
        dd.cb = sizeof dd;
        if (!EnumDisplayDevicesA(NULL, (DWORD)i, &dd, 0)) break;
        P("adapter %d: %s (%s), state 0x%X%s\r\n", i, dd.DeviceString, dd.DeviceName, dd.StateFlags,
          (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " [primary]" : "");
        if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
            SecureZeroMemory(&dm, sizeof dm);
            dm.dmSize = sizeof dm;
            if (EnumDisplaySettingsA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
                P("    mode %ux%u, %u bits, %u Hz\r\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel, dm.dmDisplayFrequency);
        }
    }
    P("monitors: %d, virtual screen %dx%d\r\n", GetSystemMetrics(SM_CMONITORS), GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    {
        HDC dc = GetDC(NULL);
        P("screen DPI %d, color planes bits %d\r\n", GetDeviceCaps(dc, LOGPIXELSX), GetDeviceCaps(dc, BITSPIXEL));
        ReleaseDC(NULL, dc);
    }
    P("desktop composition (Aero): %s\r\n", kproc("dwmapi.dll", "DwmIsCompositionEnabled") ? "dwmapi present (Vista or later)" : "no DWM (XP style desktop)");
}

/* ---- Direct3D 9 ---- */
static const char *fmt_name(D3DFORMAT f)
{
    switch ((int)f) {
    case D3DFMT_A8R8G8B8: return "A8R8G8B8"; case D3DFMT_X8R8G8B8: return "X8R8G8B8"; case D3DFMT_D24S8: return "D24S8";
    case D3DFMT_D24X8: return "D24X8"; case D3DFMT_D16: return "D16"; case D3DFMT_D32: return "D32"; case D3DFMT_R32F: return "R32F";
    case D3DFMT_A16B16G16R16F: return "A16B16G16R16F"; case D3DFMT_A4R4G4B4: return "A4R4G4B4"; case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_A1R5G5B5: return "A1R5G5B5"; case D3DFMT_L8: return "L8"; case D3DFMT_A8: return "A8";
    }
    switch ((unsigned)f) {
    case MAKEFOURCC('D', 'X', 'T', '1'): return "DXT1"; case MAKEFOURCC('D', 'X', 'T', '3'): return "DXT3";
    case MAKEFOURCC('D', 'X', 'T', '5'): return "DXT5"; case MAKEFOURCC('I', 'N', 'T', 'Z'): return "INTZ";
    }
    return "?";
}

static void check_fmt(IDirect3D9 *d3d, const char *what, DWORD usage, D3DRESOURCETYPE rt, D3DFORMAT f)
{
    HRESULT hr = IDirect3D9_CheckDeviceFormat(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, usage, rt, f);
    P("  %-34s %-14s %s\r\n", what, fmt_name(f), SUCCEEDED(hr) ? "yes" : "NO");
}

static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_dev;
static D3DCAPS9 g_caps;
static HWND g_wnd;
static int g_hwvp;

static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void caps_yesno(const char *name, int v) { P("  %-44s %s\r\n", name, v ? "yes" : "no"); }

static void probe_d3d9(void)
{
    D3DADAPTER_IDENTIFIER9 id;
    D3DCAPS9 *c = &g_caps;
    WNDCLASSA wc;
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    int i, n;
    DWORD flags;
    char ver[64];
    section("Direct3D 9");
    if (dll_present("d3d9.dll", ver, sizeof ver)) P("d3d9.dll %s\r\n", ver);
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) { P("Direct3DCreate9 failed: no Direct3D 9 on this PC\r\n"); return; }
    n = (int)IDirect3D9_GetAdapterCount(g_d3d);
    P("adapters: %d\r\n", n);
    for (i = 0; i < n; i++) {
        SecureZeroMemory(&id, sizeof id);
        if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(g_d3d, (UINT)i, 0, &id)))
            P("adapter %d: %s | driver %s %u.%u.%u.%u | vendor 0x%04X device 0x%04X subsys 0x%08X rev %u | WHQL %u\r\n", i, id.Description, id.Driver,
              HIWORD(id.DriverVersion.HighPart), LOWORD(id.DriverVersion.HighPart), HIWORD(id.DriverVersion.LowPart), LOWORD(id.DriverVersion.LowPart),
              id.VendorId, id.DeviceId, id.SubSysId, id.Revision, id.WHQLLevel);
    }
    SecureZeroMemory(c, sizeof *c);
    hr = IDirect3D9_GetDeviceCaps(g_d3d, 0, D3DDEVTYPE_HAL, c);
    if (FAILED(hr)) { P("no hardware (HAL) device: 0x%08X\r\n", hr); return; }
    P("pixel shader %u.%u, vertex shader %u.%u (needs 3.0 and 3.0)\r\n", D3DSHADER_VERSION_MAJOR(c->PixelShaderVersion), D3DSHADER_VERSION_MINOR(c->PixelShaderVersion),
      D3DSHADER_VERSION_MAJOR(c->VertexShaderVersion), D3DSHADER_VERSION_MINOR(c->VertexShaderVersion));
    P("shader limits: vertex constants %u, PS 3.0 instruction slots %u (executed max %u), VS 3.0 instruction slots %u (executed max %u)\r\n",
      c->MaxVertexShaderConst, c->MaxPixelShader30InstructionSlots, c->MaxPShaderInstructionsExecuted,
      c->MaxVertexShader30InstructionSlots, c->MaxVShaderInstructionsExecuted);
    P("max texture %ux%u, aspect %u, simultaneous render targets %u, max anisotropy %u, streams %u, stream stride %u, primitives per draw %u, vertex index %u\r\n",
      c->MaxTextureWidth, c->MaxTextureHeight, c->MaxTextureAspectRatio, c->NumSimultaneousRTs, c->MaxAnisotropy, c->MaxStreams, c->MaxStreamStride,
      c->MaxPrimitiveCount, c->MaxVertexIndex);
    P("texture caps: power-of-two only %s, non-power-of-two conditional %s, mipmaps %s, cube %s, square only %s\r\n",
      (c->TextureCaps & D3DPTEXTURECAPS_POW2) ? "YES" : "no", (c->TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) ? "YES (limited)" : "no",
      (c->TextureCaps & D3DPTEXTURECAPS_MIPMAP) ? "yes" : "no", (c->TextureCaps & D3DPTEXTURECAPS_CUBEMAP) ? "yes" : "no",
      (c->TextureCaps & D3DPTEXTURECAPS_SQUAREONLY) ? "YES" : "no");
    caps_yesno("hardware vertex processing (T&L)", (c->DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) != 0);
    caps_yesno("pure device", (c->DevCaps & D3DDEVCAPS_PUREDEVICE) != 0);
    caps_yesno("dynamic textures", (c->Caps2 & D3DCAPS2_DYNAMICTEXTURES) != 0);
    caps_yesno("StretchRect from textures", (c->DevCaps2 & D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES) != 0);
    caps_yesno("blend operation (min/max/subtract)", (c->PrimitiveMiscCaps & D3DPMISCCAPS_BLENDOP) != 0);
    caps_yesno("separate alpha blend", (c->PrimitiveMiscCaps & D3DPMISCCAPS_SEPARATEALPHABLEND) != 0);
    caps_yesno("independent colour write masks", (c->PrimitiveMiscCaps & D3DPMISCCAPS_INDEPENDENTWRITEMASKS) != 0);
    caps_yesno("blend factor", (c->SrcBlendCaps & D3DPBLENDCAPS_BLENDFACTOR) != 0);
    caps_yesno("scissor test", (c->RasterCaps & D3DPRASTERCAPS_SCISSORTEST) != 0);
    caps_yesno("stencil: two-sided", (c->StencilCaps & D3DSTENCILCAPS_TWOSIDED) != 0);
    caps_yesno("stencil: increment/decrement with wrap", (c->StencilCaps & D3DSTENCILCAPS_INCR) != 0);
    caps_yesno("alpha test compare functions all", (c->AlphaCmpCaps & D3DPCMPCAPS_ALWAYS) != 0);
    caps_yesno("vertex shader texture fetch (VS 3.0 vtf)", (c->VertexTextureFilterCaps & D3DPTFILTERCAPS_MINFPOINT) != 0);
    caps_yesno("dynamic flow control in vertex shaders", c->VS20Caps.DynamicFlowControlDepth > 0);
    P("  vertex shader 2.0 caps: temps %d, static flow depth %d, dynamic flow depth %d\r\n", c->VS20Caps.NumTemps, c->VS20Caps.StaticFlowControlDepth, c->VS20Caps.DynamicFlowControlDepth);
    P("  pixel shader 2.0 caps: temps %d, instruction slots %d\r\n", c->PS20Caps.NumTemps, c->PS20Caps.NumInstructionSlots);
    P("  shader 3.0 only: max vertex shader texture inputs used none; MaxVertexShader30InstructionSlots %u\r\n", c->MaxVertexShader30InstructionSlots);

    P("formats the renderer would use:\r\n");
    check_fmt(g_d3d, "render target", D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
    check_fmt(g_d3d, "render target (no alpha)", D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_X8R8G8B8);
    check_fmt(g_d3d, "depth/stencil", D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24S8);
    check_fmt(g_d3d, "depth", D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24X8);
    check_fmt(g_d3d, "depth as texture (INTZ)", D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'));
    check_fmt(g_d3d, "texture", 0, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
    check_fmt(g_d3d, "texture", 0, D3DRTYPE_TEXTURE, D3DFMT_A4R4G4B4);
    check_fmt(g_d3d, "texture", 0, D3DRTYPE_TEXTURE, D3DFMT_R5G6B5);
    check_fmt(g_d3d, "auto mipmap generation", D3DUSAGE_AUTOGENMIPMAP, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
    check_fmt(g_d3d, "texture", 0, D3DRTYPE_TEXTURE, (D3DFORMAT)MAKEFOURCC('D', 'X', 'T', '1'));
    check_fmt(g_d3d, "texture", 0, D3DRTYPE_TEXTURE, (D3DFORMAT)MAKEFOURCC('D', 'X', 'T', '5'));
    check_fmt(g_d3d, "float texture", 0, D3DRTYPE_TEXTURE, D3DFMT_R32F);
    P("  depth match (adapter X8R8G8B8, target A8R8G8B8, depth D24S8): %s\r\n",
      SUCCEEDED(IDirect3D9_CheckDepthStencilMatch(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_D24S8)) ? "yes" : "NO");
    for (i = 2; i <= 8; i *= 2) {
        DWORD q = 0;
        hr = IDirect3D9_CheckDeviceMultiSampleType(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE, (D3DMULTISAMPLE_TYPE)i, &q);
        P("  %dx multisampling: %s\r\n", i, SUCCEEDED(hr) ? "yes" : "no");
    }
    P("  windowed back buffer X8R8G8B8: %s\r\n",
      SUCCEEDED(IDirect3D9_CheckDeviceType(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, TRUE)) ? "yes" : "NO");

    SecureZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = wp; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "dx9debug"; wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    g_wnd = CreateWindowExA(0, wc.lpszClassName, "dx9debug (this window closes by itself)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60, 656, 519, NULL, NULL, wc.hInstance, NULL);
    SecureZeroMemory(&pp, sizeof pp);
    pp.BackBufferWidth = 640; pp.BackBufferHeight = 480; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_COPY; pp.hDeviceWindow = g_wnd; pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    flags = D3DCREATE_FPU_PRESERVE | D3DCREATE_HARDWARE_VERTEXPROCESSING;
    hr = IDirect3D9_CreateDevice(g_d3d, 0, D3DDEVTYPE_HAL, g_wnd, flags, &pp, &g_dev);
    g_hwvp = SUCCEEDED(hr);
    if (FAILED(hr)) {
        flags = D3DCREATE_FPU_PRESERVE | D3DCREATE_SOFTWARE_VERTEXPROCESSING;
        hr = IDirect3D9_CreateDevice(g_d3d, 0, D3DDEVTYPE_HAL, g_wnd, flags, &pp, &g_dev);
    }
    P("create device (windowed 640x480, swap copy, vsync off): %s\r\n", SUCCEEDED(hr) ? (g_hwvp ? "ok, hardware vertex processing" : "ok, SOFTWARE vertex processing only") : "FAILED");
    if (FAILED(hr)) { P("  hr=0x%08X\r\n", hr); g_dev = NULL; return; }
    P("texture memory the driver reports free: %u MB\r\n", IDirect3DDevice9_GetAvailableTextureMem(g_dev) >> 20);
}

/* ---- shader compiler ---- */
typedef HRESULT (WINAPI *D3DCompile_t)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT, void **, void **);
typedef HRESULT (WINAPI *D3DXCompileShader_t)(const char *, UINT, const void *, void *, const char *, const char *, DWORD, void **, void **, void **);
typedef struct { void **vt; } Obj;                      /* ID3DBlob / ID3DXBuffer: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize */
static void *blob_ptr(void *b) { return ((void *(WINAPI *)(void *))((Obj *)b)->vt[3])(b); }
static void blob_rel(void *b) { if (b) ((ULONG(WINAPI *)(void *))((Obj *)b)->vt[2])(b); }

static D3DCompile_t g_compile;
static D3DXCompileShader_t g_dxcompile;
static const char *g_compiler_name = "none";

static void compiler_test(const char *label, HMODULE m)
{
    static const char psrc[] = "float4 main(float4 c : COLOR0) : COLOR { return c * 0.5 + float4(0.25, 0, 0, 0); }";
    static const char vsrc[] = "float4 main(float4 p : POSITION) : POSITION { return p; }";
    D3DCompile_t comp;
    void *code = NULL, *err = NULL;
    HRESULT hr;
    if (!m) { P("  %-52s not found\r\n", label); return; }
    comp = (D3DCompile_t)GetProcAddress(m, "D3DCompile");
    if (!comp) { P("  %-52s has no D3DCompile\r\n", label); return; }
    hr = comp(psrc, sizeof psrc - 1, "t", NULL, NULL, "main", "ps_3_0", 0, 0, &code, &err);
    P("  %-52s ps_3_0 compile %s", label, SUCCEEDED(hr) ? "ok" : "FAILED");
    if (SUCCEEDED(hr) && g_dev) {
        IDirect3DPixelShader9 *ps = NULL;
        HRESULT h2 = IDirect3DDevice9_CreatePixelShader(g_dev, (const DWORD *)blob_ptr(code), &ps);
        P(", create %s", SUCCEEDED(h2) ? "ok" : "FAILED");
        if (ps) IDirect3DPixelShader9_Release(ps);
    }
    blob_rel(code); blob_rel(err); code = err = NULL;
    hr = comp(vsrc, sizeof vsrc - 1, "t", NULL, NULL, "main", "vs_3_0", 0, 0, &code, &err);
    P(", vs_3_0 %s\r\n", SUCCEEDED(hr) ? "ok" : "FAILED");
    blob_rel(code); blob_rel(err);
}

static void probe_compilers(void)
{
    static const char *const d3dc[] = { "d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_45.dll", "d3dcompiler_44.dll", "d3dcompiler_43.dll", "d3dcompiler_42.dll", "d3dcompiler_41.dll", "d3dcompiler_40.dll", "d3dcompiler_39.dll" };
    static const char *const d3dx[] = { "d3dx9_43.dll", "d3dx9_42.dll", "d3dx9_41.dll", "d3dx9_40.dll", "d3dx9_39.dll", "d3dx9_38.dll", "d3dx9_37.dll", "d3dx9_36.dll", "d3dx9_35.dll", "d3dx9_34.dll", "d3dx9_33.dll", "d3dx9_32.dll", "d3dx9_31.dll", "d3dx9_30.dll", "d3dx9_29.dll", "d3dx9_28.dll", "d3dx9_27.dll", "d3dx9_26.dll", "d3dx9_25.dll", "d3dx9_24.dll" };
    char ver[64], path[MAX_PATH];
    int i;
    section("Shader compiler (the game builds its combiner shaders at run time)");
    for (i = 0; i < (int)(sizeof d3dc / sizeof d3dc[0]); i++) {
        if (dll_present(d3dc[i], ver, sizeof ver)) {
            P("  %-22s present %s\r\n", d3dc[i], ver);
            if (!g_compile) {
                HMODULE m = LoadLibraryA(d3dc[i]);
                if (m && (g_compile = (D3DCompile_t)GetProcAddress(m, "D3DCompile")) != NULL) g_compiler_name = d3dc[i];
            }
        }
    }
    for (i = 0; i < (int)(sizeof d3dx / sizeof d3dx[0]); i++) {
        if (dll_present(d3dx[i], ver, sizeof ver)) {
            P("  %-22s present %s\r\n", d3dx[i], ver);
            if (!g_dxcompile) {
                HMODULE m = LoadLibraryA(d3dx[i]);
                if (m && (g_dxcompile = (D3DXCompileShader_t)GetProcAddress(m, "D3DXCompileShader")) != NULL && !g_compile) g_compiler_name = d3dx[i];
            }
        }
    }
    (void)path;
    {
        char own[MAX_PATH];
        int k;
        HMODULE a;
        GetModuleFileNameA(NULL, own, sizeof own);
        for (k = lstrlenA(own) - 1; k > 0 && own[k] != '\\'; k--) ;
        own[k + 1] = 0;
        lstrcatA(own, "d3dcompiler_47.dll");
        P("compile and create-shader tests per location:\r\n");
        a = LoadLibraryA(own);
        compiler_test("d3dcompiler_47.dll beside this program", a);
        a = LoadLibraryA("d3dcompiler_47.dll");
        compiler_test("d3dcompiler_47.dll from the search path", a);
        a = LoadLibraryA("d3dcompiler_43.dll");
        compiler_test("d3dcompiler_43.dll (DirectX June 2010 runtime)", a);
    }
    P("compiler used for the tests below: %s%s\r\n", g_compiler_name, g_compile ? " (D3DCompile)" : (g_dxcompile ? " (D3DXCompileShader)" : ""));
    if (!g_compile && !g_dxcompile) P("  NO SHADER COMPILER: the game would need d3dcompiler_43.dll (DirectX June 2010 runtime) or its own copy\r\n");
}

static void *compile(const char *src, const char *entry, const char *profile, char *err, int errn)
{
    void *code = NULL, *e = NULL;
    HRESULT hr = E_FAIL;
    UINT len = 0;
    while (src[len]) len++;
    if (g_compile) hr = g_compile(src, len, "t", NULL, NULL, entry, profile, 0, 0, &code, &e);
    else if (g_dxcompile) hr = g_dxcompile(src, len, NULL, NULL, entry, profile, 0, &code, &e, NULL);
    if (FAILED(hr)) {
        if (err && e) { const char *t = (const char *)blob_ptr(e); int k = 0; while (t[k] && k < errn - 1) { err[k] = t[k]; k++; } err[k] = 0; }
        else if (err) err[0] = 0;
        blob_rel(code); code = NULL;
    }
    blob_rel(e);
    return code;
}

/* ---- shader tests: draw a quad into a 64x64 target and read the centre pixel ---- */
static IDirect3DSurface9 *g_rt, *g_ds, *g_sys;
static IDirect3DTexture9 *g_rt_tex;

static int tests_setup(void)
{
    if (!g_dev) return 0;
    if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, 64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_rt_tex, NULL))) return 0;
    IDirect3DTexture9_GetSurfaceLevel(g_rt_tex, 0, &g_rt);
    IDirect3DDevice9_CreateDepthStencilSurface(g_dev, 64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_ds, NULL);
    IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, 64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_sys, NULL);
    return g_rt && g_sys;
}

static const D3DVERTEXELEMENT9 k_el_pos[] = { { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 }, D3DDECL_END() };
static const float k_quad[6][4] = { { -1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 }, { -1, -1, 0, 1 }, { 1, 1, 0, 1 }, { 1, -1, 0, 1 } };

static DWORD read_center(void)
{
    D3DLOCKED_RECT lr;
    DWORD px = 0;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(g_dev, g_rt, g_sys))) return 0xDEADBEEF;
    if (SUCCEEDED(IDirect3DSurface9_LockRect(g_sys, &lr, NULL, D3DLOCK_READONLY))) {
        px = ((DWORD *)((BYTE *)lr.pBits + 32 * lr.Pitch))[32];
        IDirect3DSurface9_UnlockRect(g_sys);
    }
    return px;
}

/* one drawing test. vsrc/psrc: HLSL with entries "vs"/"ps"; vc/pc: constants set before the draw. Result: centre pixel, or 0xDEADBEEF on failure. */
static DWORD run_draw(const char *vsrc, const char *psrc, const float *vc, int nvc, const float *pc, int npc,
                      IDirect3DVertexDeclaration9 *decl, const void *vdata, UINT vstride, UINT nvert, char *why, int whyn)
{
    void *vb = NULL, *pb = NULL;
    IDirect3DVertexShader9 *vs = NULL;
    IDirect3DPixelShader9 *ps = NULL;
    IDirect3DVertexDeclaration9 *d = decl;
    HRESULT hr;
    DWORD px = 0xDEADBEEF;
    why[0] = 0;
    vb = compile(vsrc, "vs", "vs_3_0", why, whyn);
    if (!vb) { if (!why[0]) lstrcpynA(why, "vertex shader did not compile (no compiler?)", whyn); return px; }
    pb = compile(psrc, "ps", "ps_3_0", why, whyn);
    if (!pb) { if (!why[0]) lstrcpynA(why, "pixel shader did not compile", whyn); blob_rel(vb); return px; }
    hr = IDirect3DDevice9_CreateVertexShader(g_dev, (const DWORD *)blob_ptr(vb), &vs);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreatePixelShader(g_dev, (const DWORD *)blob_ptr(pb), &ps);
    blob_rel(vb); blob_rel(pb);
    if (FAILED(hr)) { wsprintfA(why, "CreateShader failed 0x%08X", hr); goto done; }
    if (!d) IDirect3DDevice9_CreateVertexDeclaration(g_dev, k_el_pos, &d);
    IDirect3DDevice9_SetRenderTarget(g_dev, 0, g_rt);
    IDirect3DDevice9_SetDepthStencilSurface(g_dev, NULL);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    IDirect3DDevice9_SetVertexShader(g_dev, vs);
    IDirect3DDevice9_SetPixelShader(g_dev, ps);
    IDirect3DDevice9_SetVertexDeclaration(g_dev, d);
    if (nvc) IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 0, vc, (UINT)nvc);
    if (npc) IDirect3DDevice9_SetPixelShaderConstantF(g_dev, 0, pc, (UINT)npc);
    IDirect3DDevice9_BeginScene(g_dev);
    hr = IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, nvert / 3, vdata ? vdata : k_quad, vdata ? vstride : 16);
    IDirect3DDevice9_EndScene(g_dev);
    if (FAILED(hr)) { wsprintfA(why, "draw failed 0x%08X", hr); goto done; }
    px = read_center();
done:
    if (d && d != decl) IDirect3DVertexDeclaration9_Release(d);
    if (vs) IDirect3DVertexShader9_Release(vs);
    if (ps) IDirect3DPixelShader9_Release(ps);
    return px;
}

static void report(const char *name, DWORD px, DWORD want, int tol, const char *why)
{
    int i, ok = px != 0xDEADBEEF;
    if (ok)
        for (i = 0; i < 24; i += 8) {
            int a = (int)((px >> i) & 255), b = (int)((want >> i) & 255), dd = a > b ? a - b : b - a;
            if (dd > tol) ok = 0;
        }
    if (px == 0xDEADBEEF) P("  %-50s FAILED: %s\r\n", name, why[0] ? why : "?");
    else P("  %-50s %s (pixel %08X, expected %08X)\r\n", name, ok ? "PASS" : "WRONG RESULT", px, want);
}

static const char k_ps_pass[] = "float4 ps(float4 c : COLOR0) : COLOR { return c; }\n";

static void shader_tests(void)
{
    static char why[512], src[262144], b1[32], b2[32];
    static float vc[200 * 4], pc[24 * 4];
    DWORD px;
    int i;
    section("Shader tests (draw into a render target, read the pixel back)");
    if (!g_dev) { P("  no device: skipped\r\n"); return; }
    if (!g_compile && !g_dxcompile) { P("  no shader compiler: skipped\r\n"); return; }
    if (!tests_setup()) { P("  could not create the test render target: skipped\r\n"); return; }

    /* 1: relative constant addressing, the way generated vertex programs read c[a0 + n] */
    for (i = 0; i < 16; i++) { vc[i * 4] = (float)i / 15.0f; vc[i * 4 + 1] = vc[i * 4 + 2] = 0; vc[i * 4 + 3] = 1; }
    vc[16 * 4] = 2.0f; vc[16 * 4 + 1] = vc[16 * 4 + 2] = vc[16 * 4 + 3] = 0;
    px = run_draw("float4 KC[16] : register(c0); float4 P : register(c16);\n"
                  "struct VO { float4 p : POSITION; float4 c : COLOR0; };\n"
                  "VO vs(float4 pos : POSITION) { VO o; int a0 = (int)floor(P.x + 0.001); o.c = KC[clamp(3 + a0, 0, 15)]; o.p = pos; return o; }\n",
                  k_ps_pass, vc, 17, NULL, 0, NULL, NULL, 0, 6, why, sizeof why);
    report("vs_3_0 relative constant addressing c[a0+3]", px, 0xFF550000, 3, why);

    /* 2: 16 vertex inputs TEXCOORD0..15 summed (the game's generated shaders read up to 16 attributes) */
    {
        static const D3DVERTEXELEMENT9 el16[] = {
            { 0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION, 0 },
            { 0, 16, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 0 }, { 0, 32, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 1 },
            { 0, 48, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 2 }, { 0, 64, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 3 },
            { 0, 80, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 4 }, { 0, 96, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 5 },
            { 0, 112, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 6 }, { 0, 128, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 7 },
            { 0, 144, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 8 }, { 0, 160, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 9 },
            { 0, 176, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 10 }, { 0, 192, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 11 },
            { 0, 208, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 12 }, { 0, 224, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 13 },
            { 0, 240, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 14 },
            D3DDECL_END() };
        static float vtx[6][64];
        IDirect3DVertexDeclaration9 *d16 = NULL;
        int v, k;
        char *s = src;
        for (v = 0; v < 6; v++) {
            for (k = 0; k < 4; k++) vtx[v][k] = k_quad[v][k];
            for (k = 0; k < 15; k++) { vtx[v][4 + k * 4] = 1.0f / 64.0f; vtx[v][5 + k * 4] = vtx[v][6 + k * 4] = 0; vtx[v][7 + k * 4] = 0; }
        }
        s += wsprintfA(s, "struct VI { float4 p : POSITION;");
        for (k = 0; k < 15; k++) s += wsprintfA(s, " float4 v%d : TEXCOORD%d;", k, k);
        s += wsprintfA(s, " };\nstruct VO { float4 p : POSITION; float4 c : COLOR0; };\nVO vs(VI i) { VO o; float4 a = float4(0,0,0,1);\n");
        for (k = 0; k < 15; k++) s += wsprintfA(s, " a.x += i.v%d.x;\n", k);
        s += wsprintfA(s, " o.c = a; o.p = i.p; return o; }\n");
        IDirect3DDevice9_CreateVertexDeclaration(g_dev, el16, &d16);
        px = run_draw(src, k_ps_pass, NULL, 0, NULL, 0, d16, vtx, 256, 6, why, sizeof why);
        report("vs_3_0 position + 15 float4 inputs (all 16 registers)", px, 0xFF3C0000, 3, why);
        if (d16) IDirect3DVertexDeclaration9_Release(d16);
    }

    /* 3: arithmetic the generated vertex programs use: rcp of zero giving +infinity, exp2/log2/pow/rsqrt, compares, NaN test */
    {
        float z[4] = { 0, 0, 0, 0 };
        px = run_draw("float4 CS : register(c0);\n"
                      "struct VO { float4 p : POSITION; float4 c : COLOR0; };\n"
                      "VO vs(float4 pos : POSITION) { VO o; float inf = 1.0 / CS.w;\n"
                      "  float r = (inf > 1e30) ? 1.0 : 0.0;\n"
                      "  float g = (abs(pow(2.0, 3.0) - 8.0) < 0.01 && abs(log2(8.0) - 3.0) < 0.01 && abs(rsqrt(4.0) - 0.5) < 0.01) ? 1.0 : 0.0;\n"
                      "  float b = 1.0;\n"
                      "  o.c = float4(r, g, b, 1.0); o.p = pos; return o; }\n",
                      k_ps_pass, z, 1, NULL, 0, NULL, NULL, 0, 6, why, sizeof why);
        report("vs_3_0 1/0 = +inf, exp2/log2/pow/rsqrt", px, 0xFFFFFFFF, 3, why);
        P("    (red = divide by zero gives +infinity, green = exp2/log2/pow/rsqrt)\r\n");
    }

    /* 4: dynamic branch on a float constant (fog mode, etc.) */
    {
        float k[8] = { 2048, 0, 0, 0, 0, 0, 0, 0 };
        px = run_draw("float4 CG : register(c0);\n"
                      "struct VO { float4 p : POSITION; float4 c : COLOR0; };\n"
                      "VO vs(float4 pos : POSITION) { VO o; float4 c = float4(0, 0, 1, 1);\n"
                      "  if (CG.x == 2048.0) c = float4(0, 1, 0, 1); else if (CG.x == 2049.0) c = float4(1, 0, 0, 1);\n"
                      "  o.c = c; o.p = pos; return o; }\n",
                      k_ps_pass, k, 1, NULL, 0, NULL, NULL, 0, 6, why, sizeof why);
        report("vs_3_0 if / else on a float constant", px, 0xFF00FF00, 3, why);
    }

    /* 5: pixel shader with 4 samplers, projective divide, many constants and saturate (the combiner shader's shape) */
    {
        IDirect3DTexture9 *t[4];
        int k;
        float sc[24 * 4];
        static const DWORD col[4] = { 0xFF800000, 0xFF004000, 0xFF000020, 0xFF101010 };
        DWORD ok = 1;
        for (k = 0; k < 4; k++) {
            t[k] = NULL;
            if (SUCCEEDED(IDirect3DDevice9_CreateTexture(g_dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t[k], NULL))) {
                D3DLOCKED_RECT lr; int x, y;
                if (SUCCEEDED(IDirect3DTexture9_LockRect(t[k], 0, &lr, NULL, 0))) {
                    for (y = 0; y < 4; y++)
                        for (x = 0; x < 4; x++) ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = col[k];
                    IDirect3DTexture9_UnlockRect(t[k], 0);
                }
                IDirect3DDevice9_SetTexture(g_dev, (DWORD)k, (IDirect3DBaseTexture9 *)t[k]);
            } else ok = 0;
        }
        for (k = 0; k < 24 * 4; k++) sc[k] = 0;
        sc[4 * 7] = 1; sc[4 * 7 + 1] = 1; sc[4 * 7 + 2] = 1; sc[4 * 7 + 3] = 1;
        for (k = 0; k < 4; k++) { sc[k * 4] = 1.0f; sc[k * 4 + 1] = 1.0f; sc[k * 4 + 2] = 1.0f; }
        px = run_draw("struct VO { float4 p : POSITION; float4 t : TEXCOORD2; float4 c : COLOR0; };\n"
                      "VO vs(float4 pos : POSITION) { VO o; o.p = pos; o.t = float4(0.5, 0.5, 0, 1); o.c = float4(1, 1, 1, 1); return o; }\n",
                      "sampler2D s0 : register(s0); sampler2D s1 : register(s1); sampler2D s2 : register(s2); sampler2D s3 : register(s3);\n"
                      "float4 K[24] : register(c0);\n"
                      "struct PI { float4 c : COLOR0; float4 t0 : TEXCOORD2; };\n"
                      "float4 ps(PI i) : COLOR {\n"
                      "  float4 a = tex2D(s0, i.t0.xy / ((i.t0.w != 0.0) ? i.t0.w : 1.0) * K[0].xy);\n"
                      "  float4 b = tex2D(s1, i.t0.xy * K[1].xy); float4 c = tex2D(s2, i.t0.xy * K[2].xy); float4 d = tex2D(s3, i.t0.xy * K[3].xy);\n"
                      "  float4 o = saturate(a * K[7] + b * K[7] + c * K[7] + d * K[7]);\n"
                      "  return o; }\n",
                      NULL, 0, sc, 24, NULL, NULL, 0, 6, why, sizeof why);
        /* the draw above sets the vertex shader only; sampler setup done before it persists */
        report("ps_3_0 four samplers, projective divide, constants", px, 0xFF905030, 8, why);
        for (k = 0; k < 4; k++) { IDirect3DDevice9_SetTexture(g_dev, (DWORD)k, NULL); if (t[k]) IDirect3DTexture9_Release(t[k]); }
        (void)ok;
    }

    /* 6: how many instructions a shader can have: unrolled multiply-add chains of growing length */
    P("  instruction-count limits (a chain of N dependent multiply-adds; the game's longest generated programs have several hundred):\r\n");
    {
        static const int ns[] = { 128, 256, 512, 1024, 2048, 4096 };
        int k;
        for (k = 0; k < (int)(sizeof ns / sizeof ns[0]); k++) {
            void *vb, *pb;
            IDirect3DVertexShader9 *vs = NULL;
            IDirect3DPixelShader9 *ps = NULL;
            HRESULT h1 = E_FAIL, h2 = E_FAIL;
            {
                char *w = src;
                int q;
                w += wsprintfA(w, "float4 K : register(c0);\nfloat4 vs(float4 p : POSITION) : POSITION { float4 r = p;\n");
                for (q = 0; q < ns[k]; q++) w += wsprintfA(w, "r = r * K + K.yxwz;\n");
                wsprintfA(w, "return r; }\n");
            }
            why[0] = 0;
            vb = compile(src, "vs", "vs_3_0", why, 256);
            if (vb) { h1 = IDirect3DDevice9_CreateVertexShader(g_dev, (const DWORD *)blob_ptr(vb), &vs); blob_rel(vb); }
            {
                char *w = src;
                int q;
                w += wsprintfA(w, "float4 K : register(c0);\nfloat4 ps(float4 c : COLOR0) : COLOR { float4 r = c;\n");
                for (q = 0; q < ns[k]; q++) w += wsprintfA(w, "r = r * K + K.yxwz;\n");
                wsprintfA(w, "return r; }\n");
            }
            pb = compile(src, "ps", "ps_3_0", why + 256, 256);
            if (pb) { h2 = IDirect3DDevice9_CreatePixelShader(g_dev, (const DWORD *)blob_ptr(pb), &ps); blob_rel(pb); }
            P("    N=%-5d vertex shader: %s | pixel shader: %s\r\n", ns[k], vb ? (SUCCEEDED(h1) ? "ok" : "driver refused") : "did not compile",
              pb ? (SUCCEEDED(h2) ? "ok" : "driver refused") : "did not compile");
            if (!vb && why[0]) P("      vertex compiler said: %.110s\r\n", why);
            if (!pb && why[256]) P("      pixel compiler said: %.110s\r\n", why + 256);
            if (vs) IDirect3DVertexShader9_Release(vs);
            if (ps) IDirect3DPixelShader9_Release(ps);
        }
    }

    /* 7: two vertex streams + dynamic vertex buffer with DISCARD/NOOVERWRITE (how the renderer feeds vertices) */
    {
        IDirect3DVertexBuffer9 *vb = NULL;
        void *p;
        HRESULT h1 = IDirect3DDevice9_CreateVertexBuffer(g_dev, 1u << 20, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, NULL), h2 = E_FAIL, h3 = E_FAIL;
        if (SUCCEEDED(h1)) {
            h2 = IDirect3DVertexBuffer9_Lock(vb, 0, 4096, &p, D3DLOCK_DISCARD);
            if (SUCCEEDED(h2)) IDirect3DVertexBuffer9_Unlock(vb);
            h3 = IDirect3DVertexBuffer9_Lock(vb, 4096, 4096, &p, D3DLOCK_NOOVERWRITE);
            if (SUCCEEDED(h3)) IDirect3DVertexBuffer9_Unlock(vb);
            IDirect3DVertexBuffer9_Release(vb);
        }
        P("  %-50s %s\r\n", "dynamic vertex buffer: lock DISCARD / NOOVERWRITE", (SUCCEEDED(h1) && SUCCEEDED(h2) && SUCCEEDED(h3)) ? "PASS" : "FAILED");
    }
    (void)fx; (void)b1; (void)b2;
}

/* ---- speed tests ---- */
static void speed_tests(void)
{
    LARGE_INTEGER f, t0, t1;
    char b[32];
    int i, n;
    section("Speed tests (this is what decides whether a match is playable)");
    if (!g_dev || !g_rt) { P("  no device: skipped\r\n"); return; }
    QueryPerformanceFrequency(&f);
    {   /* a: texture uploads, like the game re-uploading changed textures */
        IDirect3DTexture9 *t = NULL;
        D3DLOCKED_RECT lr;
        static DWORD px[512 * 512];
        if (SUCCEEDED(IDirect3DDevice9_CreateTexture(g_dev, 512, 512, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL))) {
            QueryPerformanceCounter(&t0);
            for (i = 0; i < 100; i++)
                if (SUCCEEDED(IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0))) {
                    int y;
                    for (y = 0; y < 512; y++) memcpy((BYTE *)lr.pBits + y * lr.Pitch, px + y * 512, 512 * 4);
                    IDirect3DTexture9_UnlockRect(t, 0);
                }
            QueryPerformanceCounter(&t1);
            P("  upload of a 512x512 texture (managed, LockRect copy): %s ms each\r\n", fx(qpc_ms(t0, t1, f) / 100.0, b));
            IDirect3DTexture9_Release(t);
        }
    }
    {   /* b: fill rate, full-target alpha-blended quads in a 640x480 target */
        IDirect3DTexture9 *rt640 = NULL;
        IDirect3DSurface9 *s640 = NULL, *sysd = NULL;
        void *pb;
        IDirect3DPixelShader9 *ps = NULL;
        n = 60;
        if (SUCCEEDED(IDirect3DDevice9_CreateTexture(g_dev, 640, 480, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rt640, NULL))) {
            IDirect3DTexture9_GetSurfaceLevel(rt640, 0, &s640);
            pb = compile("float4 ps(float4 c : COLOR0) : COLOR { return float4(0.1, 0.1, 0.1, 0.5); }\n", "ps", "ps_3_0", NULL, 0);
            if (pb) { IDirect3DDevice9_CreatePixelShader(g_dev, (const DWORD *)blob_ptr(pb), &ps); blob_rel(pb); }
            if (ps) {
                void *vb = compile("float4 vs(float4 p : POSITION) : POSITION { return p; }\n", "vs", "vs_3_0", NULL, 0);
                IDirect3DVertexShader9 *vs = NULL;
                IDirect3DVertexDeclaration9 *d = NULL;
                if (vb) { IDirect3DDevice9_CreateVertexShader(g_dev, (const DWORD *)blob_ptr(vb), &vs); blob_rel(vb); }
                IDirect3DDevice9_CreateVertexDeclaration(g_dev, k_el_pos, &d);
                IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, 640, 480, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysd, NULL);
                if (vs && d && sysd) {
                    IDirect3DDevice9_SetRenderTarget(g_dev, 0, s640);
                    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE, FALSE);
                    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, TRUE);
                    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
                    IDirect3DDevice9_SetVertexShader(g_dev, vs); IDirect3DDevice9_SetPixelShader(g_dev, ps); IDirect3DDevice9_SetVertexDeclaration(g_dev, d);
                    IDirect3DDevice9_BeginScene(g_dev);
                    IDirect3DDevice9_GetRenderTargetData(g_dev, s640, sysd);           /* drain the pipeline before timing */
                    QueryPerformanceCounter(&t0);
                    for (i = 0; i < n; i++) IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, 2, k_quad, 16);
                    IDirect3DDevice9_EndScene(g_dev);
                    IDirect3DDevice9_GetRenderTargetData(g_dev, s640, sysd);
                    QueryPerformanceCounter(&t1);
                    P("  fill: %d full-target (640x480) alpha-blended quads: %s ms (%s ms per quad; a match draws well over 10 per frame)\r\n", n,
                      fx(qpc_ms(t0, t1, f), b), fx(qpc_ms(t0, t1, f) / n, b + 16));
                }
                if (vs) IDirect3DVertexShader9_Release(vs);
                if (d) IDirect3DVertexDeclaration9_Release(d);
                IDirect3DPixelShader9_Release(ps);
            }
            if (sysd) IDirect3DSurface9_Release(sysd);
            if (s640) IDirect3DSurface9_Release(s640);
            IDirect3DTexture9_Release(rt640);
        }
    }
    {   /* c: draw-call rate and vertex throughput with a heavier vertex shader */
        void *vb = compile("float4 K[8] : register(c0);\nfloat4 vs(float4 p : POSITION) : POSITION { float4 r = p; [unroll] for (int i = 0; i < 40; i++) r = r * K[i % 8] + K[(i + 3) % 8]; return float4(p.xy * 0.001 + r.xy * 0.0, 0, 1); }\n", "vs", "vs_3_0", NULL, 0);
        void *pb = compile("float4 ps(float4 c : COLOR0) : COLOR { return float4(0.2, 0.2, 0.2, 1); }\n", "ps", "ps_3_0", NULL, 0);
        IDirect3DVertexShader9 *vs = NULL; IDirect3DPixelShader9 *ps = NULL; IDirect3DVertexDeclaration9 *d = NULL;
        static float tris[3 * 300][4];
        float kk[8 * 4];
        if (vb) { IDirect3DDevice9_CreateVertexShader(g_dev, (const DWORD *)blob_ptr(vb), &vs); blob_rel(vb); }
        if (pb) { IDirect3DDevice9_CreatePixelShader(g_dev, (const DWORD *)blob_ptr(pb), &ps); blob_rel(pb); }
        IDirect3DDevice9_CreateVertexDeclaration(g_dev, k_el_pos, &d);
        for (i = 0; i < 3 * 300; i++) { tris[i][0] = ((i * 37) % 200) / 100.0f - 1.0f; tris[i][1] = ((i * 91) % 200) / 100.0f - 1.0f; tris[i][2] = 0; tris[i][3] = 1; }
        for (i = 0; i < 32; i++) kk[i] = 1.0f;
        if (vs && ps && d) {
            int draws = 2000;
            IDirect3DDevice9_SetRenderTarget(g_dev, 0, g_rt);
            IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, FALSE);
            IDirect3DDevice9_SetVertexShader(g_dev, vs); IDirect3DDevice9_SetPixelShader(g_dev, ps); IDirect3DDevice9_SetVertexDeclaration(g_dev, d);
            IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 0, kk, 8);
            IDirect3DDevice9_BeginScene(g_dev);
            read_center();
            QueryPerformanceCounter(&t0);
            for (i = 0; i < draws; i++) IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, 300, tris, 16);
            IDirect3DDevice9_EndScene(g_dev);
            read_center();
            QueryPerformanceCounter(&t1);
            P("  vertex work: %d draws of 300 triangles (40-instruction vertex shader, user-pointer vertices): %s ms = %s million vertices/s, %s thousand draw calls/s\r\n",
              draws, fx(qpc_ms(t0, t1, f), b), fx(draws * 900.0 / qpc_ms(t0, t1, f) / 1000.0, b + 16), fx(draws / qpc_ms(t0, t1, f), b + 8));
        } else P("  vertex work test: shaders could not be created\r\n");
        if (vs) IDirect3DVertexShader9_Release(vs);
        if (ps) IDirect3DPixelShader9_Release(ps);
        if (d) IDirect3DVertexDeclaration9_Release(d);
    }
    {   /* d: present rate, 640x480 windowed, vsync off */
        QueryPerformanceCounter(&t0);
        n = 120;
        for (i = 0; i < n; i++) {
            MSG m;
            IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000u | (DWORD)(i * 2), 1.0f, 0);
            IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
            while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
        }
        QueryPerformanceCounter(&t1);
        P("  present: clear + Present 640x480, %d frames: %s fps\r\n", n, fx(n * 1000.0 / qpc_ms(t0, t1, f), b));
    }
}

/* ---- audio ---- */
static void probe_audio(void)
{
    static const char *const x2[] = { "xaudio2_9.dll", "xaudio2_8.dll", "xaudio2_7.dll", "xaudio2_6.dll", "xaudio2_5.dll", "xaudio2_4.dll", "xaudio2_3.dll", "xaudio2_2.dll", "xaudio2_1.dll", "xaudio2_0.dll", "dsound.dll", "mmdevapi.dll" };
    UINT n, i;
    section("Audio");
    n = waveOutGetNumDevs();
    P("waveOut devices: %u\r\n", n);
    for (i = 0; i < n; i++) {
        WAVEOUTCAPSA c;
        if (waveOutGetDevCapsA(i, &c, sizeof c) == MMSYSERR_NOERROR) P("  %u: %s (formats 0x%X, %u channels)\r\n", i, c.szPname, c.dwFormats, c.wChannels);
    }
    list_dlls("audio libraries:", x2, (int)(sizeof x2 / sizeof x2[0]));
    {   /* the game's own fallback path: 48 kHz 16-bit stereo through waveOut, with its clock checked */
        HWAVEOUT h;
        WAVEFORMATEX wf;
        WAVEHDR hdr[4];
        static short buf[4][512 * 2];
        MMTIME mt;
        int k;
        DWORD s0;
        SecureZeroMemory(&wf, sizeof wf);
        wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = 48000; wf.wBitsPerSample = 16; wf.nBlockAlign = 4; wf.nAvgBytesPerSec = 48000 * 4;
        if (waveOutOpen(&h, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
            for (k = 0; k < 4; k++) {
                SecureZeroMemory(&hdr[k], sizeof hdr[k]);
                hdr[k].lpData = (LPSTR)buf[k]; hdr[k].dwBufferLength = sizeof buf[k];
                waveOutPrepareHeader(h, &hdr[k], sizeof hdr[k]);
            }
            for (k = 0; k < 4; k++) waveOutWrite(h, &hdr[k], sizeof hdr[k]);
            Sleep(60);
            mt.wType = TIME_SAMPLES;
            s0 = waveOutGetPosition(h, &mt, sizeof mt) == MMSYSERR_NOERROR && mt.wType == TIME_SAMPLES ? mt.u.sample : 0xFFFFFFFF;
            P("waveOut 48000 Hz stereo 16-bit: opens ok, plays 4 x 512-sample buffers, clock after 60 ms = %u samples%s\r\n", s0, s0 == 0xFFFFFFFF ? " (position query failed)" : "");
            waveOutReset(h);
            for (k = 0; k < 4; k++) waveOutUnprepareHeader(h, &hdr[k], sizeof hdr[k]);
            waveOutClose(h);
        } else P("waveOut 48000 Hz stereo 16-bit: could not open (the game's audio would fail)\r\n");
    }
}

/* ---- controllers ---- */
static void probe_input(void)
{
    static const char *const xi[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    UINT n;
    int i;
    section("Controllers");
    list_dlls("XInput:", xi, 3);
    for (i = 0; i < 3; i++) {
        HMODULE m = LoadLibraryA(xi[i]);
        typedef DWORD (WINAPI *XGet_t)(DWORD, void *);
        XGet_t f = m ? (XGet_t)GetProcAddress(m, "XInputGetState") : NULL;
        if (f) {
            static BYTE st[64];
            DWORD u;
            for (u = 0; u < 4; u++) P("  %s pad %u: %s\r\n", xi[i], u, f(u, st) == ERROR_SUCCESS ? "connected" : "not connected");
        }
    }
    n = joyGetNumDevs();
    P("joystick API (winmm) devices supported: %u\r\n", n);
    for (i = 0; i < (int)n && i < 16; i++) {
        JOYINFOEX ji;
        JOYCAPSA jc;
        ji.dwSize = sizeof ji; ji.dwFlags = JOY_RETURNALL;
        if (joyGetPosEx((UINT)i, &ji) == JOYERR_NOERROR && joyGetDevCapsA((UINT)i, &jc, sizeof jc) == JOYERR_NOERROR)
            P("  joystick %u: %s, %u axes, %u buttons\r\n", i, jc.szPname, jc.wNumAxes, jc.wNumButtons);
    }
    P("mouse buttons %d, keyboard type %d, raw input API: %s\r\n", GetSystemMetrics(SM_CMOUSEBUTTONS), GetKeyboardType(0),
      kproc("user32.dll", "GetRawInputData") ? "present" : "absent");
}

/* ---- video (the intro movies) ---- */
static void probe_video(void)
{
    static const char *const v[] = { "mfplat.dll", "mfreadwrite.dll", "quartz.dll", "wmvcore.dll", "wmvdecod.dll", "wmadmod.dll", "wmvdmod.dll", "msmpeg2vdec.dll", "dxva2.dll", "msdmo.dll", "qasf.dll", "wmp.dll" };
    static const GUID clsid_wmv = { 0x82d353df, 0x90bd, 0x4382, { 0x8b, 0xc2, 0x3f, 0x61, 0x92, 0xb7, 0x6e, 0x34 } };
    static const GUID clsid_wma = { 0x2eeb4adf, 0x4578, 0x4d10, { 0xbc, 0xa7, 0xbb, 0x95, 0x5f, 0x56, 0x32, 0x0a } };
    static const GUID clsid_fg = { 0xe436ebb3, 0x524f, 0x11ce, { 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa0, 0xed } };
    static const GUID iid_unk = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
    void *o = NULL;
    HRESULT hr;
    section("Video and codecs (the intro movies)");
    list_dlls("libraries:", v, (int)(sizeof v / sizeof v[0]));
    {
        HMODULE m = LoadLibraryA("mfreadwrite.dll");
        P("  MFCreateSourceReaderFromURL (what the current movie player calls): %s\r\n", m && GetProcAddress(m, "MFCreateSourceReaderFromURL") ? "yes" : "NO");
    }
    CoInitialize(NULL);
    hr = CoCreateInstance(&clsid_wmv, NULL, CLSCTX_INPROC_SERVER, &iid_unk, &o);
    P("  WMV video decoder object: %s (0x%08X)\r\n", SUCCEEDED(hr) ? "can be created" : "not available", hr);
    if (o) { ((ULONG(WINAPI *)(void *))(*(void ***)o)[2])(o); o = NULL; }
    hr = CoCreateInstance(&clsid_wma, NULL, CLSCTX_INPROC_SERVER, &iid_unk, &o);
    P("  WMA audio decoder object: %s\r\n", SUCCEEDED(hr) ? "can be created" : "not available");
    if (o) { ((ULONG(WINAPI *)(void *))(*(void ***)o)[2])(o); o = NULL; }
    hr = CoCreateInstance(&clsid_fg, NULL, CLSCTX_INPROC_SERVER, &iid_unk, &o);
    P("  DirectShow filter graph: %s (0x%08X)\r\n", SUCCEEDED(hr) ? "can be created" : "not available", hr);
    if (o) { ((ULONG(WINAPI *)(void *))(*(void ***)o)[2])(o); o = NULL; }
    CoUninitialize();
}

/* ---- which Windows functions exist (what an XP build would have to replace) ---- */
static void probe_api(void)
{
    static const struct { const char *dll, *fn, *needs; } api[] = {
        { "kernel32.dll", "InitializeCriticalSectionAndSpinCount", "XP" },
        { "kernel32.dll", "GetTickCount64", "Vista" }, { "kernel32.dll", "InitializeConditionVariable", "Vista" },
        { "kernel32.dll", "SleepConditionVariableCS", "Vista" }, { "kernel32.dll", "AcquireSRWLockExclusive", "Vista" },
        { "kernel32.dll", "ReleaseSRWLockExclusive", "Vista" }, { "kernel32.dll", "AcquireSRWLockShared", "Vista" },
        { "kernel32.dll", "InitOnceExecuteOnce", "Vista" }, { "kernel32.dll", "CreateWaitableTimerExW", "Vista" },
        { "kernel32.dll", "GetFinalPathNameByHandleW", "Vista" }, { "kernel32.dll", "SetFileInformationByHandle", "Vista" },
        { "kernel32.dll", "TrySubmitThreadpoolCallback", "Vista" }, { "kernel32.dll", "CreateTimerQueueTimer", "XP" },
        { "kernel32.dll", "GetFileSizeEx", "XP" }, { "kernel32.dll", "SetFilePointerEx", "XP" }, { "kernel32.dll", "GetSystemTimePreciseAsFileTime", "Windows 8" },
        { "kernel32.dll", "PrefetchVirtualMemory", "Windows 8" }, { "kernel32.dll", "SetThreadDescription", "Windows 10" },
        { "kernel32.dll", "AddVectoredExceptionHandler", "XP" }, { "kernel32.dll", "RtlLookupFunctionEntry", "64-bit only" },
        { "kernel32.dll", "GetActiveProcessorCount", "Windows 7" }, { "kernel32.dll", "SetThreadErrorMode", "Windows 7" },
        { "kernel32.dll", "QueueUserWorkItem", "XP" }, { "kernel32.dll", "FlsAlloc", "Server 2003 / XP x64 / Vista - NOT on 32-bit XP" }, { "kernel32.dll", "GetNativeSystemInfo", "XP" },
        { "kernel32.dll", "GetLogicalProcessorInformation", "XP SP3" }, { "kernel32.dll", "SwitchToThread", "XP" },
        { "user32.dll", "GetDpiForWindow", "Windows 10" }, { "user32.dll", "SetProcessDPIAware", "Vista" },
        { "user32.dll", "UpdateLayeredWindow", "XP" }, { "user32.dll", "MonitorFromWindow", "XP" }, { "user32.dll", "GetRawInputData", "XP" },
        { "shell32.dll", "SHGetFolderPathW", "XP" }, { "shell32.dll", "SHCreateDirectoryExW", "XP" },
        { "ole32.dll", "CoInitializeEx", "XP" }, { "advapi32.dll", "RegGetValueA", "Vista" },
        { "ntdll.dll", "RtlGetVersion", "XP" }, { "ntdll.dll", "NtDelayExecution", "XP" },
        { "winmm.dll", "timeBeginPeriod", "XP" }, { "dbghelp.dll", "SymFromAddr", "XP (dbghelp.dll)" },
        { "ws2_32.dll", "WSAStartup", "XP" }, { "bcrypt.dll", "BCryptGenRandom", "Vista" }, { "crypt32.dll", "CertOpenStore", "XP" },
    };
    int i;
    section("Windows API availability (a missing one has to be replaced by a shim before an XP build can start)");
    for (i = 0; i < (int)(sizeof api / sizeof api[0]); i++) {
        HMODULE m = LoadLibraryA(api[i].dll);
        FARPROC p = m ? GetProcAddress(m, api[i].fn) : NULL;
        P("  %-14s %-38s %-8s (introduced: %s)\r\n", api[i].dll, api[i].fn, p ? "present" : "MISSING", api[i].needs);
    }
}

/* ---- files, folders, timers, GDI overlay ---- */
static void probe_system(void)
{
    char s[MAX_PATH], fsn[32];
    DWORD d;
    ULARGE_INTEGER freeb, tot, totfree;
    LARGE_INTEGER f, a, b;
    DWORD t0, t1;
    int i;
    section("Folders, disk, timers, window effects");
    if (GetEnvironmentVariableA("LOCALAPPDATA", s, sizeof s)) P("LOCALAPPDATA environment variable: %s\r\n", s);
    else P("LOCALAPPDATA environment variable: NOT SET (XP does not define it; the game would have to ask the shell)\r\n");
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, s))) P("local application data folder: %s\r\n", s);
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, s))) P("application data folder: %s\r\n", s);
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROGRAM_FILES, NULL, 0, s))) P("program files folder: %s\r\n", s);
    GetTempPathA(sizeof s, s); P("temp folder: %s\r\n", s);
    GetModuleFileNameA(NULL, s, sizeof s);
    for (i = lstrlenA(s) - 1; i > 0 && s[i] != '\\'; i--) ;
    s[i + 1] = 0;
    P("this program runs from: %s\r\n", s);
    if (GetDiskFreeSpaceExA(s, &freeb, &tot, &totfree)) P("free space on this drive: %u MB of %u MB\r\n", (unsigned)(freeb.QuadPart >> 20), (unsigned)(tot.QuadPart >> 20));
    if (GetVolumeInformationA(NULL, NULL, 0, NULL, &d, NULL, fsn, sizeof fsn)) P("file system of the current drive: %s%s\r\n", fsn, lstrcmpiA(fsn, "NTFS") ? "  (FAT32 cannot hold files over 4 GB)" : "");
    {   /* can this folder be written, and large files created? */
        char tmp[MAX_PATH + 16];
        HANDLE h;
        lstrcpyA(tmp, s); lstrcatA(tmp, "dx9debug_write_test.tmp");
        h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        P("this folder is writable: %s\r\n", h != INVALID_HANDLE_VALUE ? "yes" : "NO");
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    QueryPerformanceFrequency(&f);
    P("performance counter frequency: %u Hz%s\r\n", (unsigned)f.QuadPart, f.QuadPart < 1000000 ? " (low: may be the old TSC/ACPI timer)" : "");
    {
        TIMECAPS tc;
        if (timeGetDevCaps(&tc, sizeof tc) == TIMERR_NOERROR) P("multimedia timer resolution: %u..%u ms\r\n", tc.wPeriodMin, tc.wPeriodMax);
    }
    timeBeginPeriod(1);
    QueryPerformanceCounter(&a);
    t0 = GetTickCount();
    for (i = 0; i < 20; i++) Sleep(1);
    QueryPerformanceCounter(&b);
    t1 = GetTickCount();
    timeEndPeriod(1);
    {
        char b1[32];
        P("Sleep(1) x20 with timeBeginPeriod(1): %s ms each (GetTickCount says %u ms total)\r\n", fx(qpc_ms(a, b, f) / 20.0, b1), t1 - t0);
    }
    {   /* the Esc/F1 overlay is a layered, per-pixel-alpha window */
        HWND w = CreateWindowExA(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT, "dx9debug", "", WS_POPUP, 0, 0, 64, 64, g_wnd, NULL, GetModuleHandleA(NULL), NULL);
        P("layered window with per-pixel alpha (the in-game menus): %s\r\n", w ? (kproc("user32.dll", "UpdateLayeredWindow") ? "can be created" : "created but UpdateLayeredWindow missing") : "CANNOT be created");
        if (w) DestroyWindow(w);
    }
}

/* ---- main ---- */
static int run_main(void)
{
    char cn[64] = "PC", path[MAX_PATH + 64], exe[MAX_PATH];
    DWORD n = sizeof cn;
    int i;
    SYSTEMTIME st;
    HANDLE in;
    char c[8];
    DWORD rd;

    /* A DLL that needs a newer Windows (d3dcompiler_47.dll on XP wants FlsAlloc) must fail quietly: without this XP shows an
     * "Entry Point Not Found" box for every LoadLibrary that cannot resolve an import. The report says "not found". */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    g_con = GetStdHandle(STD_OUTPUT_HANDLE);
    GetComputerNameA(cn, &n);
    GetModuleFileNameA(NULL, exe, sizeof exe);
    for (i = lstrlenA(exe) - 1; i > 0 && exe[i] != '\\'; i--) ;
    exe[i + 1] = 0;
    wsprintfA(path, "%sdx9debug_%s.txt", exe, cn);
    g_out = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    GetLocalTime(&st);
    P("dx9debug for NHL Hitz Pro (32-bit, no C runtime; replaces dx9probe)  %04u-%02u-%02u %02u:%02u\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    probe_windows();
    probe_cpu();
    probe_display();
    probe_d3d9();
    probe_compilers();
    shader_tests();
    speed_tests();
    probe_audio();
    probe_input();
    probe_video();
    probe_api();
    probe_system();
    if (g_dev) IDirect3DDevice9_Release(g_dev);
    if (g_d3d) IDirect3D9_Release(g_d3d);
    if (g_wnd) DestroyWindow(g_wnd);
    P("\r\nDone. Send me this file: %s\r\n", path);
    if (g_out != INVALID_HANDLE_VALUE) CloseHandle(g_out);
    P("Press Enter to close.\r\n");
    in = GetStdHandle(STD_INPUT_HANDLE);
    ReadFile(in, c, 1, &rd, NULL);
    return 0;
}

void __cdecl dx9debug_start(void)
{
    ExitProcess((UINT)run_main());
}
