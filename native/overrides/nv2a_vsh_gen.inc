/* nv2a_vsh_gen.inc -- PROJECT code, #included at the end of nv2a_vsh_interp.c
 * (it needs the static decoder). Turns the active NV2A vertex program into the
 * body of an HLSL vertex shader (see nv2a_pbx.h, Nv2aGpuVs). The semantics are
 * those of vsh_exec above; keep them in step. */
#include <stdarg.h>
#include "../src/nv2a_pbx.h"

typedef struct { char *p; size_t n, cap; int ovf; } GenBuf;

static void gen_pf(GenBuf *b, const char *fmt, ...)
{
    va_list ap;
    int r;

    if (b->ovf)
        return;
    va_start(ap, fmt);
    r = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= b->cap - b->n) {
        b->ovf = 1;
        return;
    }
    b->n += (size_t)r;
}

static const char *gen_mask(uint32_t m, char out[8])
{
    int k = 0;

    if (m & 8) out[k++] = 'x';
    if (m & 4) out[k++] = 'y';
    if (m & 2) out[k++] = 'z';
    if (m & 1) out[k++] = 'w';
    out[k] = 0;
    return out;
}

static void gen_src(GenBuf *b, const DecSrc *d)
{
    static const char sw[] = "xyzw";

    gen_pf(b, "%s", d->neg ? "-" : "");
    if (d->kind == 1)
        gen_pf(b, "R[%u]", (unsigned)d->idx);
    else if (d->kind == 2)
        gen_pf(b, "V[%u]", (unsigned)d->idx);
    else if (d->rel)
        gen_pf(b, "KR(%u, a0)", (unsigned)d->idx);
    else
        gen_pf(b, "K(%u)", (unsigned)(d->idx < NV2A_VSH_CONSTANTS ? d->idx : 0));
    if (!(d->sw[0] == 0 && d->sw[1] == 1 && d->sw[2] == 2 && d->sw[3] == 3))
        gen_pf(b, ".%c%c%c%c", sw[d->sw[0]], sw[d->sw[1]], sw[d->sw[2]], sw[d->sw[3]]);
}

int nv2a_vsh_export(Nv2aGpuVs *e)
{
    static int diag = -1;
    uint32_t s, n = 0, h = 2166136261u;

    if (diag < 0)
        diag = (getenv("RECOMP_VSH_DUMP") || getenv("RECOMP_VSH_PROFILE")
                || getenv("RECOMP_VSH_CHECK")) ? 1 : 0;
    if (diag)
        return 0;
    e->input_mask = 0;
    for (s = s_start_slot; s < NV2A_VSH_SLOTS; s++) {
        const DecIns *d;
        uint32_t k;

        if (!s_dec[s].valid)
            decode_slot(s);
        d = &s_dec[s];
        if (s_cxt_write && d->omask && !d->o_is_out)
            return 0;                         /* the program writes constants */
        memcpy(e->prog[n], s_program[s], sizeof e->prog[n]);
        n++;
        for (k = 0; k < 3; k++)
            if (((d->need >> k) & 1) && d->src[k].kind == 2)
                e->input_mask |= 1u << (d->src[k].idx & 15);
        for (k = 0; k < 4; k++)
            h = (h ^ s_program[s][k]) * 16777619u;
        if (d->final) {
            e->nslots = n;
            e->hash = h;
            return 1;
        }
    }
    return 0;
}

/* Emits the statements from "float4 V[16]" to the last output write. The
 * wrapper provides: vi (the vertex index), gbase, K(i), KR(i, a0), fetch(i, vi). */
