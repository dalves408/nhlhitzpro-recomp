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
static uint32_t s_gvs_vcount;
static unsigned long s_gvs_vskipped;
static unsigned long s_gvs_draws, s_gvs_verts, s_gvs_fb[GF_N], s_gvs_fb_verts;
static unsigned long s_gvs_vdraws, s_gvs_vverts, s_gvs_vbad_draws, s_gvs_vbad_comp;
static double s_gvs_vmax_err;
static unsigned long s_gffp_vdraws, s_gffp_vverts, s_gffp_vbad;
static uint64_t s_gvs_prepare_us,s_gvs_submit_us,s_gvs_guest_bytes;
static int gvs_timing_on(void)
{
    static int on=-1;
    if(on<0){const char *v=getenv("RECOMP_RENDER_TIMING");on=v && atoi(v)!=0;}
    return on;
}
static uint64_t gvs_clock_us(void)
{
    static LARGE_INTEGER frequency;LARGE_INTEGER now;
    if(!frequency.QuadPart)QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart*1000000ull/frequency.QuadPart);
}
void nv2a_pbx_gvs_work_stats(uint64_t out[3])
{out[0]=s_gvs_prepare_us;out[1]=s_gvs_submit_us;out[2]=s_gvs_guest_bytes;}

/* The FFP shader shares the raw attribute/index upload and the CPU verifier
 * with vertex programs. No guest storage survives the synchronous draw. */
static int gvs_ffp_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPU_FFP"); on = e && e[0] == '1';
        fprintf(stderr,"[GPUFFP] %s (unsupported batches retain CPU rendering)\n",on?"enabled":"disabled");
    }
    return on;
}

static int gvs_ffp_attr_present(const VertexAttr *a)
{
    return a->size && a->stride && (s_gpu.inline_active || a->offset) &&
           (a->type <= 2 || a->type == 4 || a->type == 5 || a->type == 6);
}

static void gvs_ffp_pack(void)
{
    float *c = (float *)(s_gvs_blk + GVS_OFF_CONST);
    uint32_t opts[4], slots[4];
    memset(c, 0, GVS_OFF_ATTR);
    memcpy(c, s_gpu.composite, 64);
    memcpy(c + 16, &s_reg[0x0480 / 4], 48);
    c[28] = reg_f(0x0A10); c[29] = reg_f(0x0A14); c[30] = reg_f(0x0A18); c[31] = reg_f(0x03B4);
    c[32] = reg_f(0x03A8); c[33] = reg_f(0x03AC); c[34] = reg_f(0x03B0);
    opts[0] = s_reg[0x03BC / 4]; opts[1] = s_reg[0x0298 / 4];
    opts[2] = s_reg[0x0314 / 4] != 0;
    opts[3] = ffp_flip_normals();
    memcpy(c + 36, opts, sizeof opts);
    memcpy(c + 40, &s_reg[0x1000 / 4], NV_LIGHTS * 0x80);
    memcpy(c + 74 * 4, nv2a_vsh_constant(59), 16);
    slots[0] = (uint32_t)(color_attr() - s_gpu.attr);
    slots[1] = (uint32_t)(texcoord_attr() - s_gpu.attr);
    slots[2] = gvs_ffp_attr_present(color_attr()); slots[3] = 0;
    memcpy(c + 75 * 4, slots, sizeof slots);
}

