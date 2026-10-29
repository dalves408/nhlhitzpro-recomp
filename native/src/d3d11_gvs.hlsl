// GPU vertex-program path for the D3D11 back end: the generated body of one NV2A
// vertex program (nv2a_vsh_gen.inc) is spliced in at the marker line. Raw data comes
// from one ByteAddressBuffer ring (layout: nv2a_pbx.h, Nv2aGpuVs). Embedded by
// scripts/hlsl2h_gvs.py -> native/src/d3d11_gvs_hlsl.h. Keep in step with
// overrides/nv2a_pb_exec.c (transform_vertex, fetch_attr, fog_factor) and nv2a_vsh_interp.c.
cbuffer C : register(b0) { float2 surf; float zscale; float aref;
                           uint atest; uint afunc; uint textured; uint pad0;
                           float2 isz; float2 pad1; };
cbuffer G : register(b2) { uint gbase; uint gpad0; float2 gaa;
                           uint gfogmode; float gfpx; float gfpy; float gpad1; };
ByteAddressBuffer RB : register(t0);
#define INF asfloat(0x7f800000u)

struct VO { float4 pos : SV_Position; float4 c : COLOR; float4 d1 : TEXCOORD1;
            float4 t0 : TEXCOORD2; float4 t1 : TEXCOORD3; float4 t2 : TEXCOORD4;
            float4 t3 : TEXCOORD5; float fog : TEXCOORD6; };

float4 K(uint i) { return asfloat(RB.Load4(gbase + i * 16u)); }
float4 KR(int i, int a0) { int ci = i + a0; if (ci < 0 || ci >= 192) ci = 0; return K((uint)ci); }

float4 m_dp3(float4 a, float4 b) { float s = a.x * b.x + a.y * b.y + a.z * b.z; return float4(s, s, s, s); }
float4 m_dph(float4 a, float4 b) { float s = a.x * b.x + a.y * b.y + a.z * b.z + b.w; return float4(s, s, s, s); }
float4 m_dp4(float4 a, float4 b) { float s = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; return float4(s, s, s, s); }
float4 m_min(float4 a, float4 b) { return float4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w); }
float4 m_max(float4 a, float4 b) { return float4(a.x >= b.x ? a.x : b.x, a.y >= b.y ? a.y : b.y, a.z >= b.z ? a.z : b.z, a.w >= b.w ? a.w : b.w); }
float4 m_slt(float4 a, float4 b) { return float4(a.x < b.x ? 1.0 : 0.0, a.y < b.y ? 1.0 : 0.0, a.z < b.z ? 1.0 : 0.0, a.w < b.w ? 1.0 : 0.0); }
float4 m_sge(float4 a, float4 b) { return float4(a.x >= b.x ? 1.0 : 0.0, a.y >= b.y ? 1.0 : 0.0, a.z >= b.z ? 1.0 : 0.0, a.w >= b.w ? 1.0 : 0.0); }

float4 i_rcp(float x) { float f = (x == 0.0) ? INF : 1.0 / x; return float4(f, f, f, f); }
float4 i_rcc(float x) {
  float f = 1.0 / x;
  if (abs(f) < 5.42101e-20) f = f < 0.0 ? -5.42101e-20 : 5.42101e-20;
  if (abs(f) > 1.884467e19) f = f < 0.0 ? -1.884467e19 : 1.884467e19;
  return float4(f, f, f, f);
}
float4 i_rsq(float x) { float f = rsqrt(abs(x)); return float4(f, f, f, f); }
float4 i_exp(float x) { float fl = floor(x); return float4(exp2(fl), x - fl, exp2(x), 1.0); }
float4 i_log(float x) {
  float ax = abs(x);
  if (ax == 0.0) return float4(-INF, 1.0, -INF, 1.0);
  float e = floor(log2(ax));
  return float4(e, ax / exp2(e), log2(ax), 1.0);
}
float4 i_lit(float4 c) {
  float nl = c.x > 0.0 ? c.x : 0.0;
  float nh = c.y > 0.0 ? c.y : 0.0;
  float p = clamp(c.w, -127.9961, 127.9961);
  float sp = (c.x > 0.0) ? ((p == 0.0) ? 1.0 : pow(nh, p)) : 0.0;
  return float4(1.0, nl, sp, 1.0);
}

