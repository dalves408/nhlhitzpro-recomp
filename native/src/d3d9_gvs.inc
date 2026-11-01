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
    s_gvs_bound = 0;
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
typedef struct { uint32_t off, stride; uint8_t ty, sz, cnst; float imm[4]; uint8_t idx, dtype, nb; uint16_t doff; } GvsAttr;

/* Vertex elements in the format the game stored them, so the CPU only copies bytes and the GPU converts: float1-4,
 * D3DCOLOR, UBYTE4N, SHORT2/4 (N). Anything else (and attributes whose missing components are not the D3D default
 * 0,0,0,1) stays float4, converted on the CPU. dtype 0xFF = float4 via the converting path. */
static uint32_t s_fmt_verts[8][5], s_fmt_float4_verts;
static void gvs_classify(GvsAttr *a)
{
    const float *m = a->imm;
    int d0 = (m[0] == 0.0f), d1 = (m[1] == 0.0f), d2 = (m[2] == 0.0f), d3 = (m[3] == 1.0f);
    a->dtype = 0xFF; a->nb = 16;
    if (a->cnst) return;
    switch (a->ty) {
    case 2:
        if (a->sz == 4) { a->dtype = D3DDECLTYPE_FLOAT4; a->nb = 16; }
        else if (a->sz == 3 && d3) { a->dtype = D3DDECLTYPE_FLOAT3; a->nb = 12; }
        else if (a->sz == 2 && d2 && d3) { a->dtype = D3DDECLTYPE_FLOAT2; a->nb = 8; }
        else if (a->sz == 1 && d1 && d2 && d3) { a->dtype = D3DDECLTYPE_FLOAT1; a->nb = 4; }
        break;
    case 0: a->dtype = D3DDECLTYPE_D3DCOLOR; a->nb = 4; break;
    case 4:
        if (a->sz == 4 && (g.caps.DeclTypes & D3DDTCAPS_UBYTE4N)) { a->dtype = D3DDECLTYPE_UBYTE4N; a->nb = 4; }
        break;
    case 1:
        if (a->sz == 2 && d2 && d3 && (g.caps.DeclTypes & D3DDTCAPS_SHORT2N)) { a->dtype = D3DDECLTYPE_SHORT2N; a->nb = 4; }
        else if (a->sz == 4 && (g.caps.DeclTypes & D3DDTCAPS_SHORT4N)) { a->dtype = D3DDECLTYPE_SHORT4N; a->nb = 8; }
        break;
    case 5:
        if (a->sz == 2 && d2 && d3) { a->dtype = D3DDECLTYPE_SHORT2; a->nb = 4; }
        else if (a->sz == 4) { a->dtype = D3DDECLTYPE_SHORT4; a->nb = 8; }
        break;
    default: break;
    }
}

#define GVS_DECL_MAX 128
static struct { uint32_t mask; uint8_t t[16]; IDirect3DVertexDeclaration9 *decl; } s_gdecl[GVS_DECL_MAX];
static int s_ngdecl;

static IDirect3DVertexDeclaration9 *gvs_decl_for(const GvsAttr *at, uint32_t na)
{
    uint32_t k, mask = 0;
    uint8_t t[16];
    D3DVERTEXELEMENT9 el[17];
    D3DVERTEXELEMENT9 end = D3DDECL_END();
    int i;
    memset(t, 0, sizeof t);
    for (k = 0; k < na; k++) { mask |= 1u << at[k].idx; t[at[k].idx] = at[k].dtype == 0xFF ? (uint8_t)D3DDECLTYPE_FLOAT4 : at[k].dtype; }
    for (i = 0; i < s_ngdecl; i++)
        if (s_gdecl[i].mask == mask && !memcmp(s_gdecl[i].t, t, sizeof t)) return s_gdecl[i].decl;
    if (s_ngdecl >= GVS_DECL_MAX) return NULL;
    for (k = 0; k < na; k++) {
        el[k].Stream = 0; el[k].Offset = at[k].doff; el[k].Type = (BYTE)t[at[k].idx];
        el[k].Method = D3DDECLMETHOD_DEFAULT; el[k].Usage = D3DDECLUSAGE_TEXCOORD; el[k].UsageIndex = at[k].idx;
    }
    el[na] = end;
    if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(g.dev, el, &s_gdecl[s_ngdecl].decl))) return NULL;
    s_gdecl[s_ngdecl].mask = mask; memcpy(s_gdecl[s_ngdecl].t, t, sizeof t);
    return s_gdecl[s_ngdecl++].decl;
}

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

