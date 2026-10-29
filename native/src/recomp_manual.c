/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>   /* getenv */

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern __declspec(thread) uint32_t g_eax;
extern ptrdiff_t g_xbox_mem_offset;

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
/* Task 3 fix attempt: complete queued IO callbacks on a LATER pump, as on hardware.
 *
 * The game's IO job list (head 0x3ADCFC) is drained by sub_001C9FC0. sub_001CA200
 * queues a read job plus a callback job (handler sub_001C9790, which calls the
 * completion routine, e.g. the loader's sub_0015C3D0) and then pumps immediately.
 * On the Xbox the read is overlapped, so the callback job does not run in that
 * first pump; it runs in a later one. Here reads finish synchronously, so the
 * completion ran nested inside the request. The group loader sub_00028AC0 does
 * "counter++ ; request(cb=sub_00028A70)" and sub_00028A70 posts event 0x117 when
 * the counter returns to 0, so with nested completion the counter hit 0 after
 * every request and 0x117 (load NewCrowd.pck) was posted once per group.
 *
 * sub_001C9790 is reached through an indirect call ([job+0x34]), so it can be
 * overridden here without regenerating. First visit of a job: report "pending"
 * (return 0, leave it queued; the pump stops). Second visit: run the real one.
 * RESULT: this STALLED the boot (nothing pumps the list again), so it is OFF by default.
 * RECOMP_DEFER_IO_CB=1 turns it on (diagnostic only).
 */
extern void sub_001C9790(void);
static int nhl_trace_loader = -1;
#define NHL_G32(a) (*(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + (a)))
extern __declspec(thread) uint32_t g_esp;
#define NHL_DEFER_SLOTS 256
static uint32_t nhl_defer_seen[NHL_DEFER_SLOTS];
static int nhl_defer_state = -1; /* -1 unread, 0 off, 1 on */
static unsigned nhl_defer_log;

static void nhl_io_cb_job_handler(void)
{
    uint32_t job = *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp + 8);
    int i, free_slot = -1;
    /* Only the loader's own completion job is deferred; other IO callbacks keep their timing. */
    if (NHL_G32(job + 0x2C) != 0x0015C3D0u) { sub_001C9790(); return; }
    for (i = 0; i < NHL_DEFER_SLOTS; i++) {
        if (nhl_defer_seen[i] == job) {
            nhl_defer_seen[i] = 0;          /* second visit: complete it */
            if (nhl_trace_loader > 0 && NHL_G32(job + 0x2C) == 0x0015C3D0u) { uint32_t s = g_esp; fprintf(stderr, "[DEFER] 2nd visit job 0x%08X cb=0x15C3D0 stack:"); for (int k = 0; k < 24; k++) fprintf(stderr, " %08X", NHL_G32(s + 4*k)); fprintf(stderr, "\n"); }
            sub_001C9790();
            return;
        }
        if (free_slot < 0 && nhl_defer_seen[i] == 0) free_slot = i;
    }
    if (free_slot < 0) { sub_001C9790(); return; } /* table full: old behavior */
    nhl_defer_seen[free_slot] = job;
    if (nhl_trace_loader > 0 && NHL_G32(job + 0x2C) == 0x0015C3D0u) { uint32_t s = g_esp; fprintf(stderr, "[DEFER] 1st visit job 0x%08X cb=0x15C3D0 stack:", job); for (int k = 0; k < 24; k++) fprintf(stderr, " %08X", NHL_G32(s + 4*k)); fprintf(stderr, "\n"); }
    if (nhl_defer_log < 40) {
        nhl_defer_log++;
        fprintf(stderr, "[DEFER] io callback job 0x%08X deferred to next pump (#%u)\n", job, nhl_defer_log);
    }
    g_esp += 4;   /* ret */
    g_eax = 0;    /* "pending": pump stops, job stays queued */
}
/* Diagnostic tracer (Task 3): RECOMP_TRACE_LOADER=1 logs the group-load completion
 * callbacks and the loader completion routine with the counter at guest 0x3FCDFC.
 * All of these are reached through indirect calls, so no regeneration is needed. */
