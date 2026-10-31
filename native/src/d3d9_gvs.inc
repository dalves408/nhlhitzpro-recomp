/* d3d9_gvs.inc -- GPU vertex-program path of the Direct3D 9 back end (#included by backend_d3d9.c).
 *
 * The D3D11 build gives the vertex shader a raw block of guest data and decodes the attributes in HLSL
 * (ByteAddressBuffer, integer ops, SV_VertexID). Shader Model 3.0 has none of that, so here the CPU decodes
 * each drawn vertex's attributes into float4 vertex-stream elements (cheap: no program runs on the CPU) and
 * the generated vs_3_0 runs the NV2A vertex program itself (the body is nv2a_vsh_gen_hlsl's, unchanged).
 *
 * Constants: c0..c191 the program's constants (the block's first 3072 bytes), c192 = surface w, h, depth
 * scale, 0; c193 = anti-aliasing scale x, y, fog mode, 0; c194 = fog parameters x, y.
 * A new program is compiled on a worker thread; the graphics thread makes the shader object (be_flip ->
 * gvs_pump); until it exists nv2a_gpuvs_probe says "not on the GPU" and the executor takes the CPU path. */

#define GVS_SPEC_MAX 1024
#define GVS_VB_BYTES (8u * 1024u * 1024u)

typedef struct {
    uint32_t hash, mask;
    volatile LONG st;                 /* 0 free, 1 compiling, 2 blob ready, 3 shader ready, 4 failed */
    ID3DBlob *blob;
    IDirect3DVertexShader9 *vs;
    IDirect3DVertexDeclaration9 *decl;
} GvsSpec;

extern uint64_t s_us_fetch;
static GvsSpec s_gspec[GVS_SPEC_MAX];
static uint32_t s_ngspec, s_gspec_fail, s_gdraws, s_gverts;
static CRITICAL_SECTION s_gcs;
static LONG s_gcs_init;

static void gvs_lock(int on)
{
    if (!s_gcs_init) {
        if (InterlockedCompareExchange(&s_gcs_init, 1, 0) == 0) InitializeCriticalSection(&s_gcs);
        else while (s_gcs_init != 1) Sleep(0);
    }
    if (on) EnterCriticalSection(&s_gcs); else LeaveCriticalSection(&s_gcs);
}

