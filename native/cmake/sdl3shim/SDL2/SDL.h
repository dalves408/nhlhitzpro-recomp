/* SDL2-style API on top of SDL3, for the Linux player.
 *
 * Why this file exists: the Linux player and the vendored toolkit were written
 * against SDL2. SDL2 and SDL3 export the same function names with different
 * signatures, so one process cannot use both. Building with -DHITZ_SDL3=ON puts
 * this directory first on the include path, so `#include <SDL2/SDL.h>` and
 * `#include <SDL.h>` land here and the calls run on SDL3. Only the calls the
 * player actually uses are covered; the event, surface, audio and gamepad
 * code that depends on changed structures has SDL3 code under HITZ_SDL3.
 */
#ifndef HITZ_SDL2_ON_SDL3_H
#define HITZ_SDL2_ON_SDL3_H

#define HITZ_SDL3 1
#include <SDL3/SDL.h>
#include <stdlib.h>

/* ---- init / hints / version ---- */
#define SDL_INIT_GAMECONTROLLER SDL_INIT_GAMEPAD
#define SDL_QUIT                SDL_EVENT_QUIT
#define SDL_KEYDOWN             SDL_EVENT_KEY_DOWN
#define SDL_KEYUP               SDL_EVENT_KEY_UP
#define SDL_MOUSEBUTTONDOWN     SDL_EVENT_MOUSE_BUTTON_DOWN
#define SDL_MOUSEBUTTONUP       SDL_EVENT_MOUSE_BUTTON_UP
#define SDL_CONTROLLERBUTTONDOWN SDL_EVENT_GAMEPAD_BUTTON_DOWN
#define SDL_CONTROLLERBUTTONUP   SDL_EVENT_GAMEPAD_BUTTON_UP

/* SDL2 returned 0 on success from these; SDL3 returns true. */
static inline int hitz_sdl_Init(Uint32 flags)             { return SDL_Init(flags) ? 0 : -1; }
static inline int hitz_sdl_InitSubSystem(Uint32 flags)    { return SDL_InitSubSystem(flags) ? 0 : -1; }
#define SDL_Init(f)          hitz_sdl_Init(f)
#define SDL_InitSubSystem(f) hitz_sdl_InitSubSystem(f)

typedef struct { Uint8 major, minor, patch; } SDL_version;
static inline void hitz_sdl_GetVersion(SDL_version *v) {
    int n = SDL_GetVersion();
    v->major = (Uint8)SDL_VERSIONNUM_MAJOR(n);
    v->minor = (Uint8)SDL_VERSIONNUM_MINOR(n);
    v->patch = (Uint8)SDL_VERSIONNUM_MICRO(n);
}
#define SDL_GetVersion(v) hitz_sdl_GetVersion(v)
#define SDL_setenv(n, v, o) SDL_setenv_unsafe((n), (v), (o))
#define SDL_GetTicks64()    SDL_GetTicks()

/* ---- window / OpenGL ---- */
#define SDL_WINDOWPOS_CENTERED   0
#define SDL_WINDOWPOS_UNDEFINED  0
#define SDL_WINDOW_SHOWN         0
#define SDL_WINDOW_ALLOW_HIGHDPI SDL_WINDOW_HIGH_PIXEL_DENSITY
#define SDL_WINDOW_FULLSCREEN_DESKTOP SDL_WINDOW_FULLSCREEN
#define SDL_GLContext            SDL_GLContext

static inline SDL_Window *hitz_sdl_CreateWindow(const char *t, int x, int y, int w, int h, Uint64 f)
{ (void)x; (void)y; return SDL_CreateWindow(t, w, h, f); }
#define SDL_CreateWindow(t, x, y, w, h, f) hitz_sdl_CreateWindow((t), (x), (y), (w), (h), (f))

static inline int hitz_sdl_SetWindowFullscreen(SDL_Window *w, Uint64 f)
{ return SDL_SetWindowFullscreen(w, (f & SDL_WINDOW_FULLSCREEN) != 0) ? 0 : -1; }
#define SDL_SetWindowFullscreen(w, f) hitz_sdl_SetWindowFullscreen((w), (f))

