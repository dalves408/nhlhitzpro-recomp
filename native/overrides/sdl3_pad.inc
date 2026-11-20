/* SDL3 gamepad layer shared by the Linux player and the Windows player.
 *
 * Included by overrides/linux/xinput_device.c (every pad comes from SDL) and
 * overrides/xinput_device.c (Windows: XInput keeps the Xbox pads, SDL takes
 * the others). SDL maps Xbox, PlayStation, Switch and most generic pads onto
 * one layout from its own database (plus SDL_GAMECONTROLLERCONFIG and
 * gamecontrollerdb.txt), so nothing here depends on which pad is plugged in.
 * Rumble is SDL_RumbleGamepad. Pads plugged in after the start are picked up
 * once a second and a removed pad frees its slot.
 *
 * The includer defines, before including this file:
 *   SDLPAD_WANTED(gamepad)   non-zero when SDL should take that pad
 *   SDLPAD_SLOT_FREE(slot)   non-zero when SDL may use that port
 * and has <SDL3/SDL.h>, "xinput_xbox.h" and <stdio.h> / <string.h> / <stdlib.h>
 * included.
 */
#ifndef SDLPAD_WANTED
#define SDLPAD_WANTED(pad) 1
#endif
#ifndef SDLPAD_SLOT_FREE
#define SDLPAD_SLOT_FREE(slot) 1
#endif

static SDL_Gamepad   *sp_pads[XBOX_MAX_CONTROLLERS];
static SDL_JoystickID sp_ids[XBOX_MAX_CONTROLLERS];
static DWORD          sp_packet[XBOX_MAX_CONTROLLERS];
static Uint64         sp_last_scan;

static int sp_log_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMP_RUMBLE_LOG"); v = (e && *e == '1') || getenv("RECOMP_PAD_LOG"); }
    return v;
}

static int sdlpad_any_open(void)
{
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (sp_pads[i])
            return 1;
    return 0;
}

static int sdlpad_slot_used(int slot)
{
    return sp_pads[slot] != NULL;
}

static void sp_describe(int slot, SDL_Gamepad *pad)
{
    SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
    const char *type = "unknown";
    switch (SDL_GetGamepadType(pad)) {
    case SDL_GAMEPAD_TYPE_XBOX360:               type = "Xbox 360"; break;
    case SDL_GAMEPAD_TYPE_XBOXONE:               type = "Xbox One/Series"; break;
    case SDL_GAMEPAD_TYPE_PS3:                   type = "PlayStation 3"; break;
    case SDL_GAMEPAD_TYPE_PS4:                   type = "PlayStation 4"; break;
    case SDL_GAMEPAD_TYPE_PS5:                   type = "PlayStation 5"; break;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:   type = "Switch Pro"; break;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: type = "Switch Joy-Con pair"; break;
    case SDL_GAMEPAD_TYPE_STANDARD:              type = "standard"; break;
    default: break;
    }
    fprintf(stderr, "[PAD] slot %d (SDL): \"%s\" type %s, vendor %04X product %04X, rumble %s, mapping %s\n",
            slot, SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "?", type,
            (unsigned)SDL_GetGamepadVendor(pad), (unsigned)SDL_GetGamepadProduct(pad),
            SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? "yes" : "no",
            SDL_GetGamepadMapping(pad) ? "found" : "none");
}

/* Open every attached gamepad that has no slot yet and that SDL should take. */
static void sdlpad_open_new(void)
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (!ids)
        return;
    for (int i = 0; i < count; i++) {
        int have = 0, free_slot = -1;
        for (int s = 0; s < XBOX_MAX_CONTROLLERS; s++) {
            if (sp_pads[s] && sp_ids[s] == ids[i]) have = 1;
            if (!sp_pads[s] && free_slot < 0 && SDLPAD_SLOT_FREE(s)) free_slot = s;
        }
        if (have || free_slot < 0)
            continue;
        {
            SDL_Gamepad *pad = SDL_OpenGamepad(ids[i]);
            if (!pad) {
                fprintf(stderr, "[PAD] could not open gamepad %u: %s\n", (unsigned)ids[i], SDL_GetError());
                continue;
            }
            if (!SDLPAD_WANTED(pad)) {
                SDL_CloseGamepad(pad);
                continue;
            }
            sp_pads[free_slot] = pad;
            sp_ids[free_slot] = ids[i];
            sp_describe(free_slot, pad);
        }
    }
    SDL_free(ids);
}