static const char k_gvs_pre[] =
    "float4 KC[192] : register(c0);\n"
    "float4 CS : register(c192);\n"
    "float4 CG : register(c193);\n"
    "float4 CF : register(c194);\n"
    "#define INF (1.0 / CS.w)\n"
    "static float4 G_IN[16];\n"
    "#define fetch(i, vi) G_IN[i]\n"
    "float4 K(uint i) { return KC[i]; }\n"
    "float4 KR(int i, int a0) { return KC[clamp(i + a0, 0, 191)]; }\n"
    "float4 m_dp3(float4 a, float4 b) { float s = a.x * b.x + a.y * b.y + a.z * b.z; return float4(s, s, s, s); }\n"
    "float4 m_dph(float4 a, float4 b) { float s = a.x * b.x + a.y * b.y + a.z * b.z + b.w; return float4(s, s, s, s); }\n"
    "float4 m_dp4(float4 a, float4 b) { float s = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; return float4(s, s, s, s); }\n"
    "float4 m_min(float4 a, float4 b) { return float4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w); }\n"
    "float4 m_max(float4 a, float4 b) { return float4(a.x >= b.x ? a.x : b.x, a.y >= b.y ? a.y : b.y, a.z >= b.z ? a.z : b.z, a.w >= b.w ? a.w : b.w); }\n"
    "float4 m_slt(float4 a, float4 b) { return float4(a.x < b.x ? 1.0 : 0.0, a.y < b.y ? 1.0 : 0.0, a.z < b.z ? 1.0 : 0.0, a.w < b.w ? 1.0 : 0.0); }\n"
    "float4 m_sge(float4 a, float4 b) { return float4(a.x >= b.x ? 1.0 : 0.0, a.y >= b.y ? 1.0 : 0.0, a.z >= b.z ? 1.0 : 0.0, a.w >= b.w ? 1.0 : 0.0); }\n"
    "float4 i_rcp(float x) { float f = (x == 0.0) ? INF : 1.0 / x; return float4(f, f, f, f); }\n"
    "float4 i_rcc(float x) {\n"
    "  float f = 1.0 / x;\n"
    "  if (abs(f) < 5.42101e-20) f = f < 0.0 ? -5.42101e-20 : 5.42101e-20;\n"
    "  if (abs(f) > 1.884467e19) f = f < 0.0 ? -1.884467e19 : 1.884467e19;\n"
    "  return float4(f, f, f, f);\n"
    "}\n"
    "float4 i_rsq(float x) { float f = rsqrt(abs(x)); return float4(f, f, f, f); }\n"
    "float4 i_exp(float x) { float fl = floor(x); return float4(exp2(fl), x - fl, exp2(x), 1.0); }\n"
    "float4 i_log(float x) {\n"
    "  float ax = abs(x);\n"
    "  if (ax == 0.0) return float4(-INF, 1.0, -INF, 1.0);\n"
    "  float e = floor(log2(ax));\n"
    "  return float4(e, ax / exp2(e), log2(ax), 1.0);\n"
    "}\n"
    "float4 i_lit(float4 c) {\n"
    "  float nl = c.x > 0.0 ? c.x : 0.0;\n"
    "  float nh = c.y > 0.0 ? c.y : 0.0;\n"
    "  float p = clamp(c.w, -127.9961, 127.9961);\n"
    "  float sp = (c.x > 0.0) ? ((p == 0.0) ? 1.0 : pow(nh, p)) : 0.0;\n"
    "  return float4(1.0, nl, sp, 1.0);\n"
    "}\n"
    /* fog_factor() of the executor: CG.z = 0 means fog off (0x800.. 0x804 are the NV097 fog mode words) */
    "float fogf(float d) {\n"
    "  float fm = CG.z;\n"
    "  if (fm == 0.0) return 1.0;\n"
    "  float f;\n"
    "  if (fm == 2048.0 || fm == 2050.0) f = CF.x + exp2(d * CF.y * 16.0) - 1.5;\n"
    "  else if (fm == 2049.0 || fm == 2051.0) f = CF.x + exp2(-d * d * CF.y * CF.y * 32.0) - 1.5;\n"
    "  else f = CF.x + d * CF.y - 1.0;\n"
    "  if (fm == 2050.0 || fm == 2051.0 || fm == 2052.0) f = abs(f);\n"
    "  if (f != f) f = 1.0;\n"
    "  return saturate(f);\n"
    "}\n"
    "struct VO { float4 pos : POSITION; float4 c : COLOR0; float4 d1 : TEXCOORD1;\n"
    "            float4 t0 : TEXCOORD2; float4 t1 : TEXCOORD3; float4 t2 : TEXCOORD4;\n"
    "            float4 t3 : TEXCOORD5; float fog : TEXCOORD6; };\n";

