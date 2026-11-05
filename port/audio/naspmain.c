// The audio RSP task as C: runs n_audio's command list into stereo s16 PCM. Presentation only.
#include <string.h>
#include <stdint.h>
#include "port.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

enum { A_SPNOOP, A_ADPCM, A_CLEARBUFF, A_ENVMIXER, A_LOADBUFF, A_RESAMPLE, A_SAVEBUFF, A_SEGMENT, A_SETBUFF, A_SETVOL, A_DMEMMOVE, A_LOADADPCM, A_MIXER, A_INTERLEAVE, A_POLEF, A_SETLOOP };
enum { MAIN_L = 1248, MAIN_R = 1616, AUX_L = 1984, AUX_R = 2352, N_BYTES = 368, N_SAMP = 184 };

static uint8_t D[4096 + 512];
static int16_t sBook[512 / 2 * 4];
static int32_t sLoop;
static int16_t sLut[64 * 4];
static int sLutLoaded;
static float sLtgt, sLrate, sRtgt, sRrate, sLvol, sRvol, sDry, sWet;

extern const uint8_t *port_rom_ptr(uint32_t off);
extern uint32_t port_resample_lut_rom(void);

static inline int16_t *S16(int a) { return (int16_t *)(D + (a & 0xFFF)); }
static inline int16_t clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

static void adpcm(int flags, int count, int align, int out, int32_t state)
{
    int16_t last[16];
    int16_t *dst = S16(out);
    const uint8_t *in = D + align;
    int i;

    if (flags & 1) memset(last, 0, sizeof last);
    else if (flags & 2) memcpy(last, (void *)(uintptr_t)sLoop, sizeof last);
    else memcpy(last, (void *)(uintptr_t)state, sizeof last);
    memcpy(dst, last, sizeof last);
    dst += 16;
    while (count > 0) {
        int code = *in++;
        int scale = code >> 4, rshift = scale < 12 ? 12 - scale : 0;
        const int16_t *b1 = sBook + (code & 15) * 16, *b2 = b1 + 8;
        int16_t fr[16];
        int k, j, seg;
        for (k = 0; k < 8; k++) {
            int b = *in++;
            fr[k * 2] = (int16_t)((b & 0xF0) << 8) >> rshift;
            fr[k * 2 + 1] = (int16_t)((b & 0x0F) << 12) >> rshift;
        }
        for (seg = 0; seg < 2; seg++) {
            int l1 = seg ? dst[6] : last[14], l2 = seg ? dst[7] : last[15];
            int16_t *o = dst + seg * 8;
            for (i = 0; i < 8; i++) {
                int32_t a = (int32_t)fr[seg * 8 + i] << 11;
                a += b1[i] * l1 + b2[i] * l2;
                for (j = 0; j < i; j++) a += b2[i - 1 - j] * fr[seg * 8 + j]; // the book holds impulse responses: residuals, not outputs
                o[i] = clamp16(a >> 11);
            }
        }
        memcpy(last, dst, sizeof last);
        dst += 16;
        count -= 32;
    }
    memcpy((void *)(uintptr_t)state, last, sizeof last);
}

static void resample(int flags, uint32_t pitch, int in, int out, int count, int32_t state)
{
    int16_t st[8];
    int16_t *o = S16(out);
    int ip = (in & 0xFFF) - 8; // four samples of history live just before the input
    uint32_t acc = 0;
    int i, k, n = count / 2;

    if (flags & 1) { memset(D + (ip & 0xFFF), 0, 8); }
    else { memcpy(st, (void *)(uintptr_t)state, sizeof st); memcpy(D + (ip & 0xFFF), st, 8); acc = (uint16_t)st[4]; }
    for (i = 0; i < n; i++) {
        const int16_t *lut = sLut + ((acc >> 10) & 63) * 4;
        int32_t a = 0;
        for (k = 0; k < 4; k++) a += (*S16(ip + k * 2) * lut[k]) >> 15;
        o[i] = clamp16(a);
        acc += pitch;
        ip += (acc >> 16) * 2;
        acc &= 0xFFFF;
    }
    memcpy(st, D + (ip & 0xFFF), 8);
    st[4] = (int16_t)acc;
    memcpy((void *)(uintptr_t)state, st, sizeof st);
}