static void gvs_stats(void)
{
    static unsigned long calls;
    char fb[160];
    int i, o = 0;

    if (!progress_logs() && !s_gvs_vdraws) return;
    if ((calls++ % 3000) != 1)
        return;
    for (i = 0; i < GF_N; i++)
        o += snprintf(fb + o, sizeof fb - (size_t)o, "%s=%lu ", s_gf_name[i], s_gvs_fb[i]);
    fprintf(stderr, "[GPUVS] draws=%lu verts=%lu | cpu-fallback verts=%lu | fallback reasons: %s\n",
            s_gvs_draws, s_gvs_verts, s_gvs_fb_verts, fb);
    if (s_gvs_vdraws)
        fprintf(stderr, "[GPUVS-VERIFY] draws=%lu verts=%lu mismatching draws=%lu components=%lu max err %.3g\n",
                s_gvs_vdraws, s_gvs_vverts, s_gvs_vbad_draws, s_gvs_vbad_comp, s_gvs_vmax_err);
    if (s_gffp_vdraws)
        fprintf(stderr, "[GPUFFP-VERIFY] draws=%lu verts=%lu mismatching draws=%lu\n",
                s_gffp_vdraws, s_gffp_vverts, s_gffp_vbad);
    if (s_gvs_vskipped)
        fprintf(stderr, "[GPUVS-VERIFY] skipped readbacks=%lu (no GPU output; not compared)\n", s_gvs_vskipped);
}

static int gvs_fail(int why, uint32_t n)
{
    s_gvs_fb[why]++;
    s_gvs_fb_verts += n;
    gvs_stats();
    return 0;
}

/* Opt-in standalone arithmetic reproductions, not a readback every frame.
 * Store packed constants, CPU-decoded attributes and expected position for
 * the actual failing vertex; the hardware regression can replay this data. */
static void gvs_ffp_fixture(uint32_t index, const Nv2aVshOutput *ref)
{
    static unsigned count;
    const char *prefix = getenv("RECOMP_GPUFFP_FIXTURE");
    float data[92][4]; unsigned a; char path[512]; FILE *f;
    if (!prefix || count >= 8) return;
    memcpy(data, s_gvs_blk + GVS_OFF_CONST, 76*16);
    for (a=0;a<16;a++) {
        if (!fetch_attr(&s_gpu.attr[a],index,data[76+a]))
            memcpy(data[76+a],s_gpu.imm_attr[a],16);
    }
    snprintf(path,sizeof path,"%s_%u.bin",prefix,count++);
    f=fopen(path,"wb");
    if (f) { fwrite(data,sizeof data,1,f); fwrite(ref->pos,16,1,f); fclose(f);
        fprintf(stderr,"[GPUFFP-FIXTURE] %s index=%u\n",path,index); }
}

