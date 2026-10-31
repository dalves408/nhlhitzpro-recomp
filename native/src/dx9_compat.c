/*
 * Windows 7 compatibility for the Direct3D 9 build (NHL_DX9).
 *
 * The Windows SDK's xinput.lib imports XINPUT1_4.DLL, which Windows 7 does not have (it has xinput1_3 with
 * the DirectX June 2010 runtime, and xinput9_1_0 always). These three functions are all the toolkit's input
 * layer and the overrides use; defining them here means the exe imports no XInput DLL and finds one when the
 * first controller is polled.
 */
#include <windows.h>

#ifdef NHL_DX9

typedef DWORD (WINAPI *xi_state_fn)(DWORD, void *);
typedef DWORD (WINAPI *xi_caps_fn)(DWORD, DWORD, void *);

static xi_state_fn s_get_state;
static DWORD (WINAPI *s_set_state)(DWORD, void *);
static xi_caps_fn s_get_caps;
static LONG s_xi_init;                    /* 0: not tried, 1: loading, 2: done */

static void xi_load(void)
{
    static const char *const names[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    int i;
    if (s_xi_init == 2) return;
    if (InterlockedCompareExchange(&s_xi_init, 1, 0) != 0) {       /* another thread is loading */
        while (s_xi_init != 2) Sleep(1);
        return;
    }
    for (i = 0; i < 3; i++) {
        HMODULE m = LoadLibraryA(names[i]);
        if (!m) continue;
        s_get_state = (xi_state_fn)(void *)GetProcAddress(m, "XInputGetState");
        s_set_state = (DWORD (WINAPI *)(DWORD, void *))(void *)GetProcAddress(m, "XInputSetState");
        s_get_caps = (xi_caps_fn)(void *)GetProcAddress(m, "XInputGetCapabilities");
        if (s_get_state) break;
    }
    MemoryBarrier();
    InterlockedExchange(&s_xi_init, 2);
}

DWORD WINAPI XInputGetState(DWORD user, void *state)
{
    xi_load();
    return s_get_state ? s_get_state(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputSetState(DWORD user, void *vib)
{
    xi_load();
    return s_set_state ? s_set_state(user, vib) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetCapabilities(DWORD user, DWORD flags, void *caps)
{
    xi_load();
    return s_get_caps ? s_get_caps(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
}

#endif
