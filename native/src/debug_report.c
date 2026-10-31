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
 * DEL in the game window marks "a graphical problem is here": the log gets a [MARK] line, three screenshots of the game
 * image (now, +1.5 s, +4 s) and, for the first two marks, a dump with the guest's RAM and GPU memory are saved. The game keeps
 * running. If the session then ends without a crash, the whole debug folder is zipped to the Desktop at exit; a session with
 * no mark and no crash ends silently.
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
#include <shlobj.h>

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
static void show_banner(const char *title, const char *l2, const char *l3, DWORD ms, COLORREF accent);
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

struct dump_job { EXCEPTION_POINTERS *ep; DWORD tid; char path[MAX_PATH]; BOOL ok; int next; };

/* A full-memory dump of this process is 2.7 GB (the guest address space is mapped in several places). The dump is
 * the normal small kind plus exactly the guest memory that matters: the main 64 MB of RAM and the 64 MB contiguous
 * window (GPU buffers, textures, push buffer), found at host = guest address + g_xbox_mem_offset. */
static BOOL CALLBACK dump_callback(PVOID ctx, PMINIDUMP_CALLBACK_INPUT in, PMINIDUMP_CALLBACK_OUTPUT out)
{
    static const ULONG64 guest_ranges[2][2] = { { 0x00000000u, 0x04000000u }, { 0x80000000u, 0x04000000u } };
    int *next_p = (int *)ctx;                     /* per dump: which of the two guest ranges comes next */
    if (in->CallbackType == MemoryCallback) {
        while (*next_p < 2) {
            ULONG64 base = (ULONG64)((uintptr_t)g_xbox_mem_offset + guest_ranges[*next_p][0]);
            ULONG64 size = guest_ranges[*next_p][1];
            MEMORY_BASIC_INFORMATION mbi;
            (*next_p)++;
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
    cbi.CallbackParam = &j->next;
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
    j.next = 0;
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

/* ---- after the dump: a zip on the Desktop, and a message that is really seen ------------------------------- */

/* tar.exe ships with Windows 10 (1803 and later) and writes a normal zip; it is started and left running, so it
 * finishes even after the player clicks OK and the game exits. Without it, the collect script is the fallback. */
static HANDLE start_zip(const char *tag, char *zip_out, size_t n)
{
    wchar_t desk[MAX_PATH], sys[MAX_PATH], dir_w[MAX_PATH], zip_w[MAX_PATH + 64], cmd[3 * MAX_PATH + 256];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t stamp_w[32], tag_w[32];
    HANDLE proc = NULL;
    zip_out[0] = 0;
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, desk))) return NULL;
    GetSystemDirectoryW(sys, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, s_dir, -1, dir_w, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, s_stamp, -1, stamp_w, 32);
    MultiByteToWideChar(CP_ACP, 0, tag, -1, tag_w, 32);
    _snwprintf(zip_w, sizeof zip_w / sizeof zip_w[0], L"%ls\\NHLHitzPro-debug-%ls-%ls.zip", desk, tag_w, stamp_w);
    WideCharToMultiByte(CP_ACP, 0, zip_w, -1, zip_out, (int)n, "?", NULL);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    {
        wchar_t tar[MAX_PATH];
        _snwprintf(tar, MAX_PATH, L"%ls\\tar.exe", sys);
        if (GetFileAttributesW(tar) != INVALID_FILE_ATTRIBUTES) {
            _snwprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\" -a -c -f \"%ls\" -C \"%ls\" .", tar, zip_w, dir_w);
        } else {
            wchar_t exe[MAX_PATH], *sl;
            GetModuleFileNameW(NULL, exe, MAX_PATH);
            sl = wcsrchr(exe, L'\\');
            if (sl) *sl = 0;
            SetEnvironmentVariableW(L"NHL_COLLECT_QUIET", L"1");
            _snwprintf(cmd, sizeof cmd / sizeof cmd[0],
                       L"\"%ls\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -ExecutionPolicy Bypass -File \"%ls\\collect_debug.ps1\"",
                       sys, exe);
        }
    }
    /* The log is complete: let go of it, so the zip does not race a file that is still being written to (an archive
     * started on a growing log came out without its directory). Anything printed later goes nowhere. */
    fprintf(stderr, "[%s] starting the zip of the debug files: %s\n", tag, zip_out);
    log_flush();
    freopen("NUL", "w", stderr);
    freopen("NUL", "w", stdout);
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        proc = pi.hProcess;
    } else {
        zip_out[0] = 0;
    }
    return proc;
}