static char *gvs_assemble(const char *body, uint32_t mask, size_t *len)
{
    static const char post[] =
        "  float4 P = R[12];\n"
        "  if (P.w == 0.0 && (1.0 / P.w) > 0.0) P.w = 1.0;       // +0 -> 1 (executor: transform_vertex); -0 stays behind the eye\n"
        "  P.x *= CG.x; P.y *= CG.y;\n"
        "  VO o;\n"
        "  float cx = P.x / CS.x * 2.0 - 1.0 - 1.0 / CS.x;       // with the D3D9 half-pixel shift\n"
        "  float cy = 1.0 - P.y / CS.y * 2.0 + 1.0 / CS.y;\n"
        "  float cz = P.w > 0.0 ? min(P.z * CS.z, 1.0) : P.z * CS.z;   // a vertex behind the eye keeps its z (far-plane clip)\n"
        "  o.pos = float4(cx * P.w, cy * P.w, cz * P.w, P.w);\n"
        "  o.c = saturate(O[3]); o.d1 = saturate(O[4]);\n"
        "  o.t0 = O[9]; o.t1 = O[10]; o.t2 = O[11]; o.t3 = O[12];\n"
        "  o.fog = fogf(O[5].x);\n"
        "  return o;\n"
        "}\n";
    size_t cap = sizeof k_gvs_pre + strlen(body) + sizeof post + 2048, n = 0;
    char *s = (char *)malloc(cap);
    uint32_t i;
    if (!s) return NULL;
    memcpy(s, k_gvs_pre, sizeof k_gvs_pre - 1); n = sizeof k_gvs_pre - 1;
    n += (size_t)snprintf(s + n, cap - n, "struct VI {");
    for (i = 0; i < 16; i++)
        if ((mask >> i) & 1u) n += (size_t)snprintf(s + n, cap - n, " float4 v%u : TEXCOORD%u;", i, i);
    n += (size_t)snprintf(s + n, cap - n, " float4 dummy : COLOR1; };\n");
    n += (size_t)snprintf(s + n, cap - n, "VO vs_gpu(VI inp) {\n  uint vi = 0u;\n");
    for (i = 0; i < 16; i++)
        if ((mask >> i) & 1u) n += (size_t)snprintf(s + n, cap - n, "  G_IN[%u] = inp.v%u;\n", i, i);
    n += (size_t)snprintf(s + n, cap - n, "%s", body);
    memcpy(s + n, post, sizeof post - 1); n += sizeof post - 1;
    s[n] = 0;
    *len = n;
    return s;
}

typedef struct { GvsSpec *sp; char *src; size_t len; } GvsJob;

static DWORD WINAPI gvs_job_main(LPVOID arg)
{
    GvsJob *j = (GvsJob *)arg;
    ID3DBlob *b = compile_src(j->src, j->len, "vs_gpu", "vs_3_0");
    free(j->src);
    j->sp->blob = b;
    MemoryBarrier();
    InterlockedExchange(&j->sp->st, b ? 2 : 4);
    free(j);
    return 0;
}

/* The graphics thread: turn finished compiles into shader objects (the device is single-threaded). */
static void gvs_pump(void)
{
    uint32_t i, n = s_ngspec;
    for (i = 0; i < n; i++) {
        GvsSpec *sp = &s_gspec[i];
        if (sp->st != 2) continue;
        if (FAILED(IDirect3DDevice9_CreateVertexShader(g.dev, (const DWORD *)ID3D10Blob_GetBufferPointer(sp->blob), &sp->vs)))
            sp->vs = NULL;
        ID3D10Blob_Release(sp->blob);
        sp->blob = NULL;
        if (!sp->vs) s_gspec_fail++;
        if (g.log) logf_("GPU VS %08X: %s\n", sp->hash, sp->vs ? "ready" : "create FAILED");
        MemoryBarrier();
        InterlockedExchange(&sp->st, sp->vs ? 3 : 4);
    }
}

/* The executor asks before it builds a block: can the GPU draw this program right now? */
int nv2a_gpuvs_probe(const Nv2aGpuVs *e)
{
    static int env = -1;
    GvsSpec *sp = NULL;
    uint32_t i;
    int ready;

    if (env < 0) env = getenv("RECOMP_D3D11_GPUVS") != NULL;
    if (!env || (s_async && !s_dev_ready) || !g.dev || g.lost) return 0;
    gvs_lock(1);
    for (i = 0; i < s_ngspec; i++)
        if (s_gspec[i].hash == e->hash && s_gspec[i].mask == e->input_mask) { sp = &s_gspec[i]; break; }
    if (!sp && s_ngspec < GVS_SPEC_MAX) {
        char *body = (char *)malloc(1u << 20), *src = NULL;
        size_t len = 0;
        sp = &s_gspec[s_ngspec];
        memset(sp, 0, sizeof *sp);
        sp->hash = e->hash; sp->mask = e->input_mask;
        if (body && nv2a_vsh_gen_hlsl(e, body, 1u << 20) && (src = gvs_assemble(body, e->input_mask, &len)) != NULL) {
            GvsJob *j = (GvsJob *)malloc(sizeof *j);
            if (j) {
                j->sp = sp; j->src = src; j->len = len;
                sp->st = 1;
                s_ngspec++;                                  /* the slot is visible once it is a live job */
                if (!QueueUserWorkItem(gvs_job_main, j, WT_EXECUTELONGFUNCTION)) {
                    sp->st = 4; s_gspec_fail++;
                    free(src); free(j);
                }
            } else {
                free(src);
                sp->st = 4; s_ngspec++; s_gspec_fail++;
            }
        } else {
            sp->st = 4; s_ngspec++; s_gspec_fail++;
        }
        free(body);
        sp = NULL;                                           /* compiling: the CPU path draws meanwhile */
    }
    ready = sp && sp->st == 3;
    gvs_lock(0);
    return ready;
}

