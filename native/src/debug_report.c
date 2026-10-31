/* Self-monitoring for the debug build (cmake -DNHL_DEBUGLOG=ON, on top of NHL_RELEASE).
 *
 * Runs inside the game: nothing outside the installed files is needed on the player's PC (dbghelp.dll and
 * xinput come with Windows). Everything lands in %LOCALAPPDATA%\NHLHitzPro\debug:
 *
 *   session_<date>_<time>.log   everything the game prints (stdout and stderr), rotated at 96 MB per half so
 *                               the last ~100-190 MB of a long session is kept; the 6 newest sessions are kept
 *   crash_<date>_<time>.dmp     minidump (stacks, modules, data + the guest's RAM and GPU window) written when the process dies on an unhandled exception,
 *                               abort(), a failed assert or an invalid CRT parameter (and a message box says so)
 *   hang_<date>_<time>.dmp      the same, once, if no frame has been presented for 15 seconds
 *   running.lock                present while the game runs; found at the next start it means the last session
 *                               ended without a clean exit (killed, power loss, crash that bypassed the handlers)
 *
 * The log carries, besides the game's own output: a header (build, OS, CPU, memory, display devices,
 * settings), a heartbeat every 5 s (frames presented, memory in use, handles), every controller button and stick
 * change with a timestamp (so the steps that led to a crash can be replayed), first-chance exceptions that are not
 * the emulated hardware's, and on exit the call stack that asked for it. */
#ifdef NHL_DEBUGLOG

#include <windows.h>
#include <dbghelp.h>
#include <io.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <psapi.h>

#if defined(__has_include)
#if __has_include("nhl_debug_tag.h")
#include "nhl_debug_tag.h"
#endif
#endif
#ifndef NHL_DEBUG_TAG
#define NHL_DEBUG_TAG "debug"
#endif

#define LOG_HALF_BYTES   (96u * 1024u * 1024u)
#define KEEP_SESSIONS    6

extern uint64_t nhl_flip_count(void);                    /* backend_d3d11.c */
extern void nhl_crash_context(PEXCEPTION_POINTERS ep);   /* main.c: guest registers and call chain */
extern DWORD WINAPI XInputGetState(DWORD user, void *state);
extern ptrdiff_t g_xbox_mem_offset;                       /* host address = guest address + this */

static char s_dir[MAX_PATH];
static char s_stamp[32];
static char s_log_name[MAX_PATH];
static ULONGLONG s_t0;
static volatile LONG s_crashing;
static volatile LONG s_clean_exit;
static int s_log_half;              /* 0: session_x.log is current, 1: session_x.log.1 */

static double now_s(void) { return (double)(GetTickCount64() - s_t0) / 1000.0; }

/* ---- log file: stderr and stdout both append to the current half; at 96 MB the other half is started ----- */

static void log_path(int half, char *out, size_t n)
{
    snprintf(out, n, half ? "%s.1" : "%s", s_log_name);
}

static void log_open(int half, const char *mode)
{
    char p[MAX_PATH];
    log_path(half, p, sizeof p);
    freopen(p, mode, stderr);
    freopen(p, "a", stdout);
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
}

static void log_rotate_if_needed(void)
{
    LARGE_INTEGER sz;
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(stderr));
    if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &sz) || sz.QuadPart < (LONGLONG)LOG_HALF_BYTES) return;
    s_log_half ^= 1;
    log_open(s_log_half, "w");
    fprintf(stderr, "[LOG] t=%.0f continued in %s (the other half holds what came before)\n", now_s(), s_log_half ? "session log .1" : "session log");
}

static void log_flush(void)
{
    HANDLE h;
    fflush(stderr);
    fflush(stdout);
    h = (HANDLE)_get_osfhandle(_fileno(stderr));
    if (h != INVALID_HANDLE_VALUE) FlushFileBuffers(h);
}

/* ---- housekeeping --------------------------------------------------------------------------------------- */

static int cmp_name_desc(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }

