/* SDL3 build only (-DHITZ_SDL3=ON): the toolkit's apu_core.c, unchanged, with
 * the audio backend named SDL3 in its start-up line ("[APU] Using SDL3 audio
 * backend"). The toolkit header fixes the name as "SDL2"; the header is
 * include-guarded, so including it first lets the name be replaced before
 * apu_core.c includes it again. HITZ_TOOLKIT_APU_CORE is the toolkit's path,
 * set in cmake/linux.cmake. */
#include "apu_xaudio2.h"
#undef XA2_BACKEND_NAME
#define XA2_BACKEND_NAME "SDL3"
#include HITZ_TOOLKIT_APU_CORE
