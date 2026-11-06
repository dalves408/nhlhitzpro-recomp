#ifndef HITZ_GL_TEXTURE_PACK_H
#define HITZ_GL_TEXTURE_PACK_H
#include <epoxy/gl.h>
#include <stdint.h>
typedef struct {
    uint64_t loads,load_us,create_us,hits,resident_bytes,pending_bytes,evictions,failures;
    unsigned images,queued;
} HitzGlPackStats;
void hitz_gl_pack_init(unsigned scale,void (*invalidate_bindings)(GLuint));
int hitz_gl_pack_active(unsigned scale);
int hitz_gl_pack_available(unsigned scale);
void hitz_gl_pack_set_live(int on);
int hitz_gl_pack_find(uint64_t hash);
GLuint hitz_gl_pack_get(int index,uint64_t frame,unsigned *levels);
void hitz_gl_pack_pump(uint64_t frame);
void hitz_gl_pack_stats(HitzGlPackStats *stats);
void hitz_gl_pack_shutdown(void);
#endif
