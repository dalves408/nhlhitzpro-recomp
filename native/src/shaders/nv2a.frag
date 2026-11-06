#version 330 core
// Register-combiner shader ported from the Windows shader and nv2a_rc_eval.
in vec4 v_color, v_d1;
in vec4 v_tc[4];
in float v_fog;
out vec4 fragColor;
uniform uvec4 hdr2;
#ifndef RC_SPECIALIZED
uniform uvec4 hdr;
#define WORDS(s) stg[2u*(s)]
#else
const uvec4 hdr = uvec4(RC_H0,RC_H1,RC_H2,RC_H3);
const uvec4 words[8] = uvec4[8](RC_WORDS);
#define WORDS(s) words[s]
#endif
uniform vec4 fogc, uvs[4];
uniform uvec4 stg[16];
uniform uint atest, afunc, use_rc;
uniform float aref;
uniform sampler2D tx0, tx1, tx2, tx3;
#define NEEDS(k) true
float saturate(float x) { return clamp(x,0.0,1.0); }
vec3 saturate(vec3 x) { return clamp(x,0.0,1.0); }
vec4 saturate(vec4 x) { return clamp(x,0.0,1.0); }
vec4 unpackc(uint c) {
  return vec4(float((c >> 16) & 255u), float((c >> 8) & 255u),
                float(c & 255u), float(c >> 24)) / 255.0;
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

vec3 mapout3(vec3 x, uint m) {
  if (m == 0x08u) return x - 0.5;
  if (m == 0x10u) return x * 2.0;
  if (m == 0x18u) return (x - 0.5) * 2.0;
  if (m == 0x20u) return x * 4.0;
  if (m == 0x30u) return x * 0.5;
  return x;
}
float mapout1(float x, uint m) { return mapout3(vec3(x, x, x), m).x; }

vec3 read_rgb(vec4 R[16], uint b) {
  vec4 r = R[b & 15u];
  vec3 v = ((b & 0x10u) != 0u) ? r.aaa : r.rgb;
  return vec3(mapin(v.x, b), mapin(v.y, b), mapin(v.z, b));
}
float read_alpha(vec4 R[16], uint b) {
  vec4 r = R[b & 15u];
  return mapin(((b & 0x10u) != 0u) ? r.a : r.b, b);
}

vec4 stage_value(uint mode, vec4 c, vec4 s) {
  if (mode == 0u) return vec4(0, 0, 0, 1);
  if (mode == 1u || mode == 2u) return s;
  if (mode == 3u) return vec4(1, 1, 1, 1);       // cube map: not supported, reads white
  if (mode == 4u) return saturate(c);
  return vec4(0, 0, 0, 0);
}

vec4 sample_stage(uint st, vec4 c) {
  float q = (c.w != 0.0) ? c.w : 1.0;
  vec2 uv = c.xy / q * uvs[st].xy;
  vec4 r;
  if (st == 0u) r = texture(tx0, uv);
  else if (st == 1u) r = texture(tx1, uv);
  else if (st == 2u) r = texture(tx2, uv);
  else r = texture(tx3, uv);
  return r;
}

void alpha_test(float alpha) {
  if (atest != 0u) {
    int a = int(alpha * 255.0 + 0.5); int r = int(aref + 0.5);
    bool ok = true;
    if (afunc == 0u) ok = false;
    else if (afunc == 1u) ok = a < r;
    else if (afunc == 2u) ok = a == r;
    else if (afunc == 3u) ok = a <= r;
    else if (afunc == 4u) ok = a > r;
    else if (afunc == 5u) ok = a != r;
    else if (afunc == 6u) ok = a >= r;
    if (!ok) discard;
  }
}

void main() {
  if (use_rc == 0u) {
    vec4 plain = v_color * texture(tx0, v_tc[0].xy * uvs[0].xy);
    alpha_test(plain.a); fragColor=plain; return;
  }
  vec4 R[16];
  vec4 tcs[4];
  vec4 smp[4];
  uint n = hdr.x & 255u, flags = hdr.x >> 8, sp = hdr.y, s, k;
  vec4 out4;

  // every sample taken unconditionally (gradients need uniform flow)
  tcs[0] = v_tc[0]; tcs[1] = v_tc[1]; tcs[2] = v_tc[2]; tcs[3] = v_tc[3];
  if (NEEDS(0u)) smp[0] = texture(tx0, tcs[0].xy / ((tcs[0].w != 0.0) ? tcs[0].w : 1.0) * uvs[0].xy);
  else smp[0] = vec4(0, 0, 0, 0);
  if (NEEDS(1u)) smp[1] = texture(tx1, tcs[1].xy / ((tcs[1].w != 0.0) ? tcs[1].w : 1.0) * uvs[1].xy);
  else smp[1] = vec4(0, 0, 0, 0);
  if (NEEDS(2u)) smp[2] = texture(tx2, tcs[2].xy / ((tcs[2].w != 0.0) ? tcs[2].w : 1.0) * uvs[2].xy);
  else smp[2] = vec4(0, 0, 0, 0);
  if (NEEDS(3u)) smp[3] = texture(tx3, tcs[3].xy / ((tcs[3].w != 0.0) ? tcs[3].w : 1.0) * uvs[3].xy);
  else smp[3] = vec4(0, 0, 0, 0);

  if (n > 8u) n = 8u;
   for (k = 0u; k < 16u; k++) R[k] = vec4(0, 0, 0, 0);
  R[3] = vec4(fogc.rgb, v_fog);
  R[4] = v_color;
  R[5] = v_d1;
   for (k = 0u; k < 4u; k++) {
    uint mode = (sp >> (k * 5u)) & 31u;
    vec4 sv = smp[k];
    if (uvs[k].z == 0.0) sv = vec4(1, 1, 1, 1);
    R[8u + k] = stage_value(mode, tcs[k], sv);
  }
  R[12].a = ((sp & 31u) != 0u) ? R[8].a : 1.0;
  if (hdr2.w != 0u) {                      // CLIPPLANE stages kill the pixel (xemu psh.c)
     for (k = 0u; k < 4u; k++) {
      if (((sp >> (k * 5u)) & 31u) == 5u) {
         for (uint j = 0u; j < 4u; j++) {
          bool ge = ((hdr2.z >> (k * 4u + j)) & 1u) != 0u;
          float v = tcs[k][j];
          if (ge ? (v >= 0.0) : (v < 0.0)) discard;
        }
      }
    }
  }

  for (s = 0u; s < n; s++) {
    uint icw = WORDS(s).x, aicw = WORDS(s).y, ocw = WORDS(s).z, aocw = WORDS(s).w;
    uint fl = ocw >> 12, afl = aocw >> 12;
    vec3 A, B, C, D, pab, pcd, ab, cd, ms, sum;
    float aA, aB, aC, aD, apab, apcd, aab, acd, ams, asum;
    bool mux_cd;

    R[1] = unpackc(stg[2u * (((flags & 0x010u) != 0u) ? s : 0u) + 1u].x);
    R[2] = unpackc(stg[2u * (((flags & 0x100u) != 0u) ? s : 0u) + 1u].y);
    mux_cd = ((flags & 1u) != 0u) ? (R[12].a >= 0.5) : ((int(R[12].a * 255.0) & 1) != 0);

    A = read_rgb(R, icw >> 24); B = read_rgb(R, icw >> 16);
    C = read_rgb(R, icw >> 8);  D = read_rgb(R, icw);
    pab = ((fl & 2u) != 0u) ? vec3(dot(A, B)) : A * B;
    pcd = ((fl & 1u) != 0u) ? vec3(dot(C, D)) : C * D;
    ab = clamp(mapout3(pab, fl & 0x38u), -1.0, 1.0);
    cd = clamp(mapout3(pcd, fl & 0x38u), -1.0, 1.0);
    sum = ((fl & 4u) != 0u) ? (mux_cd ? pcd : pab) : pab + pcd;
    ms = clamp(mapout3(sum, fl & 0x38u), -1.0, 1.0);

    aA = read_alpha(R, aicw >> 24); aB = read_alpha(R, aicw >> 16);
    aC = read_alpha(R, aicw >> 8);  aD = read_alpha(R, aicw);
    apab = aA * aB; apcd = aC * aD;
    aab = clamp(mapout1(apab, afl & 0x38u), -1.0, 1.0);
    acd = clamp(mapout1(apcd, afl & 0x38u), -1.0, 1.0);
    asum = ((afl & 4u) != 0u) ? (mux_cd ? apcd : apab) : apab + apcd;
    ams = clamp(mapout1(asum, afl & 0x38u), -1.0, 1.0);

    if (((ocw >> 4) & 15u) != 0u) {
      uint d = (ocw >> 4) & 15u;
      R[d].rgb = ab;
      if ((fl & 0x80u) != 0u) R[d].a = ab.z;
    }
    if ((ocw & 15u) != 0u) {
      uint d = ocw & 15u;
      R[d].rgb = cd;
      if ((fl & 0x40u) != 0u) R[d].a = cd.z;
    }
    if (((ocw >> 8) & 15u) != 0u) R[(ocw >> 8) & 15u].rgb = ms;
    if (((aocw >> 4) & 15u) != 0u) R[(aocw >> 4) & 15u].a = aab;
    if ((aocw & 15u) != 0u)        R[aocw & 15u].a = acd;
    if (((aocw >> 8) & 15u) != 0u) R[(aocw >> 8) & 15u].a = ams;
    R[0] = vec4(0, 0, 0, 0);
  }

  if (hdr.z != 0u || hdr.w != 0u) {
    uint f0 = hdr.z, f1 = hdr.w, ff = f1 & 255u;
    vec3 A, B, C, D, E, F, a, b;
    R[1] = unpackc(hdr2.x);
    R[2] = unpackc(hdr2.y);
    a = ((ff & 0x40u) != 0u) ? 1.0 - R[5].rgb : R[5].rgb;
    b = ((ff & 0x20u) != 0u) ? 1.0 - R[12].rgb : R[12].rgb;
    R[14].rgb = ((ff & 0x80u) != 0u) ? saturate(a + b) : a + b;
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
  alpha_test(out4.a);
  fragColor = out4;
}
