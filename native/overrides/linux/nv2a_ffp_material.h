#ifndef HITZ_NV2A_FFP_MATERIAL_H
#define HITZ_NV2A_FFP_MATERIAL_H
#include <stdint.h>

/* NV097 material-source selection. When ambient or emission comes from a
 * vertex color, MATERIAL_EMISSION supplies its multiplier; SCENE_AMBIENT
 * alone is only the complete constant-material case. Cross-checked against
 * https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/glsl/vsh-ff.c
 * (independent scalar implementation of the register semantics). */
static void hitz_ffp_material_base(float out[4], uint32_t sources,
                                 const float scene[3], const float emission[3],
                                 const float diffuse[4], const float specular[4],
                                 float material_alpha)
{
    unsigned e = sources & 3u, a = (sources >> 2) & 3u, d = (sources >> 4) & 3u;
    const float *ec = e == 1 ? diffuse : specular;
    const float *ac = a == 1 ? diffuse : specular;
    unsigned k;
    for (k = 0; k < 3; k++) {
        if (!e || !a) {
            out[k] = scene[k];
            if (e || a) out[k] += (e ? ec[k] : ac[k]) * emission[k];
        } else {
            out[k] = ec[k] + ac[k] * emission[k];
        }
    }
    out[3] = d == 1 ? diffuse[3] : d == 2 ? specular[3] : material_alpha;
}
#endif
