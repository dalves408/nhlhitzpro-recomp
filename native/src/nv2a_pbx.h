/*
 * Project extension of the NV2A back-end interface (project code, not toolkit).
 *
 * nv2a_backend.h hands a back end stage 0 only: one texture, "texel x
 * diffuse". The NV2A really runs every pixel through its register combiners,
 * which mix the vertex colours, up to four textures, constants and fog. For a
 * batch drawn by a vertex program the executor (overrides/nv2a_pb_exec.c) also
 * keeps that state, and while it calls Backend.draw() it lets the back end read
 * it through nv2a_pbx_current(). Valid only inside draw(); NULL for batches
 * without register-combiner state (then use stage 0 as before).
 *
 * Vertex i of Nv2aBatch.vertices has the extras ext[i].
 */
#ifndef NHL_NV2A_PBX_H
#define NHL_NV2A_PBX_H

#include <stdint.h>
#include <stddef.h>
#include "nv2a_backend.h"
#include "nv2a_combiner.h"

typedef struct {
    float d1[4];        /* specular colour r,g,b,a, 0..1 */
    float tc[4][4];     /* texture coordinates of stages 0-3, as the program wrote them */
    float fog;          /* fog factor (the fixed-function fog unit's output) */
} Nv2aVertexExt;

/* ---- GPU vertex-program path (RECOMP_D3D11_GPUVS=1) ----
 * The executor does not run the batch's vertex program; it hands the back end
 * the program and a block of raw data, and a generated HLSL vertex shader does
 * the transform. Block layout (bytes, relative to the block start):
 *   0     192 float4 vertex-program constants
 *   3072  16 x uint4 attribute descriptors: x = address of vertex 0's element
 *         (block-relative, wraps), y = stride, z = type | size << 8,
 *         w = 1 when the attribute is a constant (see imm) or absent
 *   3328  16 x float4 immediate value (the value of a constant attribute, or the
 *         default (0,0,0,1) of a fetched one)
 *   3584  triangle-list vertex indices (uint32 each, `nidx` of them), padded to 16
 *   then  the raw attribute bytes the descriptors point into. */
#define NV2A_VSH_SLOTS_MAX 136
#define GVS_OFF_CONST 0u
#define GVS_OFF_ATTR  3072u
#define GVS_OFF_IMM   3328u
#define GVS_OFF_IDX   3584u

typedef struct {
    uint32_t prog[NV2A_VSH_SLOTS_MAX][4];   /* microcode, start slot .. FINAL */
    uint32_t nslots, hash, input_mask;
    const uint8_t *block;                   /* valid inside draw() only */
    uint32_t block_bytes;
    uint32_t nidx;                          /* indices = vertices drawn */
    float    aa_sx, aa_sy;
    uint32_t fog_mode;                      /* 0: fog off, else the NV097 fog mode word */
    float    fog_px, fog_py;
    uint32_t verify;                        /* RECOMP_GPUVS_VERIFY: read the outputs back */
    float   *verify_out;                    /* nidx x 28 floats, or NULL */
} Nv2aGpuVs;

/* 1 if the back end can draw program e on the GPU right now (compiles its shader). */
int nv2a_gpuvs_probe(const Nv2aGpuVs *e);

/* nv2a_vsh_interp.c (project): the active program, and its HLSL body. */
int nv2a_vsh_export(Nv2aGpuVs *e);
int nv2a_vsh_gen_hlsl(const Nv2aGpuVs *e, char *out, size_t cap);
typedef struct {
    const Nv2aVertexExt *ext;
    const Nv2aCombiner  *rc;
    float                fog_color[4];   /* r,g,b,a */
    Nv2aTexture          tex[4];         /* as the title bound them */
    uint32_t             tex_valid[4];   /* 0: unusable, reads white */
    uint32_t             tex_sized[4];   /* 1: coordinates are normalised (0..1), 0: texels */
    uint32_t             tex_cube[4];
    uint32_t             clip_plane_mode; /* SET_SHADER_CLIP_PLANE_MODE: stage k, coord j -> bit 4k+j (1: kill if >= 0, 0: kill if < 0) */
    const Nv2aGpuVs     *gpu;            /* non-NULL: GPU-vertex-shader batch (vertices == NULL) */
} Nv2aBatchExt;

const Nv2aBatchExt *nv2a_pbx_current(void);

#endif