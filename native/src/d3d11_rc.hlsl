// Register-combiner pixel shader for the D3D11 back end. Port of
// overrides/nv2a_combiner.c (nv2a_rc_eval) and rc_stage_fetch in
// overrides/nv2a_pb_exec.c; keep them in step. Embedded into the back end by
// scripts/hlsl2h.py -> native/src/d3d11_rc_hlsl.h.
cbuffer C : register(b0) { float2 surf; float zscale; float aref;
                           uint atest; uint afunc; uint textured; uint pad0;
                           float2 isz; float2 pad1; };
cbuffer RC : register(b1) {
    uint4  hdr;        // control, stage_program, final0, final1
    uint4  hdr2;       // final_c0, final_c1, clip-plane mode, clip-plane enable
    float4 fogc;       // fog colour rgb
    float4 uvs[4];     // xy: texture coordinate -> normalised uv; z: stage texture valid
    uint4  stg[16];    // [2s] colour icw, alpha icw, colour ocw, alpha ocw; [2s+1] factor0, factor1
};
#ifdef SPEC
// per-program specialisation (back end prepends the SPEC_* defines): the combiner words become
// literals so all register indices and branches fold away at compile time
static const uint4 HDR = uint4(SPEC_H0, SPEC_H1, SPEC_H2, SPEC_H3);
static const uint4 SPEC_W[8] = { SPEC_WORDS };
#define STGW(s) SPEC_W[s]
#define LOOPN SPEC_N
#define UNR [unroll]
#define NEEDS(k) ((((HDR.y >> ((k) * 5u)) & 31u) == 1u) || (((HDR.y >> ((k) * 5u)) & 31u) == 2u))
#else
#define HDR hdr
#define STGW(s) stg[2u * (s)]
#define LOOPN n
#define UNR
#define NEEDS(k) true
#endif
Texture2D tx0 : register(t0);
Texture2D tx1 : register(t1);
Texture2D tx2 : register(t2);
Texture2D tx3 : register(t3);
SamplerState sm0 : register(s0);
SamplerState sm1 : register(s1);
SamplerState sm2 : register(s2);
SamplerState sm3 : register(s3);

struct VI { float3 p : POSITION; float rhw : RHW; float4 c : COLOR; float2 uv : TEXCOORD0;
            float4 d1 : TEXCOORD1; float4 t0 : TEXCOORD2; float4 t1 : TEXCOORD3;
            float4 t2 : TEXCOORD4; float4 t3 : TEXCOORD5; float fog : TEXCOORD6; };
struct VO { float4 pos : SV_Position; float4 c : COLOR; float4 d1 : TEXCOORD1;
            float4 t0 : TEXCOORD2; float4 t1 : TEXCOORD3; float4 t2 : TEXCOORD4;
            float4 t3 : TEXCOORD5; float fog : TEXCOORD6; };

VO vs_rc(VI i) {
  VO o;
  float w = (i.rhw > 1e-12) ? 1.0 / i.rhw : 1.0;
  float cx = i.p.x / surf.x * 2.0 - 1.0;
  float cy = 1.0 - i.p.y / surf.y * 2.0;
  float cz = saturate(i.p.z * zscale);
  o.pos = float4(cx * w, cy * w, cz * w, w);
  o.c = i.c; o.d1 = i.d1; o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3; o.fog = i.fog;
  return o;
}

float4 unpackc(uint c) {
  return float4((float)((c >> 16) & 255u), (float)((c >> 8) & 255u),
                (float)(c & 255u), (float)(c >> 24)) / 255.0;
}

float mapin(float x, uint b) {
  float p = max(x, 0.0);
  uint m = (b >> 5) & 7u;
  if (m == 0u) return p;
  if (m == 1u) return 1.0 - saturate(x);
  if (m == 2u) return 2.0 * p - 1.0;
  if (m == 3u) return -2.0 * p + 1.0;
  if (m == 4u) return p - 0.5;
  if (m == 5u) return -p + 0.5;
  if (m == 6u) return x;
  return -x;
}

float3 mapout3(float3 x, uint m) {
  if (m == 0x08u) return x - 0.5;
  if (m == 0x10u) return x * 2.0;
  if (m == 0x18u) return (x - 0.5) * 2.0;
  if (m == 0x20u) return x * 4.0;
  if (m == 0x30u) return x * 0.5;
  return x;
}
float mapout1(float x, uint m) { return mapout3(float3(x, x, x), m).x; }