static GvsSpec *gvs_find(const Nv2aGpuVs *e)
{
    uint32_t i;
    for (i = 0; i < s_ngspec; i++)
        if (s_gspec[i].hash == e->hash && s_gspec[i].st == 3) return &s_gspec[i];
    return NULL;
}

/* one NV2A attribute of vertex vi, as the HLSL fetch() of the D3D11 build decodes it */
static uint32_t gvs_ld(const Nv2aGpuVs *gv, uint32_t a, uint32_t n)
{
    uint32_t v = 0;
    if ((uint64_t)a + n > gv->block_bytes) return 0;
    memcpy(&v, gv->block + a, n);
    return v;
}

static void gvs_fetch(const Nv2aGpuVs *gv, uint32_t i, uint32_t vi, float *o)
{
    const uint32_t *d = (const uint32_t *)(gv->block + GVS_OFF_ATTR) + i * 4u;
    uint32_t a, ty, sz, k, v;
    memcpy(o, (const float *)(gv->block + GVS_OFF_IMM) + i * 4u, 16);
    if (d[3]) return;
    a = d[0] + vi * d[1];
    ty = d[2] & 255u;
    sz = d[2] >> 8;
    if (ty == 0u) {
        v = gvs_ld(gv, a, 4);
        o[0] = (float)((v >> 16) & 255u) / 255.0f; o[1] = (float)((v >> 8) & 255u) / 255.0f;
        o[2] = (float)(v & 255u) / 255.0f;         o[3] = (float)(v >> 24) / 255.0f;
        return;
    }
    if (ty == 6u) {
        v = gvs_ld(gv, a, 4);
        o[0] = (float)((int32_t)(v << 21) >> 21) / 1023.0f;
        o[1] = (float)((int32_t)(v << 10) >> 21) / 1023.0f;
        o[2] = (float)((int32_t)v >> 22) / 511.0f;
        o[3] = 1.0f;
        return;
    }
    for (k = 0; k < 4u && k < sz; k++) {
        if (ty == 2u)      { v = gvs_ld(gv, a + k * 4u, 4); memcpy(&o[k], &v, 4); }
        else if (ty == 4u) o[k] = (float)gvs_ld(gv, a + k, 1) / 255.0f;
        else if (ty == 1u) o[k] = (float)(int16_t)gvs_ld(gv, a + k * 2u, 2) / 32767.0f;
        else if (ty == 5u) o[k] = (float)(int16_t)gvs_ld(gv, a + k * 2u, 2);
    }
}

static int gvs_create_decl(GvsSpec *sp)
{
    D3DVERTEXELEMENT9 el[17];
    uint32_t i, n = 0, off = 0;
    D3DVERTEXELEMENT9 end = D3DDECL_END();
    for (i = 0; i < 16; i++)
        if ((sp->mask >> i) & 1u) {
            el[n].Stream = 0; el[n].Offset = (WORD)off; el[n].Type = D3DDECLTYPE_FLOAT4;
            el[n].Method = D3DDECLMETHOD_DEFAULT; el[n].Usage = D3DDECLUSAGE_TEXCOORD; el[n].UsageIndex = (BYTE)i;
            n++; off += 16;
        }
    el[n] = end;
    return SUCCEEDED(IDirect3DDevice9_CreateVertexDeclaration(g.dev, el, &sp->decl));
}

/* One GPU-vertex-program draw; be_draw_impl has bound the render target and state and chosen the pixel shader
 * and textures. Returns 1 if drawn. */