static inline int hitz_sdl_GL_SetSwapInterval(int i) { return SDL_GL_SetSwapInterval(i) ? 0 : -1; }
#define SDL_GL_SetSwapInterval(i) hitz_sdl_GL_SetSwapInterval(i)
static inline int hitz_sdl_GL_SetAttribute(SDL_GLAttr a, int v) { return SDL_GL_SetAttribute(a, v) ? 0 : -1; }
#define SDL_GL_SetAttribute(a, v) hitz_sdl_GL_SetAttribute((a), (v))
static inline int hitz_sdl_GL_MakeCurrent(SDL_Window *w, SDL_GLContext c) { return SDL_GL_MakeCurrent(w, c) ? 0 : -1; }
#define SDL_GL_MakeCurrent(w, c) hitz_sdl_GL_MakeCurrent((w), (c))
static inline void hitz_sdl_GL_GetDrawableSize(SDL_Window *w, int *pw, int *ph) { SDL_GetWindowSizeInPixels(w, pw, ph); }
#define SDL_GL_GetDrawableSize(w, a, b) hitz_sdl_GL_GetDrawableSize((w), (a), (b))
static inline void hitz_sdl_SetWindowTitle(SDL_Window *w, const char *t) { SDL_SetWindowTitle(w, t); }
static inline void hitz_sdl_GL_SwapWindow(SDL_Window *w) { SDL_GL_SwapWindow(w); }
#define SDL_GL_SwapWindow(w) hitz_sdl_GL_SwapWindow(w)
static inline int hitz_sdl_PollEvent(SDL_Event *e) { return SDL_PollEvent(e) ? 1 : 0; }
#define SDL_PollEvent(e) hitz_sdl_PollEvent(e)

/* ---- threads, mutexes, conditions, atomics ---- */
typedef SDL_Condition SDL_cond;
typedef SDL_AtomicInt SDL_atomic_t;
#define SDL_CreateCond     SDL_CreateCondition
#define SDL_DestroyCond    SDL_DestroyCondition
#define SDL_CondBroadcast  SDL_BroadcastCondition
#define SDL_CondWait       SDL_WaitCondition
#define SDL_MUTEX_TIMEDOUT 1
/* SDL2: 0 when signalled, SDL_MUTEX_TIMEDOUT (1) on timeout. */
static inline int hitz_sdl_CondWaitTimeout(SDL_Condition *c, SDL_Mutex *m, Sint32 ms)
{ return SDL_WaitConditionTimeout(c, m, ms) ? 0 : SDL_MUTEX_TIMEDOUT; }
#define SDL_CondWaitTimeout(c, m, ms) hitz_sdl_CondWaitTimeout((c), (m), (ms))
#define SDL_mutex SDL_Mutex
#define SDL_AtomicSet(a, v) SDL_SetAtomicInt((a), (v))
#define SDL_AtomicGet(a)    SDL_GetAtomicInt(a)
/* SDL2 set the priority of the calling thread through a thread handle. */
static inline int hitz_sdl_SetThreadPriority(SDL_ThreadPriority p) { return SDL_SetCurrentThreadPriority(p) ? 0 : -1; }
#define SDL_SetThreadPriority(p) hitz_sdl_SetThreadPriority(p)


/* SDL3 names the letter keys in capitals (same values as SDL2's lowercase ones). */
#define SDLK_a SDLK_A
#define SDLK_b SDLK_B
#define SDLK_c SDLK_C
#define SDLK_d SDLK_D
#define SDLK_e SDLK_E
#define SDLK_f SDLK_F
#define SDLK_g SDLK_G
#define SDLK_h SDLK_H
#define SDLK_i SDLK_I
#define SDLK_j SDLK_J
#define SDLK_k SDLK_K
#define SDLK_l SDLK_L
#define SDLK_m SDLK_M
#define SDLK_n SDLK_N
#define SDLK_o SDLK_O
#define SDLK_p SDLK_P
#define SDLK_q SDLK_Q
#define SDLK_r SDLK_R
#define SDLK_s SDLK_S
#define SDLK_t SDLK_T
#define SDLK_u SDLK_U
#define SDLK_v SDLK_V
#define SDLK_w SDLK_W
#define SDLK_x SDLK_X
#define SDLK_y SDLK_Y
#define SDLK_z SDLK_Z