static void prune_old_sessions(void)
{
    char pat[MAX_PATH], names[64][MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, i;
    snprintf(pat, sizeof pat, "%s\\session_*.log", s_dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (n < 64) { strncpy(names[n], fd.cFileName, MAX_PATH - 1); names[n][MAX_PATH - 1] = 0; n++; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, (size_t)n, MAX_PATH, cmp_name_desc);
    for (i = KEEP_SESSIONS - 1; i < n; i++) {           /* this session makes the sixth */
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\%s", s_dir, names[i]);
        DeleteFileA(p);
        strncat(p, ".1", sizeof p - strlen(p) - 1);
        DeleteFileA(p);
    }
}

static void dump_name(const char *kind, char *out, size_t n)
{
    snprintf(out, n, "%s\\%s_%s.dmp", s_dir, kind, s_stamp);
}

/* ---- dumps ---------------------------------------------------------------------------------------------- */

struct dump_job { EXCEPTION_POINTERS *ep; DWORD tid; char path[MAX_PATH]; BOOL ok; };

/* A full-memory dump of this process is 2.7 GB (the guest address space is mapped in several places). The dump is
 * the normal small kind plus exactly the guest memory that matters: the main 64 MB of RAM and the 64 MB contiguous
 * window (GPU buffers, textures, push buffer), found at host = guest address + g_xbox_mem_offset. */
static BOOL CALLBACK dump_callback(PVOID ctx, PMINIDUMP_CALLBACK_INPUT in, PMINIDUMP_CALLBACK_OUTPUT out)
{
    static const ULONG64 guest_ranges[2][2] = { { 0x00000000u, 0x04000000u }, { 0x80000000u, 0x04000000u } };
    static int next;
    (void)ctx;
    if (in->CallbackType == MemoryCallback) {
        while (next < 2) {
            ULONG64 base = (ULONG64)((uintptr_t)g_xbox_mem_offset + guest_ranges[next][0]);
            ULONG64 size = guest_ranges[next][1];
            MEMORY_BASIC_INFORMATION mbi;
            next++;
            if (VirtualQuery((LPCVOID)(uintptr_t)base, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                    (ULONG64)mbi.RegionSize >= 0x10000) {
                out->MemoryBase = base;
                out->MemorySize = (ULONG)(mbi.RegionSize < size ? mbi.RegionSize : size);
                return TRUE;
            }
        }
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI dump_thread(LPVOID p)
{
    struct dump_job *j = (struct dump_job *)p;
    HANDLE f = CreateFileA(j->path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    MINIDUMP_EXCEPTION_INFORMATION mei;
    MINIDUMP_CALLBACK_INFORMATION cbi;
    j->ok = FALSE;
    cbi.CallbackRoutine = dump_callback;
    cbi.CallbackParam = NULL;
    if (f == INVALID_HANDLE_VALUE) return 0;
    mei.ThreadId = j->tid;
    mei.ExceptionPointers = j->ep;
    mei.ClientPointers = FALSE;
    j->ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                              (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                              MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo |
                                              MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData),
                              j->ep ? &mei : NULL, NULL, &cbi);
    CloseHandle(f);
    return 0;
}

/* From a helper thread: the faulting thread may be out of stack or holding a lock the dump needs. */
static BOOL write_dump(const char *kind, EXCEPTION_POINTERS *ep, char *path_out, size_t n)
{
    struct dump_job j;
    HANDLE t;
    j.ep = ep;
    j.tid = GetCurrentThreadId();
    dump_name(kind, j.path, sizeof j.path);
    t = CreateThread(NULL, 0, dump_thread, &j, 0, NULL);
    if (t) { WaitForSingleObject(t, 120000); CloseHandle(t); }
    if (path_out) snprintf(path_out, n, "%s", j.path);
    return j.ok;
}

static void symbolize(void *addr, char *out, size_t n)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    memset(buf, 0, sizeof buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)addr, &disp, sym))
        snprintf(out, n, "%p %s+0x%llX", addr, sym->Name, (unsigned long long)disp);
    else
        snprintf(out, n, "%p", addr);
}

static void log_native_stack(const char *why)
{
    void *frames[48];
    USHORT i, k = RtlCaptureStackBackTrace(1, 48, frames, NULL);
    fprintf(stderr, "[STACK] %s, calling thread %lu:\n", why, GetCurrentThreadId());
    for (i = 0; i < k; i++) {
        char line[400];
        symbolize(frames[i], line, sizeof line);
        fprintf(stderr, "    %s\n", line);
    }
}

static void crash_report(const char *what, EXCEPTION_POINTERS *ep)
{
    char path[MAX_PATH], msg[MAX_PATH + 300];
    BOOL ok;
    if (InterlockedExchange(&s_crashing, 1)) { Sleep(INFINITE); return; }   /* a second faulting thread waits */
    fprintf(stderr, "\n[FATAL] t=%.3f %s", now_s(), what);
    if (ep && ep->ExceptionRecord)
        fprintf(stderr, " code=0x%08lX at %p", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    fprintf(stderr, " thread %lu, frames presented %llu\n", GetCurrentThreadId(), (unsigned long long)nhl_flip_count());
    if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2)
        fprintf(stderr, "  %s of address 0x%llX\n", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    nhl_crash_context(ep);
    log_native_stack("fatal");
    ok = write_dump("crash", ep, path, sizeof path);
    fprintf(stderr, "[FATAL] dump %s: %s\n", ok ? "written" : "FAILED", path);
    s_clean_exit = 1;                                   /* the lock file is deleted: this end is accounted for */
    log_flush();
    {
        char lock[MAX_PATH];
        snprintf(lock, sizeof lock, "%s\\running.lock", s_dir);
        DeleteFileA(lock);
    }
    snprintf(msg, sizeof msg,
             "NHL Hitz Pro has crashed.\n\nDebug files were saved in:\n%s\n\nPlease send the whole folder "
             "(or use the \"Collect debug files\" shortcut in the Start menu) so the problem can be fixed.", s_dir);
    MessageBoxA(NULL, msg, "NHL Hitz Pro (debug build)", MB_OK | MB_ICONERROR | MB_TOPMOST);
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep)
{
    crash_report("unhandled exception", ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void synthetic_crash(const char *what)
{
    static CONTEXT ctx;
    static EXCEPTION_RECORD rec;
    static EXCEPTION_POINTERS ep;
    RtlCaptureContext(&ctx);
    memset(&rec, 0, sizeof rec);
    rec.ExceptionCode = 0xE0000001u;
    rec.ExceptionAddress = (PVOID)ctx.Rip;
    ep.ContextRecord = &ctx;
    ep.ExceptionRecord = &rec;
    crash_report(what, &ep);
    TerminateProcess(GetCurrentProcess(), 0xDEAD0001u);
}

static void abort_handler(int sig) { (void)sig; synthetic_crash("abort() or failed assert"); }

static void invalid_param(const wchar_t *e, const wchar_t *f, const wchar_t *file, unsigned line, uintptr_t r)
{
    (void)e; (void)f; (void)file; (void)line; (void)r;
    synthetic_crash("invalid CRT parameter");
}

static void purecall(void) { synthetic_crash("pure virtual call"); }

/* First-chance exceptions: the emulated hardware's access violations are expected and handled elsewhere; anything
 * else is logged once per (code, address) so a fault that something later swallows still leaves a trace. */
static LONG CALLBACK first_chance(EXCEPTION_POINTERS *ep)
{
    static DWORD seen_code[64];
    static void *seen_addr[64];
    static LONG nseen;
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    void *a = ep->ExceptionRecord->ExceptionAddress;
    LONG i, n;
    if (c == EXCEPTION_ACCESS_VIOLATION || c == 0x40010006 || c == 0x406D1388 || c == 0xE06D7363 || c == 0x4001000A ||
            c == EXCEPTION_BREAKPOINT || c == EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    n = nseen;
    for (i = 0; i < n && i < 64; i++)
        if (seen_code[i] == c && seen_addr[i] == a) return EXCEPTION_CONTINUE_SEARCH;
    if (n < 64) {
        char line[400];
        i = InterlockedIncrement(&nseen) - 1;
        if (i < 64) { seen_code[i] = c; seen_addr[i] = a; }
        symbolize(a, line, sizeof line);
        fprintf(stderr, "[EXC] t=%.3f first-chance 0x%08lX at %s (thread %lu)\n", now_s(), c, line, GetCurrentThreadId());
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void at_exit(void)
{
    if (s_crashing || s_clean_exit) return;
    s_clean_exit = 1;
    fprintf(stderr, "[EXIT] t=%.3f the process is exiting through exit()/return from main\n", now_s());
    log_native_stack("exit");
    log_flush();
    {
        char lock[MAX_PATH];
        snprintf(lock, sizeof lock, "%s\\running.lock", s_dir);
        DeleteFileA(lock);
    }
}

/* ---- monitor thread: heartbeat and hang detection ------------------------------------------------------- */

static DWORD WINAPI monitor_thread(LPVOID unused)
{
    uint64_t last_flips = 0;
    ULONGLONG last_move = GetTickCount64(), last_hb = 0;
    int hang_dumps = 0, hung = 0;
    (void)unused;
    for (;;) {
        ULONGLONG t = GetTickCount64();
        uint64_t f = nhl_flip_count();
        Sleep(1000);
        t = GetTickCount64();
        log_rotate_if_needed();
        {   /* NHL_DEBUG_SELFTEST=crash: fault on purpose 20 s in, to check the dump path (developer use) */
            static int armed = -1;
            if (armed < 0) { const char *e = getenv("NHL_DEBUG_SELFTEST"); armed = e && !strcmp(e, "crash"); }
            if (armed && now_s() > 20.0) *(volatile int *)0 = 1;
        }
        if (f != last_flips) {
            if (hung) fprintf(stderr, "[HANG] t=%.3f frames are being presented again\n", now_s());
            hung = 0;
            last_flips = f;
            last_move = t;
        } else if (!hung && f > 0 && t - last_move > 15000) {
            hung = 1;
            fprintf(stderr, "[HANG] t=%.3f no frame presented for %llu s (frames so far %llu)\n", now_s(),
                    (unsigned long long)((t - last_move) / 1000), (unsigned long long)f);
            if (hang_dumps < 2) {
                char path[MAX_PATH];
                BOOL ok = write_dump("hang", NULL, path, sizeof path);
                hang_dumps++;
                fprintf(stderr, "[HANG] dump %s: %s\n", ok ? "written" : "FAILED", path);
            }
        }
        if (t - last_hb >= 5000) {
            PROCESS_MEMORY_COUNTERS_EX pm;
            MEMORYSTATUSEX ms;
            DWORD handles = 0;
            last_hb = t;
            memset(&pm, 0, sizeof pm);
            pm.cb = sizeof pm;
            GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
            ms.dwLength = sizeof ms;
            GlobalMemoryStatusEx(&ms);
            GetProcessHandleCount(GetCurrentProcess(), &handles);
            fprintf(stderr, "[HB] t=%.0f frames=%llu ws=%lluMB private=%lluMB handles=%lu sysfree=%lluMB\n", now_s(),
                    (unsigned long long)f, (unsigned long long)(pm.WorkingSetSize >> 20),
                    (unsigned long long)(pm.PrivateUsage >> 20), handles, (unsigned long long)(ms.ullAvailPhys >> 20));
        }
    }
}

/* ---- controller log ------------------------------------------------------------------------------------- */

typedef struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XIN_STATE;

static const char *btn_names(WORD b, char *out, size_t n)
{
    static const struct { WORD m; const char *s; } t[] = {
        {0x0001, "Up"}, {0x0002, "Down"}, {0x0004, "Left"}, {0x0008, "Right"}, {0x0010, "Start"}, {0x0020, "Back"},
        {0x0040, "L3"}, {0x0080, "R3"}, {0x0100, "LB"}, {0x0200, "RB"}, {0x1000, "A"}, {0x2000, "B"}, {0x4000, "X"}, {0x8000, "Y"}};
    size_t i, l = 0;
    out[0] = 0;
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (b & t[i].m) l += (size_t)snprintf(out + l, n - l, "%s%s", l ? "+" : "", t[i].s);
    if (!l) snprintf(out, n, "-");
    return out;
}

static DWORD WINAPI pad_thread(LPVOID unused)
{
    XIN_STATE prev[4], cur;
    int alive[4] = {0, 0, 0, 0}, i;
    ULONGLONG last_scan = 0;
    (void)unused;
    memset(prev, 0, sizeof prev);
    for (;;) {
        ULONGLONG t = GetTickCount64();
        int scan = t - last_scan > 2000;
        if (scan) last_scan = t;
        for (i = 0; i < 4; i++) {
            if (!alive[i] && !scan) continue;
            if (XInputGetState((DWORD)i, &cur) != 0) {
                if (alive[i]) { alive[i] = 0; fprintf(stderr, "[PADLOG] t=%.3f pad=%d disconnected\n", now_s(), i); }
                continue;
            }
            if (!alive[i]) { alive[i] = 1; fprintf(stderr, "[PADLOG] t=%.3f pad=%d connected\n", now_s(), i); }
            if (cur.buttons != prev[i].buttons || abs(cur.lx - prev[i].lx) > 8000 || abs(cur.ly - prev[i].ly) > 8000 ||
                    abs(cur.rx - prev[i].rx) > 8000 || abs(cur.ry - prev[i].ry) > 8000 ||
                    abs((int)cur.lt - prev[i].lt) > 64 || abs((int)cur.rt - prev[i].rt) > 64) {
                char b[96];
                fprintf(stderr, "[PADLOG] t=%.3f pad=%d btn=%s lx=%d ly=%d rx=%d ry=%d lt=%u rt=%u frames=%llu\n", now_s(), i,
                        btn_names(cur.buttons, b, sizeof b), cur.lx, cur.ly, cur.rx, cur.ry, cur.lt, cur.rt,
                        (unsigned long long)nhl_flip_count());
                prev[i] = cur;
            }
        }
        Sleep(10);
    }
}

/* ---- header --------------------------------------------------------------------------------------------- */

static void print_header(void)
{
    char exe[MAX_PATH], cpu[128] = "?", line[512];
    SYSTEMTIME st;
    MEMORYSTATUSEX ms;
    DWORD sz = sizeof cpu, i;
    HKEY k;
    DISPLAY_DEVICEA dd;
    typedef LONG (WINAPI *rtlver_fn)(OSVERSIONINFOW *);
    rtlver_fn rv = (rtlver_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW os;
    FILETIME ft;
    HANDLE fh;

    GetLocalTime(&st);
    GetModuleFileNameA(NULL, exe, sizeof exe);
    fprintf(stderr, "==== NHL Hitz Pro debug log ====\n");
    fprintf(stderr, "build tag   : %s (compiled %s %s)\n", NHL_DEBUG_TAG, __DATE__, __TIME__);
    fprintf(stderr, "started     : %04d-%02d-%02d %02d:%02d:%02d\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fprintf(stderr, "executable  : %s\n", exe);
    fh = CreateFileA(exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (fh != INVALID_HANDLE_VALUE) {
        if (GetFileTime(fh, NULL, NULL, &ft)) {
            FileTimeToSystemTime(&ft, &st);
            fprintf(stderr, "exe modified : %04d-%02d-%02d %02d:%02d:%02d, %lu bytes\n", st.wYear, st.wMonth, st.wDay, st.wHour,
                    st.wMinute, st.wSecond, GetFileSize(fh, NULL));
        }
        CloseHandle(fh);
    }
    memset(&os, 0, sizeof os);
    os.dwOSVersionInfoSize = sizeof os;
    if (rv && rv(&os) == 0) fprintf(stderr, "windows     : %lu.%lu build %lu\n", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegQueryValueExA(k, "ProcessorNameString", NULL, NULL, (LPBYTE)cpu, &sz);
        RegCloseKey(k);
    }
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    fprintf(stderr, "cpu         : %s (%lu logical)\n", cpu, (unsigned long)(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    fprintf(stderr, "memory      : %llu MB total, %llu MB free\n", (unsigned long long)(ms.ullTotalPhys >> 20), (unsigned long long)(ms.ullAvailPhys >> 20));
    memset(&dd, 0, sizeof dd);
    dd.cb = sizeof dd;
    for (i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++) {
        fprintf(stderr, "display %lu   : %s (%s) flags=0x%lX\n", i, dd.DeviceString, dd.DeviceName, dd.StateFlags);
        dd.cb = sizeof dd;
    }
    fprintf(stderr, "log folder  : %s\n", s_dir);
    {
        char base[MAX_PATH], p[MAX_PATH], text[2048];
        DWORD got = 0;
        HANDLE f;
        GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
        snprintf(p, sizeof p, "%s\\NHLHitzPro\\settings.ini", base);
        f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            if (ReadFile(f, text, sizeof text - 1, &got, NULL)) { text[got] = 0; fprintf(stderr, "settings.ini:\n%s\n", text); }
            CloseHandle(f);
        }
    }
    (void)line;
    fprintf(stderr, "================================\n");
}

/* ---- entry point ---------------------------------------------------------------------------------------- */

void nhl_debug_init(void)
{
    char base[MAX_PATH], lock[MAX_PATH];
    SYSTEMTIME st;
    DWORD attr;

    s_t0 = GetTickCount64();
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base) || strlen(base) > MAX_PATH - 80) strcpy(base, ".");
    snprintf(s_dir, sizeof s_dir, "%s\\NHLHitzPro", base);
    CreateDirectoryA(s_dir, NULL);
    strcat(s_dir, "\\debug");
    CreateDirectoryA(s_dir, NULL);
    GetLocalTime(&st);
    snprintf(s_stamp, sizeof s_stamp, "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    snprintf(s_log_name, sizeof s_log_name, "%s\\session_%s.log", s_dir, s_stamp);
    snprintf(lock, sizeof lock, "%s\\running.lock", s_dir);
    prune_old_sessions();

    log_open(0, "w");

    attr = GetFileAttributesA(lock);
    print_header();
    if (attr != INVALID_FILE_ATTRIBUTES)
        fprintf(stderr, "[PREV] the previous session did not end cleanly (running.lock was still there): it was killed, lost power, "
                        "or crashed past the handlers; see the previous session log\n");
    {
        HANDLE l = CreateFileA(lock, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (l != INVALID_HANDLE_VALUE) {
            DWORD wr;
            WriteFile(l, s_stamp, (DWORD)strlen(s_stamp), &wr, NULL);
            CloseHandle(l);
        }
    }

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(unhandled_filter);
    AddVectoredExceptionHandler(0, first_chance);
    signal(SIGABRT, abort_handler);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(invalid_param);
    _set_purecall_handler(purecall);
    atexit(at_exit);

    CreateThread(NULL, 0, monitor_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, pad_thread, NULL, 0, NULL);
}

/* Called by code that ends the process on purpose (window closed, Quit in the menu) so the log says so. */
void nhl_debug_note_exit(const char *why)
{
    if (s_crashing || s_clean_exit) return;
    s_clean_exit = 1;
    fprintf(stderr, "[EXIT] t=%.3f %s\n", now_s(), why);
    log_flush();
    {
        char lock[MAX_PATH];
        snprintf(lock, sizeof lock, "%s\\running.lock", s_dir);
        DeleteFileA(lock);
    }
}

#endif /* NHL_DEBUGLOG */