static int gvs_draw_slow(const Nv2aGpuVs *gv, const Nv2aRenderState *rs_, const RenderTarget *r, float zscale)
{
    GvsSpec *sp = gvs_find(gv);
    uint32_t att[16], na = 0, i, stride, done = 0, nidx = gv->nidx, per;
    float cst[3][4];
    uint64_t tf0;

    if (!sp || !gv->block || !nidx) return 0;
    if (!sp->decl && !gvs_create_decl(sp)) return 0;
    for (i = 0; i < 16; i++) if ((sp->mask >> i) & 1u) att[na++] = i;
    stride = 16u * na;
    if (!stride) stride = 16u;
    per = GVS_VB_BYTES / stride / 3u * 3u;
    (void)rs_;
    cst[0][0] = (float)r->w; cst[0][1] = (float)r->h; cst[0][2] = zscale; cst[0][3] = 0.0f;
    cst[1][0] = gv->aa_sx ? gv->aa_sx : 1.0f; cst[1][1] = gv->aa_sy ? gv->aa_sy : 1.0f;
    cst[1][2] = (float)gv->fog_mode; cst[1][3] = 0.0f;
    cst[2][0] = gv->fog_px; cst[2][1] = gv->fog_py; cst[2][2] = cst[2][3] = 0.0f;
    IDirect3DDevice9_SetVertexShaderConstantF(g.dev, 0, (const float *)gv->block, 192);
    s_vc_valid = 0;                              /* the constant registers no longer match the fast path's cache */
    IDirect3DDevice9_SetVertexShaderConstantF(g.dev, 192, &cst[0][0], 3);
    IDirect3DDevice9_SetVertexDeclaration(g.dev, sp->decl);
    IDirect3DDevice9_SetVertexShader(g.dev, sp->vs);
    IDirect3DDevice9_SetStreamSource(g.dev, 1, NULL, 0, 0);
    while (done < nidx) {
        uint32_t n = nidx - done, bytes, at;
        uint8_t *p;
        DWORD fl;
        if (n > per) n = per;
        n -= n % 3u;
        if (!n) break;
        bytes = n * stride;
        if (s_gvb_off + bytes > GVS_VB_BYTES) { s_gvb_off = 0; fl = D3DLOCK_DISCARD; }
        else fl = s_gvb_off ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD;
        at = s_gvb_off;
        if (FAILED(IDirect3DVertexBuffer9_Lock(s_gvb, at, bytes, (void **)&p, fl))) return done != 0;
        tf0 = qpc_us();
        for (i = 0; i < n; i++) {
            uint32_t vi = gvs_ld(gv, GVS_OFF_IDX + (done + i) * 4u, 4), k;
            float *o = (float *)(p + (size_t)i * stride);
            for (k = 0; k < na; k++) gvs_fetch(gv, att[k], vi, o + 4u * k);
        }
        s_us_fetch += qpc_us() - tf0;
        IDirect3DVertexBuffer9_Unlock(s_gvb);
        s_gvb_off += bytes;
        IDirect3DDevice9_SetStreamSource(g.dev, 0, s_gvb, at, stride);
        IDirect3DDevice9_DrawPrimitive(g.dev, D3DPT_TRIANGLELIST, 0, n / 3u);
        done += n;
    }
    s_gdraws++; s_gverts += nidx;
    return 1;
}

/* ---- fast path: decode each distinct vertex once (attribute by attribute, into cacheable memory), then draw indexed ---- */
typedef struct { uint32_t off, stride; uint8_t ty, sz, cnst; float imm[4]; } GvsAttr;

static float *s_gscratch;
static size_t s_gscratch_cap;                 /* floats */
#define GVS_IB_INDICES (1u << 20)

static uint32_t gvs_elem_bytes(const GvsAttr *a)
{
    switch (a->ty) {
    case 0: case 6: return 4;
    case 2: return 4u * a->sz;
    case 4: return a->sz;
    case 1: case 5: return 2u * a->sz;
    default: return 0;
    }
}

