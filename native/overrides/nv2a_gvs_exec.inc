/* nv2a_gvs_exec.inc -- executor side of the GPU vertex-program path
 * (RECOMP_D3D11_GPUVS=1). #included by nv2a_pb_exec.c just before
 * raster_batch_program(). A program batch is packed into one raw block
 * (layout: nv2a_pbx.h) and the D3D11 back end runs the title's vertex program
 * on the GPU from it. Anything this path cannot take returns 0 and the
 * unchanged CPU path runs. RECOMP_GPUVS_VERIFY=1 also reads the GPU's outputs
 * back for sampled draws and compares them with the CPU interpreter. */

#define GVS_BLOCK_MAX (6u << 20)
typedef struct { uint64_t lo, hi; uint32_t attr; } GvsSeg;
enum { GF_PRIM, GF_PROG, GF_ATTR, GF_BIG, GF_SPARSE, GF_BACKEND, GF_ZOFF, GF_STATE, GF_N };
static const char *const s_gf_name[GF_N] = { "prim", "prog", "attr", "big", "sparse", "backend", "zoff", "state" };

static Nv2aGpuVs s_gvs;
static uint8_t *s_gvs_blk;
static float *s_gvs_vout;
static unsigned long s_gvs_draws, s_gvs_verts, s_gvs_fb[GF_N], s_gvs_fb_verts;
static unsigned long s_gvs_vdraws, s_gvs_vverts, s_gvs_vbad_draws, s_gvs_vbad_comp;
static double s_gvs_vmax_err;

static void gvs_stats(void)
{
    static unsigned long calls;
    char fb[160];
    int i, o = 0;

    if ((calls++ % 3000) != 1)
        return;
    for (i = 0; i < GF_N; i++)
        o += snprintf(fb + o, sizeof fb - (size_t)o, "%s=%lu ", s_gf_name[i], s_gvs_fb[i]);
    fprintf(stderr, "[GPUVS] draws=%lu verts=%lu | cpu-fallback verts=%lu | fallback reasons: %s\n",
            s_gvs_draws, s_gvs_verts, s_gvs_fb_verts, fb);
    if (s_gvs_vdraws)
        fprintf(stderr, "[GPUVS-VERIFY] draws=%lu verts=%lu mismatching draws=%lu components=%lu max err %.3g\n",
                s_gvs_vdraws, s_gvs_vverts, s_gvs_vbad_draws, s_gvs_vbad_comp, s_gvs_vmax_err);
}

static int gvs_fail(int why, uint32_t n)
{
    s_gvs_fb[why]++;
    s_gvs_fb_verts += n;
    gvs_stats();
    return 0;
}

/* Compare the GPU's outputs (verify_out) with the interpreter on the same vertices. */
static void gvs_verify_compare(const uint32_t *idx, uint32_t nidx)
{
    uint32_t k, c, n = nidx > 4096 ? 4096 : nidx, bad = 0;
    int shown = 0;
    static int printed;

    s_gvs_vdraws++;
    for (k = 0; k < n; k++) {
        Nv2aVshOutput ref;
        float want[29];
        const float *got = s_gvs_vout + (size_t)k * 29;

        memset(&ref, 0, sizeof ref);
        if (!transform_vertex(idx[k], &ref))
            continue;
        memcpy(want, ref.pos, 16);
        memcpy(want + 4, ref.d0, 16);
        memcpy(want + 8, ref.d1, 16);
        memcpy(want + 12, ref.tex, 64);
        want[28] = fog_factor(ref.fog[0]);
        s_gvs_vverts++;
        for (c = 0; c < 29; c++) {
            float w = want[c], g = got[c];
            double err, tol;

            if (w != w && g != g) continue;
            if (isinf(w) && w == g) continue;
            err = fabs((double)w - (double)g);
            tol = 1e-3 * fmax(1.0, fabs((double)w)) + 2e-3;
            if (!(err <= tol)) {
                bad++;
                s_gvs_vbad_comp++;
                if (err == err && err < 1e30 && err > s_gvs_vmax_err) s_gvs_vmax_err = err;
                if (shown < 3 && printed < 24) {
                    shown++; printed++;
                    fprintf(stderr, "[GPUVS-VERIFY] hash %08X vertex %u (idx %u) comp %u: cpu %g gpu %g\n",
                            s_gvs.hash, k, idx[k], c, w, g);
                }
            }
        }
    }
    if (bad)
        s_gvs_vbad_draws++;
}

