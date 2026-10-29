/**
 * NHL Hitz Pro - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       NHL Hitz Pro
 *   Title ID:    0x00000000
 *   Base addr:   0x00010000
 *   Entry point: 0x00000000
 *   Code size:   ~??? KB (.text)
 *   Sections:    ?? (list them)
 *   Kernel imports: ??
 */

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS uint32_t g_mxcsr;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x001DB5E3  /* XBE entry point VA */
#ifdef NHL_RELEASE
/* Release layout: <install dir>\nhl_hitz_pro_recomp.exe and <install dir>\game_files\ */
#define YOUR_GAME_XBE_PATH      "game_files/default.xbe"
#define YOUR_GAME_DIR            "game_files"
#else
#define YOUR_GAME_XBE_PATH      "../game_files/default.xbe"
#define YOUR_GAME_DIR            "../game_files"
#endif

#ifdef NHL_RELEASE
/* Release build (cmake -DNHL_RELEASE=ON): no logging, no diagnostic switches,
 * and everything the game writes goes to %LOCALAPPDATA%\NHLHitzPro so an
 * install under Program Files works. */
static char s_user_data[MAX_PATH];     /* %LOCALAPPDATA%\NHLHitzPro\SaveData */

static void release_init(void)
{
    char exe[MAX_PATH], base[MAX_PATH], cache[MAX_PATH], *sl, *env, *p;

    /* Paths are relative to the install directory. */
    if (GetModuleFileNameA(NULL, exe, sizeof exe) && (sl = strrchr(exe, '\\'))) {
        *sl = 0;
        SetCurrentDirectoryA(exe);
    }
    /* No console output or log lines of any kind. */
    freopen("NUL", "w", stdout);
    freopen("NUL", "w", stderr);
    /* The RECOMP_* diagnostic and tuning switches are a developer interface:
     * drop whatever the environment carries and let the defaults below apply. */
    env = GetEnvironmentStringsA();
    if (env) {
        for (p = env; *p; p += strlen(p) + 1) {
            if (strncmp(p, "RECOMP_", 7) == 0) {
                char name[128];
                size_t n = strcspn(p, "=");
                if (n < sizeof name) {
                    memcpy(name, p, n);
                    name[n] = 0;
                    SetEnvironmentVariableA(name, NULL);
                    _putenv_s(name, "");
                }
            }
        }
        FreeEnvironmentStringsA(env);
    }
    if (GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base) == 0 || strlen(base) > MAX_PATH - 64)
        strcpy(base, ".");
    strcat(base, "\\NHLHitzPro");
    CreateDirectoryA(base, NULL);
    snprintf(s_user_data, sizeof s_user_data, "%s\\SaveData", base);
    snprintf(cache, sizeof cache, "%s\\shadercache", base);
    _putenv_s("RECOMP_D3D11_SHADERCACHE_DIR", cache);
}
#endif

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* RECOMP_CRASH_PEEK=va[>off...][,va...]: a game global and the block
         * it points to, for a pointer the stack does not lead to (a table's
         * base). Each >off follows the pointer stored at block+off first, so
         * 0x37B1E4>0x2C>0x4C is (*(*(*0x37B1E4+0x2C)+0x4C)). */
        if (getenv("RECOMP_CRASH_PEEK")) {
            const char *s = getenv("RECOMP_CRASH_PEEK");
            while (*s) {
                char *end;
                uint32_t va = (uint32_t)strtoul(s, &end, 0), v;
                int j;
                if (end == s)
                    break;
                v = *(const uint32_t *)((uintptr_t)g_xbox_mem_offset + va);
                fprintf(stderr, "  peek [0x%08X]=0x%08X", va, v);
                while (*end == '>' && v >= 0x00010000u && v < 0x04000000u) {
                    uint32_t off = (uint32_t)strtoul(end + 1, &end, 0);
                    va = v + off;
                    v = *(const uint32_t *)((uintptr_t)g_xbox_mem_offset + va);
                    fprintf(stderr, " >[0x%08X]=0x%08X", va, v);
                }
                while (*end && *end != ',')
                    end++;
                if (v >= 0x00010000u && v < 0x04000000u) {
                    const uint32_t *p =
                        (const uint32_t *)((uintptr_t)g_xbox_mem_offset + v);
                    for (j = 0; j < 32; j++)
                        fprintf(stderr, "%s%08X", j % 8 ? " " : "\n     ", p[j]);
                }
                fprintf(stderr, "\n");
                s = *end ? end + 1 : end;
            }
        }

        /* RECOMP_CRASH_DEEP: the raw stack words, and what sits behind each
         * one that points into guest RAM. A fault on data a caller handed
         * down (a bad pointer read out of a game structure) needs the
         * structure, and the callers' saved registers on this stack are the
         * only route to it once the process is gone. */
        if (getenv("RECOMP_CRASH_DEEP")) {
            int j;
            fprintf(stderr, "  guest stack words:\n");
            for (i = 0; i < 48; i += 8) {
                fprintf(stderr, "    [esp+%-4d]", i * 4);
                for (j = 0; j < 8; j++)
                    fprintf(stderr, " %08X", sp[i + j]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < 48; i++) {
                uint32_t v = sp[i];
                const uint32_t *p;
                if (v < 0x00010000u || v >= 0x04000000u || (v & 3)
                        || (v > g_xbox_code_lo && v < g_xbox_code_hi))
                    continue;
                p = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + v);
                fprintf(stderr, "  *[esp+%d]=0x%08X:", i * 4, v);
                for (j = 0; j < 24; j++)
                    fprintf(stderr, "%s%08X", j % 8 ? " " : "\n     ", p[j]);
                fprintf(stderr, "\n");
            }
        }

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched. The VEH below now reaches apu_hook_handle_mmio
 * through the toolkit's xbox_VehMmioFault.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* Trapped hardware registers (APU, USB OHCI) -> their models.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        if (xbox_VehMmioFault(ep))
            return EXCEPTION_CONTINUE_EXECUTION;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  TLS: fs_base=0x%08X fs4=0x%08X index=0x%08X\n",
                g_fs_base,
                *(uint32_t *)((uintptr_t)g_xbox_mem_offset + g_fs_base + 4),
                *(uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x596AE8));
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── WinMain ───────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    {
        /* Per-monitor DPI awareness (Windows 10 1703+): without it Windows draws the game
         * at 96 dpi and stretches it, which blurs fullscreen on a scaled (e.g. 4K at 200%)
         * display. The window then works in real pixels. */
        typedef BOOL (WINAPI *set_dpi_ctx_fn)(HANDLE);
        HMODULE u32 = GetModuleHandleA("user32.dll");
        set_dpi_ctx_fn f = u32 ? (set_dpi_ctx_fn)(void *)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : NULL;
        if (f) f((HANDLE)(INT_PTR)-4);          /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
    }
