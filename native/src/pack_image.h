#ifndef HITZ_PACK_IMAGE_H
#define HITZ_PACK_IMAGE_H
#include <stddef.h>
#include <stdint.h>

#define HITZ_PACK_MAX_MIPS 15
typedef enum {
    HITZ_PACK_INVALID, HITZ_PACK_BC1, HITZ_PACK_BC2, HITZ_PACK_BC3,
    HITZ_PACK_BC7, HITZ_PACK_BGRA8, HITZ_PACK_RGBA8
} HitzPackFormat;
typedef struct {
    const unsigned char *data;
    size_t bytes;
    uint32_t width,height;
} HitzPackLevel;
typedef struct {
    HitzPackFormat format;
    uint32_t width,height,mips;
    int opaque,generate_mips;
    size_t payload_bytes,memory_bytes;
    void *allocation;
    HitzPackLevel level[HITZ_PACK_MAX_MIPS];
} HitzPackImage;

/* Pure header/payload validation. Output borrows bytes; allocation remains NULL. */
int hitz_pack_parse_dds(const void *bytes,size_t length,HitzPackImage *image);
/* Conservative retained-buffer estimate, or zero for invalid/unsupported input. */
size_t hitz_pack_image_estimate(const char *path);
int hitz_pack_image_read(const char *path,size_t memory_limit,HitzPackImage *image);
void hitz_pack_image_free(HitzPackImage *image);
size_t hitz_pack_level_bytes(HitzPackFormat format,uint32_t width,uint32_t height);
#endif