// n_audio volume rates are linear: a 16.16 step added every 8 samples, stopping at the target.
static inline float ramp(float v, float tgt, float rate)
{
    float n = v + rate;
    if (rate > 0) return n > tgt ? tgt : n;
    if (rate < 0) return n < tgt ? tgt : n;
    return v;
}

static void envmix(int flags, int16_t cvolr, int32_t state)
{
    int16_t *in = S16(0);
    int16_t *ol = S16(MAIN_L), *or_ = S16(MAIN_R), *al = S16(AUX_L), *ar = S16(AUX_R);
    float st[8];
    int i, k;

    if (flags & 1) { sRvol = (float)cvolr; st[0] = sLvol; }
    else {
        memcpy(st, (void *)(uintptr_t)state, sizeof st);
        sLvol = st[0]; sRvol = st[1]; sLtgt = st[2]; sRtgt = st[3]; sLrate = st[4]; sRrate = st[5]; sDry = st[6]; sWet = st[7];
    }
    for (i = 0; i < N_SAMP; i += 8) {
        float nl = ramp(sLvol, sLtgt, sLrate), nr = ramp(sRvol, sRtgt, sRrate);
        for (k = 0; k < 8; k++) {
            float t = (k + 1) / 8.0f;
            float vl = sLvol + (nl - sLvol) * t, vr = sRvol + (nr - sRvol) * t;
            int s = in[i + k];
            int L = (int)(s * vl) >> 15, R = (int)(s * vr) >> 15;
            ol[i + k] = clamp16(ol[i + k] + (int)(L * sDry) / 32768);
            or_[i + k] = clamp16(or_[i + k] + (int)(R * sDry) / 32768);
            al[i + k] = clamp16(al[i + k] + (int)(L * sWet) / 32768);
            ar[i + k] = clamp16(ar[i + k] + (int)(R * sWet) / 32768);
        }
        sLvol = nl; sRvol = nr;
    }
    st[0] = sLvol; st[1] = sRvol; st[2] = sLtgt; st[3] = sRtgt; st[4] = sLrate; st[5] = sRrate; st[6] = sDry; st[7] = sWet;
    memcpy((void *)(uintptr_t)state, st, sizeof st);
}

static void polef(int flags, int gain, int buf, int32_t state)
{
    int16_t st[4], *b = S16(buf);
    int i;
    int16_t l1, l2;
    if (flags & 1) { l1 = l2 = 0; } else { memcpy(st, (void *)(uintptr_t)state, 8); l1 = st[2]; l2 = st[3]; }
    for (i = 0; i < N_SAMP; i++) {
        int32_t a = (b[i] * gain) >> 15;
        a += (sBook[0] * l1 + sBook[8] * l2) >> 11;
        l2 = l1; l1 = clamp16(a); b[i] = l1;
    }
    st[0] = st[1] = 0; st[2] = l1; st[3] = l2;
    memcpy((void *)(uintptr_t)state, st, 8);
}