int nv2a_vsh_gen_hlsl(const Nv2aGpuVs *e, char *out, size_t cap)
{
    GenBuf gb, *b = &gb;
    uint32_t s, i;
    char m1[8], m2[8];

    gb.p = out; gb.n = 0; gb.cap = cap; gb.ovf = 0;
    if (!cap)
        return 0;
    out[0] = 0;
    gen_pf(b, "  float4 V[16]; float4 R[13]; float4 O[13]; int a0 = 0;\n");
    gen_pf(b, "  [unroll] for (int ii = 0; ii < 16; ii++) V[ii] = float4(0.0, 0.0, 0.0, 1.0);\n");
    gen_pf(b, "  [unroll] for (int ij = 0; ij < 13; ij++) { R[ij] = float4(0.0, 0.0, 0.0, 0.0);"
              " O[ij] = float4(0.0, 0.0, 0.0, 0.0); }\n");
    gen_pf(b, "  O[3].w = 1.0; O[4].w = 1.0; O[9].w = 1.0; O[10].w = 1.0; O[11].w = 1.0; O[12].w = 1.0;\n");
    for (i = 0; i < 16; i++)
        if ((e->input_mask >> i) & 1)
            gen_pf(b, "  V[%u] = fetch(%uu, vi);\n", i, i);
    for (s = 0; s < e->nslots; s++) {
        DecIns d;
        decode_ins(e->prog[s], &d);
        gen_pf(b, "  { // slot %u\n", s);
        gen_pf(b, "    float4 m = float4(0.0, 0.0, 0.0, 0.0); float4 il = m;\n");
        if (d.need & 1) { gen_pf(b, "    float4 a = "); gen_src(b, &d.src[0]); gen_pf(b, ";\n"); }
        if (d.need & 2) { gen_pf(b, "    float4 bb = "); gen_src(b, &d.src[1]); gen_pf(b, ";\n"); }
        if (d.need & 4) { gen_pf(b, "    float4 c = "); gen_src(b, &d.src[2]); gen_pf(b, ";\n"); }
        switch (d.mac) {
        case 0:  break;
        case 1:  gen_pf(b, "    m = a;\n"); break;
        case 2:  gen_pf(b, "    m = a * bb;\n"); break;
        case 3:  gen_pf(b, "    m = a + c;\n"); break;
        case 4:  gen_pf(b, "    m = a * bb + c;\n"); break;
        case 5:  gen_pf(b, "    m = m_dp3(a, bb);\n"); break;
        case 6:  gen_pf(b, "    m = m_dph(a, bb);\n"); break;
        case 7:  gen_pf(b, "    m = m_dp4(a, bb);\n"); break;
        case 8:  gen_pf(b, "    m = float4(1.0, a.y * bb.y, a.z, bb.w);\n"); break;
        case 9:  gen_pf(b, "    m = m_min(a, bb);\n"); break;
        case 10: gen_pf(b, "    m = m_max(a, bb);\n"); break;
        case 11: gen_pf(b, "    m = m_slt(a, bb);\n"); break;
        case 12: gen_pf(b, "    m = m_sge(a, bb);\n"); break;
        case 13: gen_pf(b, "    a0 = (int)floor(a.x + 0.001); m = a;\n"); break;
        default: gen_pf(b, "    m = a;\n"); break;
        }
        switch (d.ilu) {
        case 0:  break;
        case 2:  gen_pf(b, "    il = i_rcp(c.x);\n"); break;
        case 3:  gen_pf(b, "    il = i_rcc(c.x);\n"); break;
        case 4:  gen_pf(b, "    il = i_rsq(c.x);\n"); break;
        case 5:  gen_pf(b, "    il = i_exp(c.x);\n"); break;
        case 6:  gen_pf(b, "    il = i_log(c.x);\n"); break;
        case 7:  gen_pf(b, "    il = i_lit(c);\n"); break;
        default: gen_pf(b, "    il = c;\n"); break;
        }
        if (d.mac && d.mac != 13 && d.mac_mask && d.tdst <= 12)
            gen_pf(b, "    R[%u].%s = m.%s;\n", (unsigned)d.tdst,
                   gen_mask(d.mac_mask, m1), gen_mask(d.mac_mask, m2));
        if (d.ilu && d.ilu_mask) {
            uint32_t it = d.mac ? 1u : d.tdst;
            if (it <= 12)
                gen_pf(b, "    R[%u].%s = il.%s;\n", (unsigned)it,
                       gen_mask(d.ilu_mask, m1), gen_mask(d.ilu_mask, m2));
        }
        if (d.omask && d.o_is_out) {
            const char *src = d.sel_ilu ? "il" : "m";
            if (d.oaddr == 0)
                gen_pf(b, "    R[12].%s = %s.%s;\n", gen_mask(d.omask, m1), src, gen_mask(d.omask, m2));
            else if (d.oaddr < 13)
                gen_pf(b, "    O[%u].%s = %s.%s;\n", (unsigned)d.oaddr, gen_mask(d.omask, m1),
                       src, gen_mask(d.omask, m2));
        }
        gen_pf(b, "  }\n");
        if (d.final)
            break;
    }
    return b->ovf ? 0 : 1;
}