/* The game's fullscreen window can sit over an ordinary message box (and the crashed window cannot be asked to step
 * aside), so the box is shown from its own thread, system-modal and foreground, after the game window has been asked
 * to minimise without waiting for it. */
static DWORD WINAPI crash_box_thread(LPVOID text)
{
    HWND game = FindWindowA("NhlD3D11Backend", NULL);
    if (game) {
        SetWindowPos(game, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        ShowWindowAsync(game, SW_MINIMIZE);
    }
    AllowSetForegroundWindow(ASFW_ANY);
    MessageBoxA(NULL, (const char *)text, "NHL Hitz Pro (debug build)",
                MB_OK | MB_ICONERROR | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_TOPMOST);
    return 0;
}

/* The same kind of box for the end of a session that had marked problems (no crash: an information box). */
static DWORD WINAPI note_box_thread(LPVOID text)
{
    HWND game = FindWindowA("NhlD3D11Backend", NULL);
    if (game) {
        SetWindowPos(game, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        ShowWindowAsync(game, SW_MINIMIZE);
    }
    AllowSetForegroundWindow(ASFW_ANY);
    MessageBoxA(NULL, (const char *)text, "NHL Hitz Pro (debug build)",
                MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_TOPMOST);
    return 0;
}

static volatile LONG s_marks;                 /* DEL presses this session */

/* A session that ended on purpose with marked problems: everything goes to one zip on the Desktop. The log is
 * complete first (the zip must not race a growing file) and the zip is waited for, so the message is true. */
static void zip_marks_on_exit(void)
{
    char zip[MAX_PATH * 2], msg[MAX_PATH * 2 + 300];
    HANDLE zp, bt;
    LONG n = s_marks;
    fprintf(stderr, "[EXIT] t=%.3f %ld issue mark(s) this session: zipping the debug files\n", now_s(), n);
    log_flush();
    {
        char lock[MAX_PATH];
        snprintf(lock, sizeof lock, "%s\\running.lock", s_dir);
        DeleteFileA(lock);
    }
    show_banner("SAVING DEBUG FILES", "Please wait a moment", "Zipping the log, screenshots and dumps to your Desktop.", 60000, RGB(255, 122, 0));
    zp = start_zip("marks", zip, sizeof zip);
    if (zp) { WaitForSingleObject(zp, 180000); CloseHandle(zp); }
    if (zip[0])
        snprintf(msg, sizeof msg, "You marked %ld graphical problem(s) this session.\n\nThe debug files were saved to your Desktop:\n%s\n\n"
                 "Please send that file so they can be fixed.\n\nThe files are also in:\n%s", (long)n, zip, s_dir);
    else
        snprintf(msg, sizeof msg, "You marked %ld graphical problem(s) this session.\n\nThe debug files are in:\n%s\n\nPlease zip that "
                 "whole folder and send it so they can be fixed.", (long)n, s_dir);
    bt = CreateThread(NULL, 0, note_box_thread, msg, 0, NULL);
    if (bt) { WaitForSingleObject(bt, INFINITE); CloseHandle(bt); }
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
    {
        char zip[MAX_PATH * 2];
        HANDLE bt;
        { HANDLE zp = start_zip("crash", zip, sizeof zip); if (zp) CloseHandle(zp); }
        if (zip[0])
            snprintf(msg, sizeof msg,
                     "NHL Hitz Pro has crashed.\n\nA zip of the debug files is being saved to your Desktop (it takes a few "
                     "seconds):\n%s\n\nPlease send that file so the problem can be fixed.\n\nThe files are also in:\n%s", zip, s_dir);
        else
            snprintf(msg, sizeof msg,
                     "NHL Hitz Pro has crashed.\n\nDebug files were saved in:\n%s\n\nPlease zip that whole folder and send it "
                     "so the problem can be fixed.", s_dir);
        bt = CreateThread(NULL, 0, crash_box_thread, msg, 0, NULL);
        if (bt) { WaitForSingleObject(bt, INFINITE); CloseHandle(bt); }
        else MessageBoxA(NULL, msg, "NHL Hitz Pro (debug build)", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL | MB_TOPMOST);
    }
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
    if (s_marks) zip_marks_on_exit();
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

/* ---- start-up banner: confirms this is the debug build ----------------------------------------------------- */

struct banner_args { char title[48], l2[120], l3[360]; DWORD ms; COLORREF accent; };

static LRESULT CALLBACK banner_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_TIMER) { DestroyWindow(h); return 0; }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcA(h, m, w, l);
}

/* Its own window on its own thread, so it is up at once (the game's window comes much later) and over the game
 * whatever mode that is in; it takes no input and goes away by itself after five seconds. */
static DWORD WINAPI banner_thread(LPVOID arg)
{
    struct banner_args *ba = (struct banner_args *)arg;
    WNDCLASSA wc;
    HWND hw;
    int dpi = 96, w, h, x, y, i, bw;
    BITMAPINFO bi;
    void *bits = NULL;
    HDC sdc, mdc;
    HBITMAP bm;
    HGDIOBJ oldbm, oldf;
    HFONT f1, f2, f3;
    uint32_t *px;
    RECT r;
    HBRUSH br;
    BLENDFUNCTION bf;
    POINT p0 = { 0, 0 }, pos;
    SIZE sz;
    MSG msg;

    {
        typedef UINT (WINAPI *dpi_fn)(void);
        HMODULE u = GetModuleHandleA("user32.dll");
        dpi_fn f = u ? (dpi_fn)(void *)GetProcAddress(u, "GetDpiForSystem") : NULL;
        if (f) dpi = (int)f();
    }
    w = 680 * dpi / 96; h = 215 * dpi / 96;
    x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    y = GetSystemMetrics(SM_CYSCREEN) / 8;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = banner_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "NhlDebugBanner";
    RegisterClassA(&wc);
    hw = CreateWindowExA(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT,
                         wc.lpszClassName, "", WS_POPUP, x, y, w, h, NULL, NULL, wc.hInstance, NULL);
    if (!hw) { free(ba); return 0; }

    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    sdc = GetDC(NULL);
    mdc = CreateCompatibleDC(sdc);
    bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits) { DestroyWindow(hw); DeleteDC(mdc); ReleaseDC(NULL, sdc); return 0; }
    oldbm = SelectObject(mdc, bm);
    SetRect(&r, 0, 0, w, h);
    br = CreateSolidBrush(ba->accent); FillRect(mdc, &r, br); DeleteObject(br);
    bw = 4 * dpi / 96;
    InflateRect(&r, -bw, -bw);
    br = CreateSolidBrush(RGB(14, 22, 40)); FillRect(mdc, &r, br); DeleteObject(br);
    SetBkMode(mdc, TRANSPARENT);
    f1 = CreateFontA(-(46 * dpi / 96), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    f2 = CreateFontA(-(26 * dpi / 96), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    f3 = CreateFontA(-(17 * dpi / 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    oldf = SelectObject(mdc, f1);
    SetTextColor(mdc, ba->accent);
    SetRect(&r, 0, 18 * dpi / 96, w, 76 * dpi / 96);
    DrawTextA(mdc, ba->title, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mdc, f2);
    SetTextColor(mdc, RGB(255, 255, 255));
    SetRect(&r, 0, 80 * dpi / 96, w, 116 * dpi / 96);
    DrawTextA(mdc, ba->l2, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mdc, f3);
    SetTextColor(mdc, RGB(200, 210, 225));
    SetRect(&r, 12 * dpi / 96, 122 * dpi / 96, w - 12 * dpi / 96, 205 * dpi / 96);
    DrawTextA(mdc, ba->l3, -1, &r, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
    SelectObject(mdc, oldf);
    DeleteObject(f1); DeleteObject(f2); DeleteObject(f3);
    GdiFlush();
    px = (uint32_t *)bits;
    for (i = 0; i < w * h; i++) px[i] |= 0xFF000000u;            /* fully opaque */

    pos.x = x; pos.y = y;
    sz.cx = w; sz.cy = h;
    bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(hw, sdc, &pos, &sz, mdc, &p0, 0, &bf, ULW_ALPHA);
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    SetWindowPos(hw, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(hw, 1, ba->ms, NULL);
    fprintf(stderr, "[DEBUG] banner \"%s\" shown for %lu ms\n", ba->title, (unsigned long)ba->ms);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    SelectObject(mdc, oldbm);
    DeleteObject(bm); DeleteDC(mdc); ReleaseDC(NULL, sdc);
    free(ba);
    return 0;
}

static void show_banner(const char *title, const char *l2, const char *l3, DWORD ms, COLORREF accent)
{
    struct banner_args *ba = (struct banner_args *)calloc(1, sizeof *ba);
    if (!ba) return;
    snprintf(ba->title, sizeof ba->title, "%s", title);
    snprintf(ba->l2, sizeof ba->l2, "%s", l2);
    snprintf(ba->l3, sizeof ba->l3, "%s", l3);
    ba->ms = ms;
    ba->accent = accent;
    CloseHandle(CreateThread(NULL, 0, banner_thread, ba, 0, NULL));
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

    {
        char l3[MAX_PATH + 160];
        snprintf(l3, sizeof l3, "Logging and crash capture are ON. Press DEL in the game when you see a graphical glitch.\nFiles: %s", s_dir);
        show_banner("DEBUG BUILD", "NHL Hitz Pro " NHL_DEBUG_TAG, l3, 5000, RGB(255, 122, 0));
    }
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
    if (s_marks) zip_marks_on_exit();
}

/* ---- DEL: mark a graphical problem ------------------------------------------------------------------------ */

#define MARK_DUMPS 2             /* dumps are ~200 MB: only the first marks get one */

static struct { ULONGLONG due; char path[MAX_PATH]; } s_caps[24];
static volatile LONG s_cap_lock;

static void cap_lock(void) { while (InterlockedCompareExchange(&s_cap_lock, 1, 0)) SwitchToThread(); }
static void cap_unlock(void) { InterlockedExchange(&s_cap_lock, 0); }

/* The renderer asks every frame: a screenshot path (without extension) that is due, if any. */
int nhl_debug_capture_due(char *path, size_t n)
{
    int i, got = 0;
    if (!s_marks) return 0;
    cap_lock();
    for (i = 0; i < 24 && !got; i++)
        if (s_caps[i].path[0] && GetTickCount64() >= s_caps[i].due) {
            snprintf(path, n, "%s", s_caps[i].path);
            s_caps[i].path[0] = 0;
            got = 1;
        }
    cap_unlock();
    return got;
}

struct mark_job { LONG n; };

static DWORD WINAPI mark_worker(LPVOID p)
{
    struct mark_job *j = (struct mark_job *)p;
    char path[MAX_PATH], kind[16];
    Sleep(700);                                    /* the first screenshot is taken by then */
    snprintf(kind, sizeof kind, "mark%ld", (long)j->n);
    if (j->n <= MARK_DUMPS) {
        BOOL ok = write_dump(kind, NULL, path, sizeof path);
        fprintf(stderr, "[MARK] #%ld dump %s: %s\n", (long)j->n, ok ? "written" : "FAILED", path);
    }
    free(j);
    return 0;
}

void nhl_debug_mark(void)
{
    static ULONGLONG last;
    ULONGLONG t = GetTickCount64();
    LONG n;
    int i, k;
    static const DWORD delays[3] = { 0, 1500, 4000 };
    char l2[96], base[MAX_PATH];
    struct mark_job *j;
    if (s_crashing || t - last < 700) return;       /* a held key or a double press counts once */
    last = t;
    n = InterlockedIncrement(&s_marks);
    fprintf(stderr, "[MARK] #%ld t=%.3f frames=%llu: DEL pressed, a graphical problem is here (screenshots mark%ld_*.bmp, dump mark%ld_*.dmp)\n",
            (long)n, now_s(), (unsigned long long)nhl_flip_count(), (long)n, (long)n);
    cap_lock();
    for (k = 0; k < 3; k++) {
        for (i = 0; i < 24 && s_caps[i].path[0]; i++)
            ;
        if (i == 24) break;
        snprintf(base, sizeof base, "%s\\mark%ld_%s_%d", s_dir, (long)n, s_stamp, k + 1);
        snprintf(s_caps[i].path, sizeof s_caps[i].path, "%s", base);
        s_caps[i].due = t + delays[k];
    }
    cap_unlock();
    j = (struct mark_job *)malloc(sizeof *j);
    if (j) {
        j->n = n;
        CloseHandle(CreateThread(NULL, 0, mark_worker, j, 0, NULL));
    }
    snprintf(l2, sizeof l2, "Issue #%ld marked at %.0f s", (long)n, now_s());
    show_banner("MARKED", l2, "Saving screenshots and info. Keep playing: the debug files are zipped to your Desktop when you quit.",
                2800, RGB(80, 200, 120));
}

#endif /* NHL_DEBUGLOG */