void port_audio_run_list(const uint32_t *c, int n)
{
    int i;
    if (!sLutLoaded) {
        const uint8_t *p = port_rom_ptr(port_resample_lut_rom());
        for (i = 0; i < 64 * 4; i++) sLut[i] = (int16_t)((p[i * 2] << 8) | p[i * 2 + 1]);
        sLutLoaded = 1;
    }
    for (i = 0; i < n; i++, c += 2) {
        uint32_t w0 = c[0], w1 = c[1];
        switch (w0 >> 24) {
        case A_ADPCM: adpcm(w1 >> 28, (w1 >> 16) & 0xFFF, (w1 >> 12) & 15, w1 & 0xFFF, w0 & 0xFFFFFF); break;
        case A_CLEARBUFF: memset(D + (w0 & 0xFFF), 0, w1 & 0xFFFF); break;
        case A_ENVMIXER: envmix((w0 >> 16) & 0xFF, (int16_t)(w0 & 0xFFFF), w1); break;
        case A_LOADBUFF: memcpy(D + (w0 & 0xFFF), (void *)(uintptr_t)w1, (w0 >> 12) & 0xFFF); break;
        case A_SAVEBUFF: memcpy((void *)(uintptr_t)w1, D + (w0 & 0xFFF), (w0 >> 12) & 0xFFF); break;
        case A_RESAMPLE: resample(w1 >> 30, ((w1 >> 14) & 0xFFFF) << 1, (w1 >> 2) & 0xFFF, (w1 & 3) * 368, N_BYTES, w0 & 0xFFFFFF); break;
        case A_SETVOL: {
            int f = (w0 >> 16) & 0xFF, v = (int16_t)(w0 & 0xFFFF);
            int hi = (int16_t)(w1 >> 16), lo = w1 & 0xFFFF;
            float rate = (float)hi + (float)lo / 65536.0f;
            if (!(f & 4)) { sLtgt = (float)v; sLrate = rate; }
            else if (f & 2) { sLvol = (float)v; sDry = (float)hi; sWet = (float)(int16_t)lo; }
            else { sRtgt = (float)v; sRrate = rate; }
            break; }
        case A_DMEMMOVE: memmove(D + ((w1 >> 16) & 0xFFF), D + (w0 & 0xFFF), w1 & 0xFFFF); break;
        case A_LOADADPCM: { int cnt = w0 & 0xFFFFFF; if (cnt > (int)sizeof sBook) cnt = sizeof sBook; memcpy(sBook, (void *)(uintptr_t)w1, cnt); break; }
        case A_MIXER: {
            int gain = (int16_t)(w0 & 0xFFFF), k;
            int16_t *in = S16(w1 >> 16), *o = S16(w1 & 0xFFFF);
            for (k = 0; k < N_SAMP; k++) o[k] = clamp16(o[k] + ((in[k] * gain) >> 15));
            break; }
        case A_INTERLEAVE: {
            int16_t tmp[N_SAMP * 2], *l = S16(MAIN_L), *r = S16(MAIN_R); int k;
            for (k = 0; k < N_SAMP; k++) { tmp[k * 2] = l[k]; tmp[k * 2 + 1] = r[k]; }
            memcpy(D, tmp, sizeof tmp);
            break; }
        case A_POLEF: polef((w0 >> 16) & 0xFF, (int16_t)(w0 & 0xFFFF), (w1 >> 24) * 368, w1 & 0xFFFFFF); break;
        case A_SETLOOP: sLoop = w1; break;
        default: break;
        }
    }
}

// ---- output ring: stereo s16, read by the page ----
#define RING_FRAMES (1 << 16)
static int16_t sRing[RING_FRAMES * 2];
static uint32_t sHead, sConsumed;
EXPORT int16_t *AudioRing(void) { return sRing; }
EXPORT uint32_t AudioRingFrames(void) { return RING_FRAMES; }
EXPORT uint32_t AudioHead(void) { return sHead; }
EXPORT void AudioConsumed(uint32_t tail) { sConsumed = tail; }

int osAiSetNextBuffer(void *buf, unsigned int nbytes)
{
    const int16_t *in = buf;
    unsigned int n = nbytes / 4, i;
    for (i = 0; i < n; i++) { uint32_t p = (sHead + i) & (RING_FRAMES - 1); sRing[p * 2] = in[i * 2]; sRing[p * 2 + 1] = in[i * 2 + 1]; }
    sHead += n;
    return 0;
}
unsigned int osAiSetFrequency(unsigned int f) { return f; }
int port_audio_queued_samples(void)
{
    int q = (int)(sHead - sConsumed) - 533;
    return q < 0 ? 0 : q;
}