#ifdef NHL_RELEASE
    release_init();
#endif

    printf("=== NHL Hitz Pro - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);

    /* Step 1: Load XBE */
    if (!load_xbe(YOUR_GAME_XBE_PATH, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Failed to load default.xbe.\n"
                    "Run scripts/run.ps1 from the project; data is in game_files.",
                    "Recomp", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Statically linked Xbox D3D submits NV2A commands directly. */
    if (!getenv("RECOMP_AC97_READY")) _putenv_s("RECOMP_AC97_READY", "1");
    if (!getenv("RECOMP_PB_EXEC")) _putenv_s("RECOMP_PB_EXEC", "1");
    /* Boot hang: XPP waits on a KEVENT it builds on the stack (see recomp_manual.c, XPP synchronous URB wait).
     * Default on since 2026-10-29: 0 hangs in 60 boots with it vs 4 in 60 without; every
     * XPP wait returns 0 with it (they fail at once without). RECOMP_XPP_WAIT_FIX=0 turns it off. */
    {
        const char *f = getenv("RECOMP_XPP_WAIT_FIX");
        if ((!f || f[0] != '0') && !getenv("RECOMP_TITLE_KEVENTS"))
            _putenv_s("RECOMP_TITLE_KEVENTS", "1");
    }
    if (!getenv("RECOMP_PB_WALK")) _putenv_s("RECOMP_PB_WALK", "dma");
    /* The XMV player paces video on D3D's VBlankCount (device+0x1DD8), which
     * only D3D's vblank DPC advances. Without delivered vblanks the intro
     * movie never gets past its first frame. */
    if (!getenv("RECOMP_VBLANK")) _putenv_s("RECOMP_VBLANK", "paced");
    /* The D3D11 back end is the default renderer since 2026-10-29 (matches at
     * 60+ fps versus ~1 fps on the CPU). RECOMP_D3D11=0 selects the software
     * renderer. */
    if (!getenv("RECOMP_D3D11")) _putenv_s("RECOMP_D3D11", "1");
    /* With the D3D11 back end nothing slows the title down any more: menus
     * flipped 400-1,400 times a second. The console holds a frame that is
     * ready early to the next vblank (D3D's presentation interval); "edge"
     * does the same and never holds a late frame. Menus then run at 60.0 fps
     * and the match is unaffected (it is below 60). */
    if (getenv("RECOMP_D3D11") && getenv("RECOMP_D3D11")[0] == '1'
            && !getenv("RECOMP_FLIP_PACING"))
        _putenv_s("RECOMP_FLIP_PACING", "edge");
    /* The rest of the D3D11 set, verified together on an RTX 3070 PC
     * (2026-10-28, quick_match claude_full3: match median 70 fps, no smear,
     * shadows, game clock 1:1 with real time): combiner shaders, vertex
     * programs on the GPU, one depth buffer per guest zeta (offscreen
     * shadow layers), and the INLINE_ARRAY short-attribute size fix. Each
     * defaults on with RECOMP_D3D11=1; NAME=0 turns it off (removed here,
     * because some of them treat any value as on). */
    if (getenv("RECOMP_D3D11") && getenv("RECOMP_D3D11")[0] == '1') {
        /* Also on since 2026-10-29: the rest of the earlier opt-in set, which
         * Confirmed in play: the NV09F/NV062
         * blit (loading/tip screen), GPU vertex shaders compiled on a worker
         * (scoreboard-pan stall), the on-disk shader cache (match-start
         * hitches), whole-texture decode, depth-as-texture (depth of field),
         * behind-eye z for GPU vertex programs (entrance light beam), 4:3
         * window/present, F11 borderless fullscreen, and the keyboard as the
         * controller (merged with a real pad, only while the window has
         * focus). */
        static const char *const d3d11_set[] = {
            "RECOMP_D3D11_RC", "RECOMP_D3D11_GPUVS", "RECOMP_D3D11_SHAREZ",
            "RECOMP_INLINE_SHORTFIX",
            "RECOMP_NV_BLIT", "RECOMP_D3D11_GPUVS_ASYNC", "RECOMP_D3D11_SHADERCACHE",
            "RECOMP_TEXDEC_FAST", "RECOMP_D3D11_ZTEX", "RECOMP_D3D11_GPUVS_FARCLIP",
            "RECOMP_ASPECT_4_3", "RECOMP_F11_FULLSCREEN", "RECOMP_KEYBOARD",
        };
        size_t i;
        for (i = 0; i < sizeof d3d11_set / sizeof d3d11_set[0]; i++) {
            const char *v = getenv(d3d11_set[i]);
            if (!v)
                _putenv_s(d3d11_set[i], "1");
            else if (v[0] == '0')
                _putenv_s(d3d11_set[i], "");       /* empty = remove */
        }
        /* CPU vertex-transform pool for draws the GPU path can't take
         * (measured ~+10% on 8c/16t). */
        if (!getenv("RECOMP_VX_THREADS")) _putenv_s("RECOMP_VX_THREADS", "8");
        if (!getenv("RECOMP_VX_MIN")) _putenv_s("RECOMP_VX_MIN", "48");
        /* The back end has its own window; the toolkit's framebuffer window
         * would only show guest memory beside it (Play.bat and run.ps1 set
         * it for the software renderer). */
        if (getenv("RECOMP_FB_WINDOW"))
            _putenv_s("RECOMP_FB_WINDOW", "");
    }
    /* One-CPU exclusion between the title at raised IRQL and interrupt
     * delivery. Without it DirectSound's interrupt-side front-end writes
     * interleave with the title's, the movie audio stream's segment list is
     * corrupted, and the intro FMV freezes partway (1 run in 2 without, 0 in
     * 3 with). */
    if (!getenv("RECOMP_DISPATCH_GATE")) _putenv_s("RECOMP_DISPATCH_GATE", "1");
    /* Contiguous memory above the image's physical range, as the console's
     * kernel keeps it. This title's sections run to ~0x5A8000, and blocks
     * placed under that give one physical address two meanings: XPP's OHCI
     * descriptors at 0x00402000 resolved to the image and read back zero, so
     * the pad never enumerated. */
    if (!getenv("RECOMP_CONTIG_AVOID_IMAGE")) _putenv_s("RECOMP_CONTIG_AVOID_IMAGE", "1");
    /* An emulated controller on port 1 (USB host controllers + pad), driven
     * by an XInput controller or RECOMP_PAD_SCRIPT, and by the keyboard with
     * RECOMP_KEYBOARD (on with the D3D11 back end, above). The black-screen
     * stall once blamed on the keyboard was the boot hang, which happened
     * without it too and is fixed by RECOMP_XPP_WAIT_FIX. */
    if (!getenv("RECOMP_USB")) _putenv_s("RECOMP_USB", "1");

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* Bring up the emulated USB host controllers (and the pad behind them).
     *
     * Another entry point with no caller in the tree: ohci.c traps its
     * registers and the memory layout's fault handler routes them to
     * xbox_OhciHandleMmio, but only xbox_OhciInit arms either, and nothing
     * called it -- so RECOMP_USB=1 alone left the title with no controller
     * and every RECOMP_PAD_* switch silent. It returns at once unless
     * RECOMP_USB is set, and needs guest memory mapped. */
    {
        extern void xbox_OhciInit(void);
        xbox_OhciInit();
    }

    /* Optional Direct3D 11 render back end (native/src/backend_d3d11.c): the
     * executor hands it decoded draws instead of rasterising on the CPU. Off
     * unless RECOMP_D3D11=1; the software renderer stays the default. */
    if (getenv("RECOMP_D3D11") && getenv("RECOMP_D3D11")[0] == '1') {
        extern void backend_d3d11_register(void);
        backend_d3d11_register();
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
#ifdef NHL_RELEASE
        xbox_path_init(YOUR_GAME_DIR, s_user_data);
#else
        xbox_path_init(YOUR_GAME_DIR, "../runtime/SaveData");
#endif
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();
    { extern void nhl_watch_start(void); nhl_watch_start(); }

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