/* 0 if some read would leave the block (the caller then takes the checked per-vertex path) */
static int gvs_decode_range(const Nv2aGpuVs *gv, const GvsAttr *at, uint32_t na, uint32_t vmin, uint32_t n, float *out)
{
    uint32_t k, i, S = 4u * na;
    for (k = 0; k < na; k++) {
        const GvsAttr *a = &at[k];
        float *o = out + 4u * k;
        if (a->cnst) {
            for (i = 0; i < n; i++, o += S) { o[0] = a->imm[0]; o[1] = a->imm[1]; o[2] = a->imm[2]; o[3] = a->imm[3]; }
        } else {
            uint32_t a0 = a->off + vmin * a->stride, el = gvs_elem_bytes(a), sz = a->sz;
            const uint8_t *p;
            if (!el || (uint64_t)a0 + (uint64_t)(n - 1) * a->stride + el > gv->block_bytes) return 0;
            p = gv->block + a0;
            switch (a->ty) {
            case 2:
                for (i = 0; i < n; i++, o += S, p += a->stride) {
                    memcpy(o, p, 4u * sz);
                    if (sz < 4) memcpy(o + sz, a->imm + sz, 4u * (4u - sz));
                }
                break;
            case 0:
                for (i = 0; i < n; i++, o += S, p += a->stride) {
                    uint32_t v; memcpy(&v, p, 4);
                    o[0] = (float)((v >> 16) & 255u) * (1.0f / 255.0f); o[1] = (float)((v >> 8) & 255u) * (1.0f / 255.0f);
                    o[2] = (float)(v & 255u) * (1.0f / 255.0f);          o[3] = (float)(v >> 24) * (1.0f / 255.0f);
                }
                break;
            case 4:
                for (i = 0; i < n; i++, o += S, p += a->stride) {
                    uint32_t c;
                    for (c = 0; c < 4; c++) o[c] = c < sz ? (float)p[c] * (1.0f / 255.0f) : a->imm[c];
                }
                break;
            case 1: case 5: {
                float sc = a->ty == 1 ? 1.0f / 32767.0f : 1.0f;
                for (i = 0; i < n; i++, o += S, p += a->stride) {
                    uint32_t c;
                    for (c = 0; c < 4; c++) {
                        int16_t v;
                        if (c < sz) { memcpy(&v, p + 2u * c, 2); o[c] = (float)v * sc; } else o[c] = a->imm[c];
                    }
                }
                break; }
            case 6:
                for (i = 0; i < n; i++, o += S, p += a->stride) {
                    uint32_t v; memcpy(&v, p, 4);
                    o[0] = (float)((int32_t)(v << 21) >> 21) / 1023.0f; o[1] = (float)((int32_t)(v << 10) >> 21) / 1023.0f;
                    o[2] = (float)((int32_t)v >> 22) / 511.0f;          o[3] = 1.0f;
                }
                break;
            default: return 0;
            }
        }
    }
    return 1;
}