static int gvs_try(void)
{
    static int on = -1, verify = -1;
    uint32_t n = s_gpu.idx_count, nidx, i, a, minidx = 0xFFFFFFFFu, maxidx = 0, pos, ni;
    const uint8_t *mem;
    uint32_t *ib;
    GvsSeg seg[NV2A_VSH_INPUTS], mseg[NV2A_VSH_INPUTS];
    uint32_t nseg = 0, nm = 0, segpos[NV2A_VSH_INPUTS];
    uint32_t attr_seg[NV2A_VSH_INPUTS];
    uint64_t copied = 0;
    float *imm;
    uint32_t *desc;
    const float *c59;

    if (on < 0) {
        const char *e = getenv("RECOMP_D3D11_GPUVS");
        on = e && e[0] == '1';
        verify = on && getenv("RECOMP_GPUVS_VERIFY") != NULL;
    }
    if (!on || n < 3 || !batch_uses_program() || !pbx_enabled() || !backend_draws() || !s_gpu.rc_seen)
        return 0;
    if (s_bcount)
        return gvs_fail(GF_STATE, n);

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:      nidx = n - n % 3; break;
    case NV_PRIM_TRIANGLE_STRIP: nidx = 3 * (n - 2); break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:        nidx = 3 * (n - 2); break;
    case NV_PRIM_QUADS:          nidx = 6 * (n / 4); break;
    case NV_PRIM_QUAD_STRIP:     nidx = 1; break;       /* sized below */
    default:                     nidx = 0; break;
    }
    if (s_gpu.prim == NV_PRIM_QUAD_STRIP) {   /* i = 0, 2, ... while i + 3 < n */
        uint32_t q = 0;
        for (i = 0; i + 3 < n; i += 2) q++;
        nidx = 6 * q;
    }
    if (!nidx)
        return gvs_fail(GF_PRIM, n);

    memset(&s_gvs, 0, sizeof s_gvs);
    if (!nv2a_vsh_export(&s_gvs))
        return gvs_fail(GF_PROG, n);
    c59 = nv2a_vsh_constant(59);
    if (c59[2] != 0.0f)
        return gvs_fail(GF_ZOFF, n);

    if (!s_gvs_blk && !(s_gvs_blk = (uint8_t *)malloc(GVS_BLOCK_MAX)))
        return gvs_fail(GF_BIG, n);
    if (verify && !s_gvs_vout)
        s_gvs_vout = (float *)malloc(4096 * 29 * sizeof(float));
    {
        static unsigned long vcount;
        s_gvs.verify = (verify && s_gvs_vout && (vcount < 60 || (vcount % 250) == 0)) ? 1 : 0;
        s_gvs.verify_out = s_gvs.verify ? s_gvs_vout : NULL;
        vcount++;
    }
    if (!nv2a_gpuvs_probe(&s_gvs))
        return gvs_fail(GF_BACKEND, n);

    for (i = 0; i < n; i++) {
        if (s_gpu.idx[i] < minidx) minidx = s_gpu.idx[i];
        if (s_gpu.idx[i] > maxidx) maxidx = s_gpu.idx[i];
    }
    mem = (const uint8_t *)xbox_GetMemoryOffset();
    imm = (float *)(s_gvs_blk + GVS_OFF_IMM);
    desc = (uint32_t *)(s_gvs_blk + GVS_OFF_ATTR);
    memset(desc, 0, 16 * 16);
    for (a = 0; a < NV2A_VSH_INPUTS; a++) {
        imm[a * 4 + 0] = imm[a * 4 + 1] = imm[a * 4 + 2] = 0.0f;
        imm[a * 4 + 3] = 1.0f;
        desc[a * 4 + 3] = 1;                     /* constant unless a fetch below says otherwise */
        attr_seg[a] = 0xFFFFFFFFu;
    }
    for (a = 0; a < NV2A_VSH_INPUTS; a++) {
        const VertexAttr *at = &s_gpu.attr[a];
        uint32_t elem;
        uint64_t lo, hi;

        if (!((s_gvs.input_mask >> a) & 1u))
            continue;
        if (!at->size && a < NV_VERTEX_ATTRS) {
            memcpy(&imm[a * 4], s_gpu.imm_attr[a], 16);
            continue;
        }
        if (!at->size || !at->stride)
            continue;
        if (!(at->type <= 2 || at->type == 4 || at->type == 5 || at->type == 6))
            continue;                            /* fetch_attr: unsupported type reads as absent */
        elem = attr_elem_bytes(at);
        lo = (uint64_t)at->offset + (uint64_t)minidx * at->stride;
        hi = (uint64_t)at->offset + (uint64_t)maxidx * at->stride + elem;
        if (s_gpu.inline_active) {
            if (hi > (uint64_t)s_gpu.inline_count * 4)
                return gvs_fail(GF_ATTR, n);
        } else {
            if (!at->offset || !guest_span_ok(lo, elem) || !guest_span_ok(hi - elem, elem))
                return gvs_fail(GF_ATTR, n);
        }
        seg[nseg].lo = lo; seg[nseg].hi = hi; seg[nseg].attr = a; nseg++;
    }
    /* sort by lo, merge near ones */
    for (i = 1; i < nseg; i++) {
        uint32_t j = i;
        GvsSeg t = seg[i];
        while (j && seg[j - 1].lo > t.lo) { seg[j] = seg[j - 1]; j--; }
        seg[j] = t;
    }
    for (i = 0; i < nseg; i++) {
        if (nm && seg[i].lo <= mseg[nm - 1].hi + 64) {
            if (seg[i].hi > mseg[nm - 1].hi) mseg[nm - 1].hi = seg[i].hi;
        } else {
            mseg[nm].lo = seg[i].lo; mseg[nm].hi = seg[i].hi; nm++;
        }
        attr_seg[seg[i].attr] = nm - 1;
    }
    for (i = 0; i < nm; i++)
        copied += mseg[i].hi - mseg[i].lo;
    if (copied > 32768u + 16u * (uint64_t)n)
        return gvs_fail(GF_SPARSE, n);

    ni = (nidx * 4u + 15u) & ~15u;
    pos = GVS_OFF_IDX + ni;
    for (i = 0; i < nm; i++) {
        uint64_t len = mseg[i].hi - mseg[i].lo;
        uint32_t p = (pos + 15u) & ~15u;
        p += (uint32_t)(mseg[i].lo & 15u);
        if ((uint64_t)p + len > GVS_BLOCK_MAX)
            return gvs_fail(GF_BIG, n);
        memcpy(s_gvs_blk + p, (s_gpu.inline_active ? (const uint8_t *)s_gpu.inline_buf : mem) + mseg[i].lo, (size_t)len);
        segpos[i] = p;
        pos = p + (uint32_t)len;
    }
    for (a = 0; a < NV2A_VSH_INPUTS; a++) {
        const VertexAttr *at = &s_gpu.attr[a];
        uint32_t m = attr_seg[a];
        if (m == 0xFFFFFFFFu)
            continue;
        desc[a * 4 + 0] = (uint32_t)((uint64_t)segpos[m] + (uint64_t)at->offset - mseg[m].lo);
        desc[a * 4 + 1] = at->stride;
        desc[a * 4 + 2] = at->type | (at->size < 4 ? at->size : 4u) << 8;
        desc[a * 4 + 3] = 0;
    }
    for (i = 0; i < NV2A_VSH_CONSTANTS; i++)
        memcpy(s_gvs_blk + GVS_OFF_CONST + i * 16u, nv2a_vsh_constant(i), 16);

    /* triangle-list index array, in the CPU path's assembly order */
    ib = (uint32_t *)(s_gvs_blk + GVS_OFF_IDX);
    {
        uint32_t o = 0;
        switch (s_gpu.prim) {
        case NV_PRIM_TRIANGLES:
            for (i = 0; i + 2 < n; i += 3) { ib[o++] = s_gpu.idx[i]; ib[o++] = s_gpu.idx[i+1]; ib[o++] = s_gpu.idx[i+2]; }
            break;
        case NV_PRIM_TRIANGLE_STRIP:
            for (i = 0; i + 2 < n; i++) { ib[o++] = s_gpu.idx[i]; ib[o++] = s_gpu.idx[i+1]; ib[o++] = s_gpu.idx[i+2]; }
            break;
        case NV_PRIM_TRIANGLE_FAN:
        case NV_PRIM_POLYGON:
            for (i = 1; i + 1 < n; i++) { ib[o++] = s_gpu.idx[0]; ib[o++] = s_gpu.idx[i]; ib[o++] = s_gpu.idx[i+1]; }
            break;
        case NV_PRIM_QUADS:
            for (i = 0; i + 3 < n; i += 4) {
                ib[o++] = s_gpu.idx[i]; ib[o++] = s_gpu.idx[i+1]; ib[o++] = s_gpu.idx[i+2];
                ib[o++] = s_gpu.idx[i]; ib[o++] = s_gpu.idx[i+2]; ib[o++] = s_gpu.idx[i+3];
            }
            break;
        case NV_PRIM_QUAD_STRIP:
            for (i = 0; i + 3 < n; i += 2) {
                ib[o++] = s_gpu.idx[i]; ib[o++] = s_gpu.idx[i+1]; ib[o++] = s_gpu.idx[i+3];
                ib[o++] = s_gpu.idx[i]; ib[o++] = s_gpu.idx[i+3]; ib[o++] = s_gpu.idx[i+2];
            }
            break;
        default: break;
        }
    }
    s_gvs.block = s_gvs_blk;
    s_gvs.block_bytes = pos;
    s_gvs.nidx = nidx;
    s_gvs.aa_sx = s_gpu.aa_sx;
    s_gvs.aa_sy = s_gpu.aa_sy;
    s_gvs.fog_mode = s_gpu.fog_enable ? (s_gpu.fog_mode ? s_gpu.fog_mode : 1u) : 0u;
    s_gvs.fog_px = s_gpu.fog_param[0];
    s_gvs.fog_py = s_gpu.fog_param[1];

    /* the CPU path's per-batch bookkeeping */
    xf_set_viewport();
    s_xf_no_tex = 0;                             /* batch_uses_program() is true */
    s_gpu.batches_program++;
    s_gpu.verts_program += n;
    if (s_gpu.blend_enable) {
        uint32_t pair = s_gpu.blend_sfactor << 16 | (s_gpu.blend_dfactor & 0xFFFF);
        int j;
        for (j = 0; j < s_gpu.blend_npairs && s_gpu.blend_pairs[j] != pair; j++)
            ;
        if (j == s_gpu.blend_npairs && j < 16)
            s_gpu.blend_pairs[s_gpu.blend_npairs++] = pair;
    }
    for (i = 0; i < NV2A_VSH_INPUTS; i++) {
        const VertexAttr *at = &s_gpu.attr[i];
        uint32_t f = at->type | (at->size << 4) | (i << 8), j;
        if (!at->size || !at->stride)
            continue;
        for (j = 0; j < (uint32_t)s_gpu.xf_nfmt && s_gpu.xf_fmt[j] != f; j++)
            ;
        if (j == (uint32_t)s_gpu.xf_nfmt && s_gpu.xf_nfmt < 16)
            s_gpu.xf_fmt[s_gpu.xf_nfmt++] = f;
    }
    if (s_gpu.texs[0].valid)
        note_texture_use();
    {   /* RECOMP_GPUVS_PLOG=<tex0 offset hex>[,<fromflip>,<toflip>]: diagnostic. For GPU-path draws with that stage-0
         * texture, print the interpreter's outputs (screen pos, w, diffuse, t0) of up to 12 vertices,
         * the clip range, ZMIN_MAX_CONTROL and texture CONTROL0/ADDRESS. */
        static int pl = -1; static uint32_t poff, pf0 = 0, pf1 = 0xFFFFFFFFu; static unsigned long pn;
        if (pl < 0) { const char *e = getenv("RECOMP_GPUVS_PLOG"); pl = (e && sscanf(e, "%x,%u,%u", &poff, &pf0, &pf1) >= 1) ? 1 : 0; }
        if (pl && s_gpu.texs[0].valid && ((s_gpu.texs[0].offset ^ poff) & 0x0FFFFFFFu) == 0 && pn < 20000
            && (uint32_t)s_gpu.flips >= pf0 && (uint32_t)s_gpu.flips < pf1) {
            uint32_t k, m = nidx < 12 ? nidx : 12;
            pn++;
            fprintf(stderr, "[PLOG] flip %u n %u clip %g..%g zmm %08X ctl0 %08X addr %08X fmt %08X filt %08X cull %u/%X\n",
                    (unsigned)s_gpu.flips, nidx, reg_f(0x0394), reg_f(0x0398), s_reg[0x1D78 / 4], s_reg[0x1B0C / 4], s_reg[0x1B08 / 4],
                    s_reg[0x1B04 / 4], s_reg[0x1B14 / 4], s_reg[0x0308 / 4], s_reg[0x039C / 4]);
            for (k = 0; k < m; k++) {
                Nv2aVshOutput ref;
                memset(&ref, 0, sizeof ref);
                if (!transform_vertex(ib[k], &ref)) continue;
                fprintf(stderr, "[PLOG]   v%u pos %.1f %.1f %.6g w %.6g d0 %.3f %.3f %.3f %.3f t0 %.3f %.3f %.3f %.3f\n", k,
                        ref.pos[0], ref.pos[1], ref.pos[2], ref.pos[3], ref.d0[0], ref.d0[1], ref.d0[2], ref.d0[3],
                        ref.tex[0][0], ref.tex[0][1], ref.tex[0][2], ref.tex[0][3]);
            }
        }
    }
    s_bkind = 1;
    s_pbx_gvs = &s_gvs;
    backend_flush_gvs(nidx);
    s_pbx_gvs = NULL;
    if (s_gvs.verify)
        gvs_verify_compare((const uint32_t *)(s_gvs_blk + GVS_OFF_IDX), nidx);
    s_gvs_draws++;
    s_gvs_verts += n;
    gvs_stats();
    return 1;
}