/* Compare the GPU's outputs (verify_out) with the interpreter on the same vertices. */
static void gvs_verify_compare(const uint32_t *idx, uint32_t nidx)
{
    uint32_t k, c, n = nidx > 4096 ? 4096 : nidx, bad = 0;
    int shown = 0;
    static int printed;

    s_gvs_vdraws++;
    if (s_gvs.fixed_function) s_gffp_vdraws++;
    s_vertex_snapshot = s_gvs.block;
    s_vertex_snapshot_bytes = s_gvs.block_bytes;
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
        if (s_gvs.fixed_function) s_gffp_vverts++;
        for (c = 0; c < 29; c++) {
            float w = want[c], g = got[c];
            double err, tol;

            if (w != w && g != g) continue;
            if (isinf(w) && w == g) continue;
            err = fabs((double)w - (double)g);
            tol = 1e-3 * fmax(1.0, fabs((double)w)) + 2e-3;
            if (!(err <= tol)) {
                if (s_gvs.fixed_function && !shown) gvs_ffp_fixture(idx[k], &ref);
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
    if (bad && s_gvs.fixed_function) s_gffp_vbad++;
    s_vertex_snapshot = NULL;
}

static int gvs_try_impl(void)
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
    int ffp = !batch_uses_program() && batch_is_ffp() && gvs_ffp_on();

    if (on < 0) {
        const char *e = getenv("RECOMP_D3D11_GPUVS");
        on = e && e[0] == '1';
        verify = on && getenv("RECOMP_GPUVS_VERIFY") != NULL;
    }
    if (!on || n < 3 || (!batch_uses_program() && !ffp) || !pbx_enabled() || !backend_draws() || !s_gpu.rc_seen)
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
    if (ffp) {
        if (!gvs_ffp_attr_present(&s_gpu.attr[0])) return gvs_fail(GF_ATTR, n);
        s_gvs.fixed_function = 1;
        s_gvs.hash = 0xFF010001u;
        s_gvs.input_mask = (1u << 0) | (1u << 2) | (1u << 4) |
            (1u << (color_attr() - s_gpu.attr)) | (1u << (texcoord_attr() - s_gpu.attr)) |
            (1u << 10) | (1u << 11) | (1u << 12);
    } else if (!nv2a_vsh_export(&s_gvs))
        return gvs_fail(GF_PROG, n);
    c59 = nv2a_vsh_constant(59);
    if (!ffp && c59[2] != 0.0f)
        return gvs_fail(GF_ZOFF, n);

    if (!s_gvs_blk && !(s_gvs_blk = (uint8_t *)malloc(GVS_BLOCK_MAX)))
        return gvs_fail(GF_BIG, n);
    if (verify && !s_gvs_vout)
        s_gvs_vout = (float *)malloc(4096 * 29 * sizeof(float));
    {
        static unsigned long vcount;
        s_gvs.verify = (verify && s_gvs_vout && (vcount < 60 || (vcount % 250) == 0)) ? 1 : 0;
        s_gvs.verify_out = s_gvs.verify ? s_gvs_vout : NULL;
        s_gvs.verify_count = s_gvs.verify ? &s_gvs_vcount : NULL;
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
        if (ffp && !gvs_ffp_attr_present(at)) {
            if (a == 2) memcpy(&imm[a * 4], s_gpu.imm_attr[a], 16);
            continue;
        }
        if (!ffp && !at->size && a < NV_VERTEX_ATTRS) {
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
    if (ffp) gvs_ffp_pack(); /* fills its own constants; do not copy a table immediately overwritten */
    else memcpy(s_gvs_blk + GVS_OFF_CONST, nv2a_vsh_constant(0), NV2A_VSH_CONSTANTS * 16u);

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
    s_gvs_guest_bytes+=copied;
    s_gvs.nidx = nidx;
    s_gvs.aa_sx = s_gpu.aa_sx;
    s_gvs.aa_sy = s_gpu.aa_sy;
    s_gvs.fog_mode = s_gpu.fog_enable ? (s_gpu.fog_mode ? s_gpu.fog_mode : 1u) : 0u;
    s_gvs.fog_px = s_gpu.fog_param[0];
    s_gvs.fog_py = s_gpu.fog_param[1];

    /* the CPU path's per-batch bookkeeping */
    xf_set_viewport();
    s_xf_no_tex = ffp && !(texcoord_attr()->offset && texcoord_attr()->stride);
    if (ffp) s_gpu.batches_ffp++;
    else { s_gpu.batches_program++; s_gpu.verts_program += n; }
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
    s_gvs_vcount = 0;
    {uint64_t began=gvs_timing_on()?gvs_clock_us():0;
     backend_flush_gvs(nidx);
     if(began)s_gvs_submit_us+=gvs_clock_us()-began;}
    s_pbx_gvs = NULL;
    if (s_gvs.verify) {
        if (s_gvs_vcount == (nidx > 4096 ? 4096 : nidx))
            gvs_verify_compare((const uint32_t *)(s_gvs_blk + GVS_OFF_IDX), nidx);
        else {
            Nv2aSurface surface;
            current_surface(&surface);
            if (++s_gvs_vskipped <= 8)
                fprintf(stderr, "[GPUVS-VERIFY] no readback: hash=%08X surface=%08X %ux%u bpp=%u\n",
                        s_gvs.hash, surface.color_va, surface.width, surface.height, surface.bytes_per_pixel);
        }
    }
    s_gvs_draws++;
    s_gvs_verts += n;
    gvs_stats();
    return 1;
}

static int gvs_try(void)
{
    uint64_t began=gvs_timing_on()?gvs_clock_us():0, submitted=s_gvs_submit_us;
    int result=gvs_try_impl();
    if(began){uint64_t elapsed=gvs_clock_us()-began,nested=s_gvs_submit_us-submitted;
        if(elapsed>=nested)s_gvs_prepare_us+=elapsed-nested;}
    return result;
}