uint ld8(uint a) { return (RB.Load(a & ~3u) >> ((a & 3u) * 8u)) & 255u; }
uint ld16(uint a) { return ld8(a) | (ld8(a + 1u) << 8); }
uint ld32(uint a) { if ((a & 3u) == 0u) return RB.Load(a); return ld16(a) | (ld16(a + 2u) << 16); }

float4 fetch(uint i, uint vi) {
  uint4 d = RB.Load4(gbase + GVS_ATTR + i * 16u);
  float4 o = asfloat(RB.Load4(gbase + GVS_IMM + i * 16u));
  if (d.w != 0u) return o;
  uint a = gbase + d.x + vi * d.y;
  uint ty = d.z & 255u;
  uint sz = d.z >> 8;
  if (ty == 0u) {
    uint v = ld32(a);
    return float4((float)((v >> 16) & 255u), (float)((v >> 8) & 255u), (float)(v & 255u), (float)(v >> 24)) / 255.0;
  }
  if (ty == 6u) {
    uint v = ld32(a);
    return float4((float)((int)(v << 21) >> 21) / 1023.0, (float)((int)(v << 10) >> 21) / 1023.0,
                  (float)((int)v >> 22) / 511.0, 1.0);
  }
  [unroll] for (uint k = 0u; k < 4u; k++) {
    if (k < sz) {
      if (ty == 2u) o[k] = asfloat(ld32(a + k * 4u));
      else if (ty == 4u) o[k] = (float)ld8(a + k) / 255.0;
      else if (ty == 1u) o[k] = (float)((int)(ld16(a + k * 2u) << 16) >> 16) / 32767.0;
      else if (ty == 5u) o[k] = (float)((int)(ld16(a + k * 2u) << 16) >> 16);
    }
  }
  return o;
}

// fog_factor() of the executor: gfogmode 0 = fog off
float fogf(float d) {
  if (gfogmode == 0u) return 1.0;
  float f;
  if (gfogmode == 0x800u || gfogmode == 0x802u) f = gfpx + exp2(d * gfpy * 16.0) - 1.5;
  else if (gfogmode == 0x801u || gfogmode == 0x803u) f = gfpx + exp2(-d * d * gfpy * gfpy * 32.0) - 1.5;
  else f = gfpx + d * gfpy - 1.0;
  if (gfogmode == 0x802u || gfogmode == 0x803u || gfogmode == 0x804u) f = abs(f);
  if (f != f) f = 1.0;
  return saturate(f);
}

VO vs_gpu(uint vid : SV_VertexID) {
  uint vi = RB.Load(gbase + GVS_IDX + vid * 4u);
//@@BODY@@
  float4 P = R[12];
  if (asuint(P.w) == 0u) P.w = 1.0;       // +0 -> 1 (executor: transform_vertex); -0 stays behind the eye
  P.x *= gaa.x; P.y *= gaa.y;
  VO o;
#ifdef GVS_VERIFY
  o.pos = P;
#else
  float cx = P.x / surf.x * 2.0 - 1.0;
  float cy = 1.0 - P.y / surf.y * 2.0;
#ifdef GVS_FARCLIP
  // RECOMP_D3D11_GPUVS_FARCLIP=1: a vertex behind the eye (w < 0) keeps its z. Its screen z lies
  // beyond the clip range's top, and clamping it to 1 moved it onto the far plane, so the far-plane
  // clip that NV2A (CULL_NEAR_FAR) and xemu do no longer removed it: a quad that crosses the eye
  // (the arena spotlight pools in the bench/entrance shots) then covered most of the screen.
  float cz = P.w > 0.0 ? min(P.z * zscale, 1.0) : P.z * zscale;
#else
  float cz = min(P.z * zscale, 1.0);      // no lower clamp: D3D clips z < 0 like the CPU near-plane clip
#endif
  o.pos = float4(cx * P.w, cy * P.w, cz * P.w, P.w);
#endif
  o.c = saturate(O[3]); o.d1 = saturate(O[4]);
  o.t0 = O[9]; o.t1 = O[10]; o.t2 = O[11]; o.t3 = O[12];
  o.fog = fogf(O[5].x);
  return o;
}

[maxvertexcount(1)] void gs_pass(point VO i[1], inout PointStream<VO> s) { s.Append(i[0]); }