extern void sub_00028A70(void);
extern void sub_00028AA0(void);
extern void sub_00028A90(void);
extern void sub_0015C3D0(void);
static void nhl_tr_28A70(void) { fprintf(stderr, "[LDR] sub_00028A70 (group done) counter=%d before\n", (int)NHL_G32(0x3FCDFC)); sub_00028A70(); fprintf(stderr, "[LDR]   -> counter=%d\n", (int)NHL_G32(0x3FCDFC)); }
static void nhl_tr_28AA0(void) { fprintf(stderr, "[LDR] sub_00028AA0 (crowd loaded cb)\n"); sub_00028AA0(); }
static void nhl_tr_28A90(void) { fprintf(stderr, "[LDR] sub_00028A90 (load FAIL cb)\n"); sub_00028A90(); }
static void nhl_tr_15C3D0(void) { fprintf(stderr, "[LDR] sub_0015C3D0 (load completion) queued=%d\n", (int)NHL_G32(0x584A98)); sub_0015C3D0(); }
/* Task 3 fix: coalesce the "all groups loaded" event (0x117) across one batch.
 *
 * Event handler sub_0002A4C0 (registered via push 0x2A4C0 at 0x2AFA4) requests a
 * batch of resource groups with sub_00028AC0: "counter[0x3FCDFC]++ ; async load,
 * on completion call sub_00028A70". sub_00028A70 does "if (--counter == 0) post
 * event 0x117", and 0x117 starts the NewCrowd.pck load. On the Xbox the loads
 * complete after the whole batch has been queued, so the counter reaches 0 once.
 * Here every load finishes synchronously inside its own request, so the counter
 * goes 1 -> 0 after EACH of the ~20 requests and 0x117 is posted ~20 times, each
 * time re-loading and re-parsing NewCrowd.pck (~2 MB of never-freed contiguous
 * memory per pass) until the 64 MB arena is exhausted.
 *
 * Fix: while sub_0002A4C0 is on the stack, a counter that reaches 0 only sets a
 * pending flag; when the outermost sub_0002A4C0 returns, if the counter is still 0
 * the post happens exactly once (by running the original sub_00028A70 with the
 * counter set to 1). Both functions are reached through indirect calls (a stored
 * handler pointer and a loader callback pointer), so this needs no regeneration.
 * RECOMP_COALESCE_LOAD=0 disables it.
 */
extern void sub_0002A4C0(void);
extern __declspec(thread) uint32_t g_ecx, g_edx;
static int nhl_coalesce_state = -1;
static int nhl_coalesce_depth;
static int nhl_coalesce_pending;
static unsigned nhl_coalesce_log;

static void nhl_coalesce_28A70(void)
{
    volatile uint32_t *ctr = (volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x3FCDFC);
    if (nhl_coalesce_depth > 0) {
        *ctr = *ctr - 1;
        if (*ctr == 0) nhl_coalesce_pending = 1;
        if (nhl_coalesce_log < 60) { nhl_coalesce_log++; fprintf(stderr, "[COALESCE] group done inside batch: counter=%d pending=%d\n", (int)*ctr, nhl_coalesce_pending); }
        g_esp += 4;   /* ret */
        return;
    }
    sub_00028A70();
}

