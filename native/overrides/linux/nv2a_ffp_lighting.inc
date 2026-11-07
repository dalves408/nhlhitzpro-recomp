/* Shared by the executor and its lighting regression harness. The including
 * translation unit supplies NV2A register/attribute access and material selection. */
static int lit_color(uint32_t index, float out[4])
{
    const float *mv = (const float *)&s_reg[0x0480 / 4];
    float pos[4], nrm[4], vc[4] = {1,1,1,1}, sc[4] = {0,0,0,1}, e[3], n[3], len;
    uint32_t mask = s_reg[0x03BC / 4], colmat = s_reg[0x0298 / 4];
    int i, have_vc;

    if (!s_reg[0x0314 / 4])
        return 0;
    have_vc = fetch_attr(color_attr(), index, vc);
    fetch_attr(&s_gpu.attr[4], index, sc);

    {
        float scene[3] = {reg_f(0x0A10), reg_f(0x0A14), reg_f(0x0A18)};
        float emission[3] = {reg_f(0x03A8), reg_f(0x03AC), reg_f(0x03B0)};
        hitz_ffp_material_base(out, colmat, scene, emission, vc, sc, reg_f(0x03B4));
    }
    /* Prelit arena/crowd geometry enables lighting with no active lights.
     * The eye position and normalized normal cannot contribute in that case. */
    if (!(mask & 0xFFFFu)) goto clamp_color;

    if (!fetch_attr(&s_gpu.attr[0], index, pos)) return 0;
    /* A disabled normal array selects the current normal; it does not turn
     * lighting off. In particular the goal mesh has position/UV vertices and
     * ambient-only lighting, which needs no normal at all. */
    if (!fetch_attr(&s_gpu.attr[2], index, nrm))
        memcpy(nrm, s_gpu.imm_attr[2], sizeof nrm);

    for (i = 0; i < 3; i++) {
        e[i] = mv[4 * i] * pos[0] + mv[4 * i + 1] * pos[1]
             + mv[4 * i + 2] * pos[2] + mv[4 * i + 3];
        n[i] = mv[4 * i] * nrm[0] + mv[4 * i + 1] * nrm[1]
             + mv[4 * i + 2] * nrm[2];
    }
    len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }
    /* Optional (RECOMP_FFP_FLIP_NORMALS): light the side the viewer sees.
     * Some meshes carry normals that face away from the camera (X-Men
     * Legends' streets and sidewalks: authentic file data, drawn with culling
     * off), and lighting them as stored leaves only the light's ambient term
     * -- black ground. Flipping a normal that points away from the eye is
     * two-sided lighting with the back material equal to the front one.
     * This is NOT hardware behaviour: it applies even when the title left
     * two-sided lighting off (NV097_SET_TWO_SIDE_LIGHT_EN, method 0x17C4,
     * which X-Men Legends writes as 0), and how the hardware makes such a mesh
     * look right is not established. So it is a compatibility option a game
     * project turns on for meshes it has seen need it, and off otherwise, so
     * it cannot change how any other title is lit.
     * ponytail: per vertex, not per face; exact for flat ground, a vertex on
     * a silhouette may pick the other side. */
    if (ffp_flip_normals() && n[0] * e[0] + n[1] * e[1] + n[2] * e[2] > 0.0f) {
        n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
    }

    for (i = 0; i < NV_LIGHTS; i++) {
        uint32_t kind = (mask >> (2 * i)) & 3, b = 0x1000 + (uint32_t)i * 0x80;
        float l[3], att = 1.0f, ndl, amb[3], dif[3];
        int k;

        if (!kind)
            continue;
        if (kind == 1) {                                    /* infinite */
            l[0] = reg_f(b + 0x34); l[1] = reg_f(b + 0x38); l[2] = reg_f(b + 0x3C);
        } else {                                            /* local / spot */
            float d;
            l[0] = reg_f(b + 0x5C) - e[0];
            l[1] = reg_f(b + 0x60) - e[1];
            l[2] = reg_f(b + 0x64) - e[2];
            d = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
            if (d > reg_f(b + 0x24) && reg_f(b + 0x24) > 0.0f)
                continue;                                   /* out of range */
            if (d > 1e-8f) { l[0] /= d; l[1] /= d; l[2] /= d; }
            att = reg_f(b + 0x68) + reg_f(b + 0x6C) * d + reg_f(b + 0x70) * d * d;
            att = att > 1e-8f ? 1.0f / att : 1.0f;
        }
        ndl = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
        if (ndl < 0.0f) ndl = 0.0f;
        for (k = 0; k < 3; k++) {
            amb[k] = reg_f(b + 0x00 + 4 * k);
            dif[k] = reg_f(b + 0x0C + 4 * k);
            /* COLOR_MATERIAL: 1 in the ambient (bits 2-3) or diffuse (bits
             * 4-5) field takes that material colour from the vertex. */
            if (have_vc && ((colmat >> 2) & 3) == 1) amb[k] *= vc[k];
            if (have_vc && ((colmat >> 4) & 3) == 1) dif[k] *= vc[k];
            if (((colmat >> 2) & 3) == 2) amb[k] *= sc[k];
            if (((colmat >> 4) & 3) == 2) dif[k] *= sc[k];
            out[k] += att * (amb[k] + dif[k] * ndl);
        }
    }
clamp_color:
    for (i = 0; i < 4; i++)
        out[i] = out[i] < 0.0f ? 0.0f : out[i] > 1.0f ? 1.0f : out[i];
    return 1;
}
