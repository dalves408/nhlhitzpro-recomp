/* 16-tap Blackman-Harris windowed-sinc voice resampling, ported from
 * NHL Hitz Pro 1.0.7. Included by apu_vp.c and its streaming regression.
 * Same LGPL-2.1-or-later license as apu_vp.c. No graphics/guest changes. */
#define RS_HALF   8
#define RS_TAPS   (2 * RS_HALF)
#define RS_PH     512                              /* kernel samples per source sample */
#define RS_MAXSRC (NUM_SAMPLES_PER_FRAME * 4 + 2)

/* Double phase keeps stream position independent of 1- vs 32-sample calls. */
typedef struct { float b[2][RS_MAXSRC + 64]; int len; double pos; } VpResampler;
static VpResampler s_rs[MCPX_HW_MAX_VOICES];
static float s_rs_kernel[2 * RS_HALF * RS_PH + 2];
static int s_rs_ready;

static void rs_kernel_init(void)
{
    const double pi = 3.14159265358979323846;
    int i, n = 2 * RS_HALF * RS_PH;
    for (i = 0; i <= n; i++) {
        double u = (double)i / RS_PH - RS_HALF;                       /* -8 .. 8 source samples */
        double sinc = fabs(u) < 1e-9 ? 1.0 : sin(pi * u) / (pi * u);
        double w = 0.35875 + 0.48829 * cos(pi * u / RS_HALF) + 0.14128 * cos(2.0 * pi * u / RS_HALF)
                 + 0.01168 * cos(3.0 * pi * u / RS_HALF);
        s_rs_kernel[i] = (float)(sinc * w);
    }
    s_rs_kernel[n + 1] = 0.0f;
    s_rs_ready = 1;
}

static inline float rs_k(float u)                                     /* kernel at u source samples from the centre */
{
    float p = (u + RS_HALF) * RS_PH, fr;
    int i;
    if (p < 0.0f || p >= (float)(2 * RS_HALF * RS_PH)) return 0.0f;
    i = (int)p;
    fr = p - (float)i;
    return s_rs_kernel[i] + fr * (s_rs_kernel[i + 1] - s_rs_kernel[i]);
}

static int voice_resample(MCPXAPUState *d, uint16_t v, float samples[][2],
                          int requested_num, float rate)
{
    MCPXAPUVoiceFilter *f = &d->vp.filters[v];
    VpResampler *r = &s_rs[v];
    float src[RS_MAXSRC][2];
    float step, fc, coef[RS_TAPS];
    int need, got, out, i, j, ch;

    if (!s_rs_ready) rs_kernel_init();
    if (!(rate > 0.0f)) rate = 1.0f;
    step = 1.0f / rate;
    fc = step > 1.0f ? 1.0f / step : 1.0f;

    if (!f->rs_have) {                      /* a new play of the voice: silence in front of its first sample */
        memset(r, 0, sizeof *r);
        r->len = RS_HALF;
        r->pos = (float)RS_HALF;
        f->rs_have = 1;
    }

    /* Source samples to add so the last requested output has its 8 look-ahead samples. A step above about 4 (a voice
     * pitched more than two octaves up) wants more than the buffer holds: make fewer outputs rather than lose source. */
    for (;;) {
        need = (int)floor(r->pos + (double)(requested_num - 1) * step) + RS_HALF + 1 - r->len;
        if (need <= RS_MAXSRC || requested_num <= 1) break;
        requested_num /= 2;
    }
    if (need > RS_MAXSRC) need = RS_MAXSRC;
    if (need < 0) need = 0;

    got = need > 0 ? voice_get_samples(d, v, src, need) : 0;
    if (got < 0) got = 0;
    for (i = 0; i < got && r->len < (int)(sizeof r->b[0] / sizeof r->b[0][0]); i++, r->len++) {
        r->b[0][r->len] = src[i][0];
        r->b[1][r->len] = src[i][1];
    }

    for (out = 0; out < requested_num; out++) {
        int k0 = (int)floor(r->pos);
        float sum = 0.0f, inv, a0 = 0.0f, a1 = 0.0f;
        if (k0 + RS_HALF >= r->len) break;                    /* the look-ahead is not there (yet) */
        for (j = 0; j < RS_TAPS; j++) {
            float x = (float)(k0 - (RS_HALF - 1) + j) - r->pos;
            coef[j] = fc * rs_k(x * fc);
            sum += coef[j];
        }
        inv = fabsf(sum) > 0.1f ? 1.0f / sum : 1.0f;
        for (j = 0; j < RS_TAPS; j++) {
            a0 += coef[j] * r->b[0][k0 - (RS_HALF - 1) + j];
            a1 += coef[j] * r->b[1][k0 - (RS_HALF - 1) + j];
        }
        samples[out][0] = a0 * inv;
        samples[out][1] = a1 * inv;
        r->pos += step;
    }

    {   /* drop what no later output needs */
        int drop = (int)floor(r->pos) - (RS_HALF - 1);
        if (drop > r->len) drop = r->len;
        if (drop > 0) {
            for (ch = 0; ch < 2; ch++)
                memmove(r->b[ch], r->b[ch] + drop, (size_t)(r->len - drop) * sizeof(float));
            r->len -= drop;
            r->pos -= (float)drop;
        }
    }

    if (out == 0 && got == 0) {
        /* Nothing new and nothing left to interpolate: the voice has stopped or its stream is empty. voice_process
         * treats 0 as an underrun and a negative as the end; the voice's active bit says which. */
        int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
        return active ? 0 : -1;
    }
    return out;
}