static void nhl_coalesce_2A4C0(void)
{
    uint32_t esp0 = g_esp; static unsigned dbg; static int dbgon = -1;
    if (dbgon < 0) { const char *e = getenv("RECOMP_TRACE_LOADER"); dbgon = (e && *e == '1'); }
    nhl_coalesce_depth++;
    if (dbgon && dbg < 40) fprintf(stderr, "[COALESCE] 2A4C0 enter esp=%08X ret=%08X depth=%d\n", esp0, *(volatile uint32_t*)((uintptr_t)g_xbox_mem_offset + esp0), nhl_coalesce_depth);
    sub_0002A4C0();
    if (dbgon && dbg++ < 40) fprintf(stderr, "[COALESCE] 2A4C0 exit esp=%08X (expected %08X) eax=%08X\n", g_esp, esp0+4, g_eax);
    nhl_coalesce_depth--;
    if (nhl_coalesce_depth == 0 && nhl_coalesce_pending) {
        volatile uint32_t *ctr = (volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x3FCDFC);
        uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
        nhl_coalesce_pending = 0;
        if (*ctr == 0) {
            fprintf(stderr, "[COALESCE] batch finished, counter=0: posting event 0x117 once\n");
            *ctr = 1;
            g_esp -= 4;
            *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp) = 0; /* dummy return address */
            sub_00028A70();    /* --counter == 0 -> posts 0x117, pops the dummy */
            g_eax = eax; g_ecx = ecx; g_edx = edx;
        } else {
            fprintf(stderr, "[COALESCE] batch finished, counter=%d (loads still outstanding): not posting\n", (int)*ctr);
        }
    }
}
extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern int nhl_xpp_wait_hook_for(uint32_t xbox_va);
extern void nhl_xpp_wait_hook(void);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (xbox_va >= 0xFE000000u && nhl_xpp_wait_hook_for(xbox_va)) return nhl_xpp_wait_hook;
    if (xbox_va == 0x0002A4C0u || xbox_va == 0x00028A70u) {
        if (nhl_coalesce_state < 0) { const char *e = getenv("RECOMP_COALESCE_LOAD"); nhl_coalesce_state = !(e && *e == '0'); }
        if (nhl_coalesce_state) return xbox_va == 0x0002A4C0u ? nhl_coalesce_2A4C0 : nhl_coalesce_28A70;
    }
    if (nhl_trace_loader < 0) { const char *e = getenv("RECOMP_TRACE_LOADER"); nhl_trace_loader = (e && *e == '1'); }
    if (nhl_trace_loader) {
        if (xbox_va == 0x00028A70u && !nhl_coalesce_state) return nhl_tr_28A70;
        if (xbox_va == 0x00028AA0u) return nhl_tr_28AA0;
        if (xbox_va == 0x00028A90u) return nhl_tr_28A90;
        if (xbox_va == 0x0015C3D0u) return nhl_tr_15C3D0;
    }
    if (xbox_va == 0x001C9790u) {
        if (nhl_defer_state < 0) { const char *e = getenv("RECOMP_DEFER_IO_CB"); nhl_defer_state = (e && *e == '1'); }
        if (nhl_defer_state) return nhl_io_cb_job_handler;
    }
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    /* Who called: the guest return address is on top of the stack, and the
     * words below it that look like code addresses are the callers up the
     * chain. Informational only; the skip itself is unchanged. One buffer,
     * one write: other threads log to stderr concurrently. */
    {
        char buf[1400];
        int n;
        uint32_t s = g_esp, obj = NHL_G32(s + 4u), td = NHL_G32(s + 0x18u), k, shown = 0;
        n = snprintf(buf, sizeof buf, "[ICALL]   esp=0x%08X ret=0x%08X arg1=0x%08X arg2=0x%08X obj@arg1:",
                     s, NHL_G32(s), obj, NHL_G32(s + 8u));
        if (obj < 0x07F00000u)
            for (k = 0; k < 12 && n < 1000; k++)
                n += snprintf(buf + n, sizeof buf - n, " %08X", NHL_G32(obj + 4u * k));
        n += snprintf(buf + n, sizeof buf - n, " | td@[esp+0x18]=0x%08X:", td);
        if (td >= 0x80000000u && td < 0x84000000u)
            for (k = 0; k < 12 && n < 1100; k++)
                n += snprintf(buf + n, sizeof buf - n, " %08X", NHL_G32(td + 4u * k));
        n += snprintf(buf + n, sizeof buf - n, " | raw[esp+0x20..]:");
        for (k = 8; k < 16; k++)
            n += snprintf(buf + n, sizeof buf - n, " %08X", NHL_G32(s + 4u * k));
        {
            uint32_t o2 = NHL_G32(s + 0x28u), vt;
            if (o2 > 0x10000u && o2 < 0x07F00000u) {
                vt = NHL_G32(o2);
                n += snprintf(buf + n, sizeof buf - n, " | o2=0x%08X vt=0x%08X vt[0..3]:", o2, vt);
                if (vt > 0x10000u && vt < 0x07F00000u)
                    for (k = 0; k < 4; k++)
                        n += snprintf(buf + n, sizeof buf - n, " %08X", NHL_G32(vt + 4u * k));
            }
        }
        {
            /* [0x530000]+4 is the asset manager `this` that sub_000C81A0 caches in [0x4ED33C];
             * m = saved edi in sub_00155600's frame (only meaningful for the 0x14EBBB skip). */
            uint32_t g = NHL_G32(0x530000u), m1 = (g > 0x10000u && g < 0x07F00000u) ? NHL_G32(g + 4u) : 0,
                     m2 = NHL_G32(s + 0x44u);
            n += snprintf(buf + n, sizeof buf - n, " | mgr[0x530000]->+4=0x%08X:", m1);
            if (m1 > 0x10000u && m1 < 0x07F00000u)
                for (k = 0; k < 7; k++)
                    n += snprintf(buf + n, sizeof buf - n, " +%02X=%08X", 0xA8 + 4 * k, NHL_G32(m1 + 0xA8u + 4u * k));
            n += snprintf(buf + n, sizeof buf - n, " | mgr@[esp+0x44]=0x%08X", m2);
            if (m2 > 0x10000u && m2 < 0x07F00000u)
                n += snprintf(buf + n, sizeof buf - n, " +BC=%08X", NHL_G32(m2 + 0xBCu));
        }
        n += snprintf(buf + n, sizeof buf - n, " | code-range stack words:");
        for (k = 0; k < 64 && shown < 8 && n < 1300; k++) {
            uint32_t w = NHL_G32(s + 4u * k);
            if (w >= 0x00010000u && w < 0x005B0000u) {
                n += snprintf(buf + n, sizeof buf - n, " [+%02X]%08X", 4 * k, w);
                shown++;
            }
        }
        snprintf(buf + n, sizeof buf - n, "\n");
        fputs(buf, stderr);
    }    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * The default trap exits with code 86 at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = !trap || *trap != '0'; /* Fail closed unless explicitly disabled. */

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (default: stop; RECOMP_UNIMPL_TRAP=0 permits diagnostic continuation)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) _Exit(86); /* Diagnostic stop without the Debug CRT abort dialog. */
}