/* 0 if some read would leave the block (the caller then takes the checked per-vertex path). out: n vertices of S bytes. */
static int gvs_decode_range(const Nv2aGpuVs *gv, const GvsAttr *at, uint32_t na, uint32_t vmin, uint32_t n, uint8_t *out, uint32_t S)
{
    uint32_t k, i;
    for (k = 0; k < na; k++) {
        const GvsAttr *a = &at[k];
        uint8_t *o = out + a->doff;
        if (a->cnst) {
            for (i = 0; i < n; i++, o += S) memcpy(o, a->imm, 16);
        } else {
            uint32_t a0 = a->off + vmin * a->stride, el = gvs_elem_bytes(a), sz = a->sz;
            const uint8_t *p;
            if (!el || (uint64_t)a0 + (uint64_t)(n - 1) * a->stride + el > gv->block_bytes) return 0;
            p = gv->block + a0;
            if (a->dtype != 0xFF) {                     /* the game's own bytes */
                switch (a->nb) {
                case 4:  for (i = 0; i < n; i++, o += S, p += a->stride) memcpy(o, p, 4); break;
                case 8:  for (i = 0; i < n; i++, o += S, p += a->stride) memcpy(o, p, 8); break;
                case 12: for (i = 0; i < n; i++, o += S, p += a->stride) memcpy(o, p, 12); break;
                default: for (i = 0; i < n; i++, o += S, p += a->stride) memcpy(o, p, 16); break;
                }
                continue;
            }
            switch (a->ty) {
            case 2:
                for (i = 0; i < n; i++, o += S, p += a->stride) {
                    float *f = (float *)o;
                    memcpy(f, p, 4u * sz);
                    if (sz < 4) memcpy(f + sz, a->imm + sz, 4u * (4u - sz));
                }
                break;
            case 4:
                for (i = 0; i < n; i++, o += S, p += a->stride) {
                    float *f = (float *)o; uint32_t c;
                    for (c = 0; c < 4; c++) f[c] = c < sz ? (float)p[c] * (1.0f / 255.0f) : a->imm[c];
                }
                break;
            case 1: case 5: {
                float sc = a->ty == 1 ? 1.0f / 32767.0f : 1.0f;
                for (i = 0; i < n; i++, o += S, p += a->stride) {
                    float *f = (float *)o; uint32_t c;
                    for (c = 0; c < 4; c++) {
                        int16_t v;
                        if (c < sz) { memcpy(&v, p + 2u * c, 2); f[c] = (float)v * sc; } else f[c] = a->imm[c];
                    }
                }
                break; }
            case 6:
                for (i = 0; i < n; i++, o += S, p += a->stride) {
                    float *f = (float *)o; uint32_t v; memcpy(&v, p, 4);
                    f[0] = (float)((int32_t)(v << 21) >> 21) / 1023.0f; f[1] = (float)((int32_t)(v << 10) >> 21) / 1023.0f;
                    f[2] = (float)((int32_t)v >> 22) / 511.0f;          f[3] = 1.0f;
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
    IDirect3DVertexDeclaration9 *dcl;

    if (!sp || !gv->block || !nidx) return 0;
    if ((uint64_t)GVS_OFF_IDX + (uint64_t)nidx * 4u > gv->block_bytes) return 0;
    idx = (const uint32_t *)(gv->block + GVS_OFF_IDX);
    for (i = 0; i < nidx; i++) { uint32_t v = idx[i]; if (v < vmin) vmin = v; if (v > vmax) vmax = v; }
    range = vmax - vmin + 1u;
    if (range > 65536u || range > nidx * 4u + 64u || !s_gib) return gvs_draw_slow(gv, rs_, r, zscale);
    stride = 0;
    for (i = 0; i < 16; i++)
        if ((sp->mask >> i) & 1u) {
            const uint32_t *d = (const uint32_t *)(gv->block + GVS_OFF_ATTR) + i * 4u;
            GvsAttr *a = &at[na++];
            a->off = d[0]; a->stride = d[1]; a->ty = (uint8_t)(d[2] & 255u); a->sz = (uint8_t)(d[2] >> 8); a->cnst = d[3] != 0;
            if (a->sz > 4) a->sz = 4;
            memcpy(a->imm, (const float *)(gv->block + GVS_OFF_IMM) + i * 4u, 16);
            a->idx = (uint8_t)i;
            gvs_classify(a);
            a->doff = (uint16_t)stride;
            stride += a->nb;
            if (!a->cnst) { if (a->dtype == 0xFF) s_fmt_float4_verts += nidx; else if (a->ty < 8 && a->sz < 5) s_fmt_verts[a->ty][a->sz] += nidx; }
        }
    if (!stride) return 0;
    dcl = gvs_decl_for(at, na);
    if (!dcl) return gvs_draw_slow(gv, rs_, r, zscale);
    if ((uint64_t)range * stride > GVS_VB_BYTES) return gvs_draw_slow(gv, rs_, r, zscale);
    if ((size_t)range * na * 4u > s_gscratch_cap) {
        free(s_gscratch);
        s_gscratch_cap = (size_t)range * na * 4u + 4096u;
        s_gscratch = (float *)malloc(s_gscratch_cap * sizeof(float));
        if (!s_gscratch) { s_gscratch_cap = 0; return 0; }
    }
    tf0 = qpc_us();
    if (!gvs_decode_range(gv, at, na, vmin, range, (uint8_t *)s_gscratch, stride)) return gvs_draw_slow(gv, rs_, r, zscale);
    s_us_fetch += qpc_us() - tf0;
    pf.g_decode += qpc_us() - tf0;
    tf0 = qpc_us();

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
    { uint64_t n_ = qpc_us(); pf.g_lock += n_ - tf0; tf0 = n_; }

    cst[0][0] = (float)r->w; cst[0][1] = (float)r->h; cst[0][2] = zscale; cst[0][3] = 0.0f;
    cst[1][0] = gv->aa_sx ? gv->aa_sx : 1.0f; cst[1][1] = gv->aa_sy ? gv->aa_sy : 1.0f;
    cst[1][2] = (float)gv->fog_mode; cst[1][3] = 0.0f;
    cst[2][0] = gv->fog_px; cst[2][1] = gv->fog_py; cst[2][2] = cst[2][3] = 0.0f;
    if (!s_vc_valid) {                                                    /* program constants: only the registers that changed */
        IDirect3DDevice9_SetVertexShaderConstantF(g.dev, 0, (const float *)gv->block, 192);
        memcpy(lastc, gv->block, sizeof lastc);
        s_vc_valid = 1;
    } else {
        const uint32_t *a = (const uint32_t *)lastc, *b = (const uint32_t *)gv->block;
        int lo = -1, hi = -1, k;
        for (k = 0; k < 192; k++)
            if (a[4 * k] != b[4 * k] || a[4 * k + 1] != b[4 * k + 1] || a[4 * k + 2] != b[4 * k + 2] || a[4 * k + 3] != b[4 * k + 3]) {
                if (lo < 0) lo = k;
                hi = k;
            }
        if (lo >= 0) {
            IDirect3DDevice9_SetVertexShaderConstantF(g.dev, (UINT)lo, (const float *)gv->block + 4 * lo, (UINT)(hi - lo + 1));
            memcpy(lastc + 4 * lo, (const float *)gv->block + 4 * lo, (size_t)(hi - lo + 1) * 16u);
        }
    }
    IDirect3DDevice9_SetVertexShaderConstantF(g.dev, 192, &cst[0][0], 3);
    if (!s_gvs_bound || s_gvs_decl != dcl) { IDirect3DDevice9_SetVertexDeclaration(g.dev, dcl); s_gvs_decl = dcl; }
    if (!s_gvs_bound || s_gvs_vs != sp->vs) { IDirect3DDevice9_SetVertexShader(g.dev, sp->vs); s_gvs_vs = sp->vs; }
    if (!s_gvs_bound) {
        IDirect3DDevice9_SetStreamSource(g.dev, 1, NULL, 0, 0);
        IDirect3DDevice9_SetIndices(g.dev, s_gib);
        s_gvs_bound = 1;
    }
    IDirect3DDevice9_SetStreamSource(g.dev, 0, s_gvb, at_off, stride);
    { uint64_t n_ = qpc_us(); pf.g_set += n_ - tf0; tf0 = n_; }
    IDirect3DDevice9_DrawIndexedPrimitive(g.dev, D3DPT_TRIANGLELIST, 0, 0, range, s_gib_off, nidx / 3u);
    pf.g_draw += qpc_us() - tf0;
    s_gib_off += nidx;
    s_gdraws++; s_gverts += nidx;
    return 1;
}