static void sdlpad_init(void)
{
    /* Keep the default SIGINT/SIGTERM: SDL would turn them into an SDL_EVENT_QUIT
     * that nothing reads without a window, and Ctrl-C would stop working. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    sdlpad_open_new();
}

/* At most once a second: free the slots of removed pads and open new ones.
 * `force` rescans now (a slot just lost its XInput pad, for instance). */
static void sdlpad_rescan(int force)
{
    Uint64 now = SDL_GetTicks();
    if (!force && now - sp_last_scan < 1000)
        return;
    sp_last_scan = now;
    /* SDL learns about plugged and unplugged devices when it is updated; with no pad open nothing
     * else calls that, so a pad plugged in later would never show up. */
    SDL_UpdateGamepads();
    for (int s = 0; s < XBOX_MAX_CONTROLLERS; s++) {
        if (sp_pads[s] && (!SDL_GamepadConnected(sp_pads[s]) || !SDLPAD_SLOT_FREE(s))) {
            if (sp_log_on())
                fprintf(stderr, "[PAD] slot %d (SDL): gamepad released\n", s);
            SDL_CloseGamepad(sp_pads[s]);
            sp_pads[s] = NULL;
        }
    }
    sdlpad_open_new();
}

/* Fill pState from the SDL pad in `slot`. Returns 0 when there is none. */
static int sdlpad_get(DWORD slot, XBOX_INPUT_STATE *pState)
{
    SDL_Gamepad *c;
    WORD btn = 0;
    if (slot >= XBOX_MAX_CONTROLLERS)
        return 0;
    c = sp_pads[slot];
    if (!c || !SDL_GamepadConnected(c))
        return 0;

    SDL_UpdateGamepads();

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++sp_packet[slot];

    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_LEFT_STICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_SOUTH) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_EAST) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_WEST) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_NORTH) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFTX);
    pState->Gamepad.sThumbLY = (SHORT)(-1 - SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY = (SHORT)(-1 - SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHTY));
    return 1;
}

/* Rumble on the SDL pad in `slot`. Returns -1 when there is none, else 0. */
static int sdlpad_rumble(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    SDL_Gamepad *c;
    static int log = -1, logged;
    if (log < 0) { const char *e = getenv("RECOMP_RUMBLE_LOG"); log = e && *e == '1'; }
    c = dwPort < XBOX_MAX_CONTROLLERS ? sp_pads[dwPort] : NULL;
    if (!c) {
        if (log && logged < 50) { logged++; fprintf(stderr, "[RUMBLE] pad %lu %u/%u: no controller open\n",
            (unsigned long)dwPort, pVibration->wLeftMotorSpeed, pVibration->wRightMotorSpeed); }
        return -1;
    }

    /* XInput keeps the motors at the last values until they change; a title
     * may send a value once and stop. SDL needs a duration, so hold it for
     * the longest SDL allows and re-send on every report. */
    {
        /* Hitz pulses both motors at 0xFFFF and stops them a few frames
         * later. Many pads barely spin up in that time, so a stop that lands
         * sooner than RECOMP_RUMBLE_MIN_MS (default 250) after the start
         * becomes a short timed rumble: SDL ends it itself. */
        static Uint64 on_at[XBOX_MAX_CONTROLLERS];
        static int min_ms = -1;
        Uint64 now;
        Uint16 lo, hi;
        Uint32 dur = 0xFFFF;
        int held = 0;
        bool ok;
        if (min_ms < 0) { const char *e = getenv("RECOMP_RUMBLE_MIN_MS"); min_ms = e ? atoi(e) : 250; if (min_ms < 0) min_ms = 0; }
        now = SDL_GetTicks();
        lo = pVibration->wLeftMotorSpeed;
        hi = pVibration->wRightMotorSpeed;
        if (lo || hi) {
            on_at[dwPort] = now ? now : 1;
        } else if (on_at[dwPort] && now - on_at[dwPort] < (Uint64)min_ms) {
            dur = (Uint32)((Uint64)min_ms - (now - on_at[dwPort]));
            held = 1; on_at[dwPort] = 0;
            lo = hi = 0xFFFF; /* Hitz only ever uses full strength */
        } else {
            on_at[dwPort] = 0;
        }
        ok = SDL_RumbleGamepad(c, lo, hi, dur);
        if (log && logged < 2000) { logged++; fprintf(stderr, "[RUMBLE] t=%llu pad %lu (SDL) left %u right %u%s rc %d%s%s\n",
            (unsigned long long)now, (unsigned long)dwPort, pVibration->wLeftMotorSpeed, pVibration->wRightMotorSpeed,
            held ? " (stop deferred)" : "", ok ? 0 : -1, ok ? "" : " ", ok ? "" : SDL_GetError()); }
    }
    return 0;
}