/* Guest RAM is coherent host-backed memory with no emulated CPU cache.
 * WBINVD therefore has no dirty guest lines to write back or invalidate.
 * Preserve memory ordering without executing the privileged host opcode.
 * This does not drain or acknowledge NV2A GPU commands.
 */
#include <windows.h>
void nhl_guest_wbinvd(void)
{
    MemoryBarrier();

}

/* Replace the GPU-interrupt sleep with a wait on the executor's actual
 * completion word. No submitted values are copied into completion memory.
 * Target is the fence record selected by D3D at guest 0x00254037.
 */
void nhl_wait_gpu_fence(uint32_t word, uint32_t target)
{
    volatile LONG *completed = (volatile LONG *)((uintptr_t)g_xbox_mem_offset + word);
    ULONGLONG start = GetTickCount64();
    while ((int32_t)((uint32_t)InterlockedCompareExchange(completed, 0, 0) - target) < 0) {
        if (GetTickCount64() - start > 10000) {
            fprintf(stderr, "[GPUWAIT] stalled: word=%08X completed=%08X target=%08X\n",
                    word, (uint32_t)*completed, target);
            _Exit(87);
        }
        Sleep(1);
    }
}

/* ---- Diagnostic: RECOMP_WATCH_VA=<hex guest VA> logs every write to that dword (default off).
 * The page holding it is made read-only; a vectored handler logs old/new value, thread id, time and
 * the code-range words on the guest stack (the guest callers), then single-steps the write and
 * re-protects. Writes by another thread during that one instruction are missed. Used to find who
 * stores [asset manager + 0xBC] (boot hang investigation). Not a fix. */
#include <windows.h>
static uintptr_t nhl_watch_host;          /* host address of the watched dword */
static uintptr_t nhl_watch_page;          /* host page base */
static uint32_t  nhl_watch_va;
static __declspec(thread) uint32_t nhl_watch_old;
static __declspec(thread) int nhl_watch_stepping;