float3 read_rgb(float4 R[16], uint b) {
  float4 r = R[b & 15u];
  float3 v = (b & 0x10u) ? r.aaa : r.rgb;
  return float3(mapin(v.x, b), mapin(v.y, b), mapin(v.z, b));
}
float read_alpha(float4 R[16], uint b) {
  float4 r = R[b & 15u];
  return mapin((b & 0x10u) ? r.a : r.b, b);
}

float4 stage_value(uint mode, float4 c, float4 s) {
  if (mode == 0u) return float4(0, 0, 0, 1);
  if (mode == 1u || mode == 2u) return s;
  if (mode == 3u) return float4(1, 1, 1, 1);       // cube map: not supported, reads white
  if (mode == 4u) return saturate(c);
  return float4(0, 0, 0, 0);
}

float4 sample_stage(uint st, float4 c) {
  float q = (c.w != 0.0) ? c.w : 1.0;
  float2 uv = c.xy / q * uvs[st].xy;
  float4 r;
  if (st == 0u) r = tx0.Sample(sm0, uv);
  else if (st == 1u) r = tx1.Sample(sm1, uv);
  else if (st == 2u) r = tx2.Sample(sm2, uv);
  else r = tx3.Sample(sm3, uv);
  return r;
}

float4 ps_rc(VO i) : SV_Target {
  float4 R[16];
  float4 tcs[4];
  float4 smp[4];
  uint n = HDR.x & 255u, flags = HDR.x >> 8, sp = HDR.y, s, k;
  float4 out4;

  // every sample taken unconditionally (gradients need uniform flow)
  tcs[0] = i.t0; tcs[1] = i.t1; tcs[2] = i.t2; tcs[3] = i.t3;
  if (NEEDS(0u)) smp[0] = tx0.Sample(sm0, tcs[0].xy / ((tcs[0].w != 0.0) ? tcs[0].w : 1.0) * uvs[0].xy);
  else smp[0] = float4(0, 0, 0, 0);
  if (NEEDS(1u)) smp[1] = tx1.Sample(sm1, tcs[1].xy / ((tcs[1].w != 0.0) ? tcs[1].w : 1.0) * uvs[1].xy);
  else smp[1] = float4(0, 0, 0, 0);
  if (NEEDS(2u)) smp[2] = tx2.Sample(sm2, tcs[2].xy / ((tcs[2].w != 0.0) ? tcs[2].w : 1.0) * uvs[2].xy);
  else smp[2] = float4(0, 0, 0, 0);
  if (NEEDS(3u)) smp[3] = tx3.Sample(sm3, tcs[3].xy / ((tcs[3].w != 0.0) ? tcs[3].w : 1.0) * uvs[3].xy);
  else smp[3] = float4(0, 0, 0, 0);

  if (n > 8u) n = 8u;
  [unroll] for (k = 0u; k < 16u; k++) R[k] = float4(0, 0, 0, 0);
  R[3] = float4(fogc.rgb, i.fog);
  R[4] = i.c;
  R[5] = i.d1;
  [unroll] for (k = 0u; k < 4u; k++) {
    uint mode = (sp >> (k * 5u)) & 31u;
    float4 sv = smp[k];
    if (uvs[k].z == 0.0) sv = float4(1, 1, 1, 1);
    R[8u + k] = stage_value(mode, tcs[k], sv);
  }
  R[12].a = (sp & 31u) ? R[8].a : 1.0;
  if (hdr2.w != 0u) {                      // CLIPPLANE stages kill the pixel (xemu psh.c)
    [unroll] for (k = 0u; k < 4u; k++) {
      if (((sp >> (k * 5u)) & 31u) == 5u) {
        [unroll] for (uint j = 0u; j < 4u; j++) {
          bool ge = ((hdr2.z >> (k * 4u + j)) & 1u) != 0u;
          float v = tcs[k][j];
          if (ge ? (v >= 0.0) : (v < 0.0)) discard;
        }
      }
    }
  }

  UNR for (s = 0u; s < LOOPN; s++) {
    uint icw = STGW(s).x, aicw = STGW(s).y, ocw = STGW(s).z, aocw = STGW(s).w;
    uint fl = ocw >> 12, afl = aocw >> 12;
    float3 A, B, C, D, pab, pcd, ab, cd, ms, sum;
    float aA, aB, aC, aD, apab, apcd, aab, acd, ams, asum;
    bool mux_cd;

    R[1] = unpackc(stg[2u * ((flags & 0x010u) ? s : 0u) + 1u].x);
    R[2] = unpackc(stg[2u * ((flags & 0x100u) ? s : 0u) + 1u].y);
    mux_cd = (flags & 1u) ? (R[12].a >= 0.5) : (((int)(R[12].a * 255.0) & 1) != 0);

    A = read_rgb(R, icw >> 24); B = read_rgb(R, icw >> 16);
    C = read_rgb(R, icw >> 8);  D = read_rgb(R, icw);
    pab = (fl & 2u) ? dot(A, B).xxx : A * B;
    pcd = (fl & 1u) ? dot(C, D).xxx : C * D;
    ab = clamp(mapout3(pab, fl & 0x38u), -1.0, 1.0);
    cd = clamp(mapout3(pcd, fl & 0x38u), -1.0, 1.0);
    sum = (fl & 4u) ? (mux_cd ? pcd : pab) : pab + pcd;
    ms = clamp(mapout3(sum, fl & 0x38u), -1.0, 1.0);

    aA = read_alpha(R, aicw >> 24); aB = read_alpha(R, aicw >> 16);
    aC = read_alpha(R, aicw >> 8);  aD = read_alpha(R, aicw);
    apab = aA * aB; apcd = aC * aD;
    aab = clamp(mapout1(apab, afl & 0x38u), -1.0, 1.0);
    acd = clamp(mapout1(apcd, afl & 0x38u), -1.0, 1.0);
    asum = (afl & 4u) ? (mux_cd ? apcd : apab) : apab + apcd;
    ams = clamp(mapout1(asum, afl & 0x38u), -1.0, 1.0);

    if ((ocw >> 4) & 15u) {
      uint d = (ocw >> 4) & 15u;
      R[d].rgb = ab;
      if (fl & 0x80u) R[d].a = ab.z;
    }
    if (ocw & 15u) {
      uint d = ocw & 15u;
      R[d].rgb = cd;
      if (fl & 0x40u) R[d].a = cd.z;
    }
    if ((ocw >> 8) & 15u) R[(ocw >> 8) & 15u].rgb = ms;
    if ((aocw >> 4) & 15u) R[(aocw >> 4) & 15u].a = aab;
    if (aocw & 15u)        R[aocw & 15u].a = acd;
    if ((aocw >> 8) & 15u) R[(aocw >> 8) & 15u].a = ams;
    R[0] = float4(0, 0, 0, 0);
  }

  if (HDR.z != 0u || HDR.w != 0u) {
    uint f0 = HDR.z, f1 = HDR.w, ff = f1 & 255u;
    float3 A, B, C, D, E, F, a, b;
    R[1] = unpackc(hdr2.x);
    R[2] = unpackc(hdr2.y);
    a = (ff & 0x40u) ? 1.0 - R[5].rgb : R[5].rgb;
    b = (ff & 0x20u) ? 1.0 - R[12].rgb : R[12].rgb;
    R[14].rgb = (ff & 0x80u) ? saturate(a + b) : a + b;
    R[14].a = 0.0;
    E = read_rgb(R, f1 >> 24);
    F = read_rgb(R, f1 >> 16);
    R[15].rgb = E * F;
    R[15].a = 0.0;
    A = read_rgb(R, f0 >> 24); B = read_rgb(R, f0 >> 16);
    C = read_rgb(R, f0 >> 8);  D = read_rgb(R, f0);
    out4.rgb = D + C * (1.0 - A) + B * A;
    out4.a = read_alpha(R, f1 >> 8);
  } else {
    out4 = R[12];
  }
  out4 = saturate(out4);
  if (atest) {
    int a = (int)(out4.a * 255.0 + 0.5); int r = (int)(aref + 0.5);
    bool ok = true;
    if (afunc == 0) ok = false;
    else if (afunc == 1) ok = a < r;
    else if (afunc == 2) ok = a == r;
    else if (afunc == 3) ok = a <= r;
    else if (afunc == 4) ok = a > r;
    else if (afunc == 5) ok = a != r;
    else if (afunc == 6) ok = a >= r;
    if (!ok) discard;
  }
  return out4;
}