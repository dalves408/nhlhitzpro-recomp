/**
 * Xbox Input Compatibility Layer -- Windows, SDL3 only.
 *
 * Used instead of the toolkit's xinput_device.c when the player is built with
 * -DNHL_SDL3=ON (the 64-bit Windows build). Every controller (Xbox, PlayStation,
 * Switch, generic pads...) comes from SDL3's gamepad layer, and rumble goes
 * through SDL_RumbleGamepad. The 32-bit / XP builds keep the toolkit's XInput code.
 *
 * Player numbers: a pad is given the lowest free port when it is first seen and
 * keeps it for as long as it stays connected, so plugging in another pad never
 * changes the number of one that is already playing (see sdl3_pad.inc).
 */

#include "xinput_xbox.h"
#include "keyboard.h"
#include "mkb_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

static BOOL g_controller_connected[XBOX_MAX_CONTROLLERS];

#define SDLPAD_WANTED(pad)     1          /* every gamepad SDL knows */
#define SDLPAD_SLOT_FREE(slot) 1
#include "sdl3_pad.inc"

/* Merge the keyboard on top of a pad on port 0 rather than only standing in
 * for a missing one (see the toolkit's xinput_device.c for the history). */
static void keyboard_merge(XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    int i;

    if (!xbox_InputKeyboardPoll(&kb))
        return;
    pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
            pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
    /* The input layer records edges, so an unchanged packet number is
     * read as the same state and the press never happens. */
    pState->dwPacketNumber = kb.dwPacketNumber;
}

/* The Windows player never calls xbox_InputInit, so the first use starts SDL. The pad state is read from the
 * game thread and, for the overlays, from the window thread: one lock covers SDL's side of it. */
static CRITICAL_SECTION g_lock;
static volatile LONG g_state;       /* 0 = not started, 1 = starting, 2 = ready */

static void ensure_init(void)
{
    if (g_state == 2)
        return;
    if (InterlockedCompareExchange(&g_state, 1, 0) == 0) {
        InitializeCriticalSection(&g_lock);
        sdlpad_init();
        if (sp_log_on() && !sdlpad_any_open())
            fprintf(stderr, "[PAD] SDL started; no gamepad found yet (one plugged in later is picked up)\n");
        InterlockedExchange(&g_state, 2);
        return;
    }
    while (g_state != 2)
        Sleep(1);
}

void xbox_InputInit(void)
{
    ensure_init();
}

/* Look for pads that came or went, now and then (once a second inside). */
static void pads_scan(int force)
{
    EnterCriticalSection(&g_lock);
    sdlpad_rescan(force);
    LeaveCriticalSection(&g_lock);
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    int ok;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    ensure_init();
    if (dwPort == 0)
        pads_scan(0);

    EnterCriticalSection(&g_lock);
    ok = sdlpad_get(dwPort, pState);
    LeaveCriticalSection(&g_lock);

    if (!ok) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0) {
            /* Keyboard and mouse stand in for an absent pad, so the game is
             * playable with no controller plugged in. */
            int kb;
            memset(pState, 0, sizeof *pState);
            kb = xbox_InputKeyboardPoll(pState);
            if (mkb_merge(dwPort, pState))
                g_controller_connected[dwPort] = TRUE;
            if (kb || mkb_enabled())
                return ERROR_SUCCESS;
        }
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    g_controller_connected[dwPort] = TRUE;

    if (dwPort == 0)
        keyboard_merge(pState);

    /* Mouse+keyboard merges on top of a real pad too: the mouse owns the
     * aim stick while it is moving, WASD owns movement while held. */
    if (dwPort == 0)
        mkb_merge(dwPort, pState);

    return ERROR_SUCCESS;
}

/* 1 when a pad is connected on `port`. The USB layer asks this every quarter of a second to decide which
 * players are plugged in. */
int hitz_PadConnected(DWORD port)
{
    int connected;
    if (port >= XBOX_MAX_CONTROLLERS) return 0;
    ensure_init();
    pads_scan(0);
    EnterCriticalSection(&g_lock);
    connected = sdlpad_slot_used((int)port) && SDL_GamepadConnected(sp_pads[port]);
    LeaveCriticalSection(&g_lock);
    return connected;
}

/* The pad on `port` in XInput's own button layout (the layout the overlays and the debug log use), with no
 * keyboard or mouse merged in: the settings and quit overlays are driven by pad 1 only. Returns 0 when there is
 * no pad on that port. */
int hitz_PadPeek(DWORD port, WORD *buttons, BYTE *lt, BYTE *rt,
                 SHORT *lx, SHORT *ly, SHORT *rx, SHORT *ry)
{
    XBOX_INPUT_STATE st;
    WORD b;
    int ok;
    if (port >= XBOX_MAX_CONTROLLERS)
        return 0;
    ensure_init();
    EnterCriticalSection(&g_lock);
    ok = sdlpad_get(port, &st);
    LeaveCriticalSection(&g_lock);
    if (!ok)
        return 0;
    b = st.Gamepad.wButtons & 0x00FF;
    if (st.Gamepad.bAnalogButtons[XBOX_BUTTON_A]) b |= 0x1000;
    if (st.Gamepad.bAnalogButtons[XBOX_BUTTON_B]) b |= 0x2000;
    if (st.Gamepad.bAnalogButtons[XBOX_BUTTON_X]) b |= 0x4000;
    if (st.Gamepad.bAnalogButtons[XBOX_BUTTON_Y]) b |= 0x8000;
    if (st.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK]) b |= 0x0100;
    if (st.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE]) b |= 0x0200;
    *buttons = b;
    *lt = st.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER];
    *rt = st.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER];
    *lx = st.Gamepad.sThumbLX; *ly = st.Gamepad.sThumbLY;
    *rx = st.Gamepad.sThumbRX; *ry = st.Gamepad.sThumbRY;
    return 1;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    int rc = -1;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    ensure_init();
    EnterCriticalSection(&g_lock);
    if (sdlpad_slot_used((int)dwPort))
        rc = sdlpad_rumble(dwPort, pVibration);     /* includes the minimum pulse (RECOMP_RUMBLE_MIN_MS) */
    LeaveCriticalSection(&g_lock);
    return rc < 0 ? ERROR_DEVICE_NOT_CONNECTED : ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort] || sdlpad_slot_used((int)dwPort);
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    ensure_init();
    if (!sdlpad_slot_used((int)dwPort))
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = 1;       /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;    /* XINPUT_DEVSUBTYPE_GAMEPAD */
    return ERROR_SUCCESS;
}