static LONG CALLBACK nhl_watch_veh(EXCEPTION_POINTERS *ep)
{
    DWORD old;
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2 && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        if (a >= nhl_watch_page && a < nhl_watch_page + 0x1000) {
            nhl_watch_old = *(volatile uint32_t *)nhl_watch_host;
            VirtualProtect((void *)nhl_watch_page, 0x1000, PAGE_READWRITE, &old);
            nhl_watch_stepping = 1;
            ep->ContextRecord->EFlags |= 0x100;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    } else if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && nhl_watch_stepping) {
        uint32_t now = *(volatile uint32_t *)nhl_watch_host;
        nhl_watch_stepping = 0;
        VirtualProtect((void *)nhl_watch_page, 0x1000, PAGE_READONLY, &old);
        if (now != nhl_watch_old) {
            char buf[600]; int n; uint32_t s = g_esp, k, shown = 0;
            n = snprintf(buf, sizeof buf, "[WATCH] %08X: %08X -> %08X tid=%lu t=%llums esp=%08X stack:", nhl_watch_va,
                         nhl_watch_old, now, GetCurrentThreadId(), (unsigned long long)GetTickCount64(), s);
            if (s > 0x10000u && s < 0x07F00000u)
                for (k = 0; k < 96 && shown < 8; k++) {
                    uint32_t w = NHL_G32(s + 4u * k);
                    if (w >= 0x00010000u && w < 0x005B0000u) { n += snprintf(buf + n, sizeof buf - n, " [+%02X]%08X", 4 * k, w); shown++; }
                }
            snprintf(buf + n, sizeof buf - n, "\n");
            fputs(buf, stderr);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void nhl_watch_start(void)
{
    const char *e = getenv("RECOMP_WATCH_VA");
    DWORD old;
    if (!e || !*e) return;
    nhl_watch_va = (uint32_t)strtoul(e, NULL, 16);
    nhl_watch_host = (uintptr_t)g_xbox_mem_offset + nhl_watch_va;
    nhl_watch_page = nhl_watch_host & ~(uintptr_t)0xFFF;
    AddVectoredExceptionHandler(1, nhl_watch_veh);
    if (!VirtualProtect((void *)nhl_watch_page, 0x1000, PAGE_READONLY, &old))
        fprintf(stderr, "[WATCH] VirtualProtect failed err=%lu\n", GetLastError());
    else
        fprintf(stderr, "[WATCH] armed on guest VA 0x%08X (host page %p), value now %08X\n", nhl_watch_va, (void *)nhl_watch_page, *(volatile uint32_t *)nhl_watch_host);
}

/* XPP synchronous URB wait: diagnostic (RECOMP_XPP_WAIT_LOG=1), the fix itself is in main.c.
 *
 * sub_002F7659 (XPP's synchronous control request, called from the game's pad
 * init sub_000337E0/sub_00034000) builds its request object on the caller's
 * STACK (URB at [ebp-0x48], completion routine sub_002F8945 = KeSetEvent), and
 * builds the KEVENT it waits on by writing the header bytes itself
 * ([ebp-0x18]=0 type, [ebp-0x16]=4 size) -- no KeInitializeEvent. sub_002F8956
 * then calls KeWaitForSingleObject (import slot 0x2FEA00, call at 0x2F897C, return
 * 0x2F8983) with a 50 ms timeout. The kernel layer only knows events made by
 * KeInitializeEvent, so without RECOMP_TITLE_KEVENTS=1 it passes the stack address
 * to Win32 as a HANDLE and the wait fails AT ONCE with STATUS_UNSUCCESSFUL
 * (0xC0000001). XPP treats "not 0x102" as done and returns while the request is
 * still queued in the OHCI; the caller's frame is reused, and later the OHCI
 * completion (ISR thread, sub_002FDC32 -> 2FD8CD -> 2FD72B -> 2F8419) runs a
 * completion routine out of that recycled slot: the stale "[urb+8]=4" call
 * (skip 1, every boot), and when the slot is the live loader frame of
 * sub_00155600 it zeroes [loader+0x28] -> call through null (skip 2) -> the
 * font load never completes -> hang.
 * On the console the wait blocks until the OHCI completes the transfer.
 * RECOMP_XPP_WAIT_FIX=1 (main.c) turns RECOMP_TITLE_KEVENTS on, which makes
 * that wait real. This hook only logs the outcome of that one wait. */
#include <windows.h>
static int nhl_xpp_wait_log = -1;
static unsigned nhl_xpp_wait_n;

int nhl_xpp_wait_hook_for(uint32_t xbox_va)
{
    if (nhl_xpp_wait_log < 0) { const char *l = getenv("RECOMP_XPP_WAIT_LOG"); nhl_xpp_wait_log = (l && l[0] == '1'); }
    if (!nhl_xpp_wait_log) return 0;
    return xbox_va == NHL_G32(0x002FEA00u);   /* XPP's KeWaitForSingleObject import */
}

void nhl_xpp_wait_hook(void)
{
    uint32_t esp = g_esp;
    uint32_t ret = NHL_G32(esp);
    int ours = (ret == 0x002F8983u);
    unsigned long long t0 = ours ? GetTickCount64() : 0;
    recomp_func_t orig = recomp_lookup_kernel(NHL_G32(0x002FEA00u));
    uint32_t obj = NHL_G32(esp + 4);
    if (orig) orig();
    if (ours && nhl_xpp_wait_n++ < 32)
        fprintf(stderr, "[XPPWAIT] KeWait(obj %08X, from XPP 0x2F8983) -> status=0x%X after %llu ms (0x102 = timed out, 0xC0000001 = bad handle) tid=%lu\n",
                obj, (unsigned)g_eax, (unsigned long long)(GetTickCount64() - t0), GetCurrentThreadId());
}