static int gvs_draw(const Nv2aGpuVs *gv, const Nv2aRenderState *rs_, const RenderTarget *r, float zscale)
{
    GvsSpec *sp = gvs_find(gv);
    GvsAttr at[16];
    uint32_t na = 0, i, stride, nidx = gv->nidx, vmin = 0xFFFFFFFFu, vmax = 0, range, at_off;
    const uint32_t *idx;
    float cst[3][4];
    static float lastc[192 * 4];
    uint64_t tf0;
    uint16_t *ip;
    float *vp;
    DWORD fl;

    if (!sp || !gv->block || !nidx) return 0;
    if ((uint64_t)GVS_OFF_IDX + (uint64_t)nidx * 4u > gv->block_bytes) return 0;
    idx = (const uint32_t *)(gv->block + GVS_OFF_IDX);
    for (i = 0; i < nidx; i++) { uint32_t v = idx[i]; if (v < vmin) vmin = v; if (v > vmax) vmax = v; }
    range = vmax - vmin + 1u;
    if (range > 65536u || range > nidx * 4u + 64u || !s_gib) return gvs_draw_slow(gv, rs_, r, zscale);
    if (!sp->decl && !gvs_create_decl(sp)) return 0;
    for (i = 0; i < 16; i++)
        if ((sp->mask >> i) & 1u) {
            const uint32_t *d = (const uint32_t *)(gv->block + GVS_OFF_ATTR) + i * 4u;
            GvsAttr *a = &at[na++];
            a->off = d[0]; a->stride = d[1]; a->ty = (uint8_t)(d[2] & 255u); a->sz = (uint8_t)(d[2] >> 8); a->cnst = d[3] != 0;
            if (a->sz > 4) a->sz = 4;
            memcpy(a->imm, (const float *)(gv->block + GVS_OFF_IMM) + i * 4u, 16);
        }
    stride = 16u * na;
    if (!stride) return 0;
    if ((uint64_t)range * stride > GVS_VB_BYTES) return gvs_draw_slow(gv, rs_, r, zscale);
    if ((size_t)range * na * 4u > s_gscratch_cap) {
        free(s_gscratch);
        s_gscratch_cap = (size_t)range * na * 4u + 4096u;
        s_gscratch = (float *)malloc(s_gscratch_cap * sizeof(float));
        if (!s_gscratch) { s_gscratch_cap = 0; return 0; }
    }
    tf0 = qpc_us();
    if (!gvs_decode_range(gv, at, na, vmin, range, s_gscratch)) return gvs_draw_slow(gv, rs_, r, zscale);
    s_us_fetch += qpc_us() - tf0;

    /* the vertices */
    if (s_gvb_off + range * stride > GVS_VB_BYTES) { s_gvb_off = 0; fl = D3DLOCK_DISCARD; }
    else fl = s_gvb_off ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD;
    at_off = s_gvb_off;
    if (FAILED(IDirect3DVertexBuffer9_Lock(s_gvb, at_off, range * stride, (void **)&vp, fl))) return 0;
    memcpy(vp, s_gscratch, (size_t)range * stride);
    IDirect3DVertexBuffer9_Unlock(s_gvb);
    s_gvb_off += range * stride;
    /* the indices, relative to the first decoded vertex */
    if (s_gib_off + nidx > GVS_IB_INDICES) { s_gib_off = 0; fl = D3DLOCK_DISCARD; }
    else fl = s_gib_off ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD;
    if (FAILED(IDirect3DIndexBuffer9_Lock(s_gib, s_gib_off * 2u, nidx * 2u, (void **)&ip, fl))) return 0;
    for (i = 0; i < nidx; i++) ip[i] = (uint16_t)(idx[i] - vmin);
    IDirect3DIndexBuffer9_Unlock(s_gib);

    cst[0][0] = (float)r->w; cst[0][1] = (float)r->h; cst[0][2] = zscale; cst[0][3] = 0.0f;
    cst[1][0] = gv->aa_sx ? gv->aa_sx : 1.0f; cst[1][1] = gv->aa_sy ? gv->aa_sy : 1.0f;
    cst[1][2] = (float)gv->fog_mode; cst[1][3] = 0.0f;
    cst[2][0] = gv->fog_px; cst[2][1] = gv->fog_py; cst[2][2] = cst[2][3] = 0.0f;
    if (!s_vc_valid || memcmp(lastc, gv->block, sizeof lastc)) {         /* program constants: usually unchanged between draws */
        IDirect3DDevice9_SetVertexShaderConstantF(g.dev, 0, (const float *)gv->block, 192);
        memcpy(lastc, gv->block, sizeof lastc);
        s_vc_valid = 1;
    }
    IDirect3DDevice9_SetVertexShaderConstantF(g.dev, 192, &cst[0][0], 3);
    IDirect3DDevice9_SetVertexDeclaration(g.dev, sp->decl);
    IDirect3DDevice9_SetVertexShader(g.dev, sp->vs);
    IDirect3DDevice9_SetStreamSource(g.dev, 1, NULL, 0, 0);
    IDirect3DDevice9_SetStreamSource(g.dev, 0, s_gvb, at_off, stride);
    IDirect3DDevice9_SetIndices(g.dev, s_gib);
    IDirect3DDevice9_DrawIndexedPrimitive(g.dev, D3DPT_TRIANGLELIST, 0, 0, range, s_gib_off, nidx / 3u);
    s_gib_off += nidx;
    s_gdraws++; s_gverts += nidx;
    return 1;
}