/* ---- surfaces ---- */
#define SDL_PIXELFORMAT_RGB888 SDL_PIXELFORMAT_XRGB8888
static inline SDL_Surface *hitz_sdl_CreateRGBSurfaceFrom(void *px, int w, int h, int depth, int pitch,
                                                          Uint32 rm, Uint32 gm, Uint32 bm, Uint32 am)
{ return SDL_CreateSurfaceFrom(w, h, SDL_GetPixelFormatForMasks(depth, rm, gm, bm, am), px, pitch); }
#define SDL_CreateRGBSurfaceFrom(px, w, h, d, p, r, g, b, a) hitz_sdl_CreateRGBSurfaceFrom((px), (w), (h), (d), (p), (r), (g), (b), (a))
static inline SDL_Surface *hitz_sdl_CreateRGBSurfaceWithFormat(Uint32 flags, int w, int h, int depth, SDL_PixelFormat fmt)
{ (void)flags; (void)depth; return SDL_CreateSurface(w, h, fmt); }
#define SDL_CreateRGBSurfaceWithFormat(f, w, h, d, fmt) hitz_sdl_CreateRGBSurfaceWithFormat((f), (w), (h), (d), (fmt))
#define SDL_FreeSurface SDL_DestroySurface
static inline SDL_Surface *hitz_sdl_ConvertSurfaceFormat(SDL_Surface *s, SDL_PixelFormat fmt, Uint32 flags)
{ (void)flags; return SDL_ConvertSurface(s, fmt); }
#define SDL_ConvertSurfaceFormat(s, f, fl) hitz_sdl_ConvertSurfaceFormat((s), (f), (fl))

/* ---- events: the key symbol moved from key.keysym.sym to key.key ---- */
#define HITZ_KEYSYM(e) ((e).key.key)


/* ---- game controller names (SDL2 SDL_CONTROLLER_* are SDL3 SDL_GAMEPAD_*) ---- */
#define SDL_CONTROLLER_BUTTON_A             SDL_GAMEPAD_BUTTON_SOUTH
#define SDL_CONTROLLER_BUTTON_B             SDL_GAMEPAD_BUTTON_EAST
#define SDL_CONTROLLER_BUTTON_X             SDL_GAMEPAD_BUTTON_WEST
#define SDL_CONTROLLER_BUTTON_Y             SDL_GAMEPAD_BUTTON_NORTH
#define SDL_CONTROLLER_BUTTON_BACK          SDL_GAMEPAD_BUTTON_BACK
#define SDL_CONTROLLER_BUTTON_START         SDL_GAMEPAD_BUTTON_START
#define SDL_CONTROLLER_BUTTON_LEFTSTICK     SDL_GAMEPAD_BUTTON_LEFT_STICK
#define SDL_CONTROLLER_BUTTON_RIGHTSTICK    SDL_GAMEPAD_BUTTON_RIGHT_STICK
#define SDL_CONTROLLER_BUTTON_LEFTSHOULDER  SDL_GAMEPAD_BUTTON_LEFT_SHOULDER
#define SDL_CONTROLLER_BUTTON_RIGHTSHOULDER SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER
#define SDL_CONTROLLER_BUTTON_DPAD_UP       SDL_GAMEPAD_BUTTON_DPAD_UP
#define SDL_CONTROLLER_BUTTON_DPAD_DOWN     SDL_GAMEPAD_BUTTON_DPAD_DOWN
#define SDL_CONTROLLER_BUTTON_DPAD_LEFT     SDL_GAMEPAD_BUTTON_DPAD_LEFT
#define SDL_CONTROLLER_BUTTON_DPAD_RIGHT    SDL_GAMEPAD_BUTTON_DPAD_RIGHT
#define SDL_CONTROLLER_AXIS_LEFTX           SDL_GAMEPAD_AXIS_LEFTX
#define SDL_CONTROLLER_AXIS_LEFTY           SDL_GAMEPAD_AXIS_LEFTY
#define SDL_CONTROLLER_AXIS_RIGHTX          SDL_GAMEPAD_AXIS_RIGHTX
#define SDL_CONTROLLER_AXIS_RIGHTY          SDL_GAMEPAD_AXIS_RIGHTY
#define SDL_CONTROLLER_AXIS_TRIGGERLEFT     SDL_GAMEPAD_AXIS_LEFT_TRIGGER
#define SDL_CONTROLLER_AXIS_TRIGGERRIGHT    SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
#define HITZ_PADBUTTON(e) ((e).gbutton.button)

#endif /* HITZ_SDL2_ON_SDL3_H */
