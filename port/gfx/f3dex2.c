// F3DEX2 display list interpreter. Reads what the game's own RSP/RDP would have read and draws it
// through a small backend. Render only: nothing here writes game state.
#include "port.h"
#include "gfx.h"
#include "port_prof.h"
void *memset(void *, int, unsigned long);
void *memcpy(void *, const void *, unsigned long);
float sqrtf(float);
void *malloc(unsigned long);
void free(void *);

typedef struct { uint32_t w0, w1; } Cmd;
typedef struct { float x, y, z, w, u, v; uint8_t r, g, b, a; int16_t ob[3], tc[2]; float ls, lt; } Vert; // ob/tc/ls/lt: raw source, for the trace only
typedef struct { int16_t ob[3]; uint16_t flag; int16_t tc[2]; uint8_t cn[4]; } N64Vtx;

enum {
    OP_VTX = 0x01, OP_MODIFYVTX = 0x02, OP_CULLDL = 0x03, OP_BRANCH_Z = 0x04, OP_TRI1 = 0x05, OP_TRI2 = 0x06, OP_QUAD = 0x07,
    OP_TEXTURE = 0xD7, OP_POPMTX = 0xD8, OP_GEOMETRYMODE = 0xD9, OP_MTX = 0xDA, OP_MOVEWORD = 0xDB, OP_MOVEMEM = 0xDC,
    OP_DL = 0xDE, OP_ENDDL = 0xDF, OP_RDPHALF_1 = 0xE1, OP_OTHERMODE_L = 0xE2, OP_OTHERMODE_H = 0xE3, OP_TEXRECT = 0xE4,
    OP_TEXRECTFLIP = 0xE5, OP_LOADTLUT = 0xF0, OP_RDPHALF_2 = 0xF1, OP_SETTILESIZE = 0xF2, OP_LOADBLOCK = 0xF3, OP_LOADTILE = 0xF4,
    OP_SETTILE = 0xF5, OP_FILLRECT = 0xF6, OP_FILLCOLOR = 0xF7, OP_FOGCOLOR = 0xF8, OP_BLENDCOLOR = 0xF9, OP_PRIMCOLOR = 0xFA,
    OP_ENVCOLOR = 0xFB, OP_COMBINE = 0xFC, OP_SETTIMG = 0xFD, OP_SETZIMG = 0xFE, OP_SETCIMG = 0xFF, OP_PRIMDEPTH = 0xEE, OP_SCISSOR = 0xED, OP_SPECIAL_1 = 0xD5,
};
enum { G_ZBUFFER_ = 1, G_SHADE_ = 4, G_CULL_FRONT_ = 0x200, G_CULL_BACK_ = 0x400, G_FOG_ = 0x10000, G_LIGHTING_ = 0x20000, G_TEXGEN_ = 0x40000, G_TEXGENLIN_ = 0x80000, G_SMOOTH_ = 0x200000 };

typedef struct { int fmt, siz, line, tmem, pal, cms, cmt, masks, maskt, shifts, shiftt; int uls, ult, lrs, lrt; } Tile;
typedef struct { const uint8_t *img; int stride; int siz; int fmt; int uls, ult; int shuffled; } Loaded;

static struct {
    float mv[10][4][4], proj[4][4], mvp[4][4]; int mvtop;
    int32_t mvpfix[16]; // the RSP's copy of the MVP in s15.16, which G_MW_MATRIX writes into
    Vert vtx[64];
    uint32_t geom, omh, oml;
    uint32_t seg[16];
    uint32_t combhi, comblo;
    float prim[4], env[4], fogc[4], blendc[3], blenda, primlod; uint32_t fillc;
    float primz;
    int texon; float texs, texT; int texlevel, textile;
    Tile tile[8]; Loaded tm[512];
    uint8_t *timg; int timgfmt, timgsiz, timgw;
    uint8_t tlut[512];
    int numlights; uint8_t lightcol[8][3]; int8_t lightdir[8][3]; int8_t lookat[2][3];
    float fogm, fogo;
    int vp[4]; int sc[4];
    int cimg_main, cimg_depth; const void *zimg;
    uint32_t half1, half2;
    float lightnorm[8][3];
} S;

static uint32_t sOps, sTris, sRects;
#include <emscripten.h>
// ---- widescreen (proof/widescreen-spec.md): render only, the sim never sees any of it ----
// k = (4/3) / (canvas aspect), 1 at 4:3. 3D under a full-picture viewport has clip x scaled by k, so the
// band in the middle is the N64 picture and the sides show more stage. Anything narrower stays in the band.
static float sK = 1.0f;
#define WIDE_SPAN 300
EMSCRIPTEN_KEEPALIVE void PortSetCanvas(int w, int h)
{
    float k = (w > 0 && h > 0) ? (4.0f / 3.0f) / ((float)w / (float)h) : 1.0f;
    if (k > 1.0f) k = 1.0f;
    if (k < 4.0f / 7.0f) k = 4.0f / 7.0f;
    sK = k;
    port_set_canvas_size(w, h);
}
// The magnifying glass: a fighter in the side bands is on the screen. At k = 1 this is the game's own test.
extern int gmCameraCheckTargetInBounds(float x, float y);
int port_gfx_in_wide_frame(float x, float y) { return gmCameraCheckTargetInBounds(x * sK, y); }
static uint32_t sBad[4]; // vertex loads past the buffer, too many lights: counted, then clamped
#include <emscripten.h>
// Debug trace: for the next N frames, one line per distinct draw state with its screen box. Render only.
int printf(const char *, ...);
static int sTrace;
EMSCRIPTEN_KEEPALIVE void GfxTrace(int frames) { sTrace = frames; }
// Coverage: every distinct draw state the game used, with a count. Dumped on request, render only.
typedef struct { uint32_t hi, lo, oml, omh, geom; uint16_t tex; uint8_t rect, used; uint32_t n; } Cov;
#define COVN 4096
static int sCovOn;
EMSCRIPTEN_KEEPALIVE void GfxCoverage(int on) { sCovOn = on; }
static Cov sCov[COVN]; static uint32_t sCovLost, sMwHist[16], sDefault[256];
static void cover(const RbState *st, int rect)
{
    uint32_t omh = S.omh & 0x00F03000u, geom = S.geom & (G_ZBUFFER_ | G_SHADE_ | G_FOG_ | G_LIGHTING_ | G_TEXGEN_);
    uint16_t tex = st->tex[0].key ? (uint16_t)(0x100 | S.tile[S.textile].fmt << 4 | S.tile[S.textile].siz) : 0;
    uint32_t h = (st->combine_hi * 31u + st->combine_lo) * 2654435761u ^ (S.oml * 40503u) ^ omh ^ geom ^ tex ^ (uint32_t)rect;
    int i, k;
    for (k = 0; k < 64; k++) {
        Cov *c = &sCov[(h + k) & (COVN - 1)];
        if (!c->used) { c->used = 1; c->hi = st->combine_hi; c->lo = st->combine_lo; c->oml = S.oml; c->omh = omh; c->geom = geom; c->tex = tex; c->rect = rect; c->n = 1; return; }
        if (c->hi == st->combine_hi && c->lo == st->combine_lo && c->oml == S.oml && c->omh == omh && c->geom == geom && c->tex == tex && c->rect == rect) { c->n++; return; }
    }
    (void)i; sCovLost++;
}
EMSCRIPTEN_KEEPALIVE void GfxCoverageDump(void)
{
    int i;
    for (i = 0; i < COVN; i++) if (sCov[i].used)
        printf("COV %s cc=%06x:%08x oml=%08x omh=%08x geom=%06x tex=%03x n=%u\n", sCov[i].rect ? "rect" : "tri", sCov[i].hi & 0xFFFFFF, sCov[i].lo, sCov[i].oml, sCov[i].omh, sCov[i].geom, sCov[i].tex, sCov[i].n);
    for (i = 0; i < 16; i++) if (sMwHist[i]) printf("COVMW %d %u\n", i, sMwHist[i]);
    for (i = 0; i < 256; i++) if (sDefault[i]) printf("COVOP %02x %u\n", i, sDefault[i]);
    printf("COVBAD vtx=%u lights=%u\n", sBad[0], sBad[1]);
    printf("COVEND lost=%u\n", sCovLost);
}
static void trace_draw(const char *kind, const RbState *st, const RbVertex *v, int n)
{
    if (sCovOn) cover(st, kind[0] == 'r');
    float x0 = 9, y0 = 9, x1 = -9, y1 = -9, u0 = 1e9, u1 = -1e9, t0 = 1e9, t1 = -1e9; int i;
    if (!sTrace) return;
    for (i = 0; i < n; i++) { if (v[i].u < u0) u0 = v[i].u; if (v[i].u > u1) u1 = v[i].u; if (v[i].v < t0) t0 = v[i].v; if (v[i].v > t1) t1 = v[i].v; }
    { const Tile *T = &S.tile[S.textile]; printf("GTT texs=%.4f,%.4f tile=%d tmem=%d line=%d cms=%d cmt=%d masks=%d maskt=%d shifts=%d shiftt=%d uls=%d ult=%d lrs=%d lrt=%d clamp=%d,%d uv=%.1f..%.1f,%.1f..%.1f\n", S.texs, S.texT, S.textile, T->tmem, T->line, T->cms, T->cmt, T->masks, T->maskt, T->shifts, T->shiftt, T->uls, T->ult, T->lrs, T->lrt, st->tex[0].clamp_s, st->tex[0].clamp_t, u0, u1, t0, t1); }
    for (i = 0; i < n; i++) { float w = v[i].w > 0 ? v[i].w : 1, x = v[i].x / w, y = v[i].y / w; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    printf("GT %s cc=%06x:%08x oml=%08x omh=%08x geom=%06x t0=%d/%d %dx%d t1=%dx%d prim=%.2f,%.2f,%.2f,%.2f env=%.2f,%.2f,%.2f,%.2f bl=%05x/%d%d%d%d ac=%d edge=%d 2c=%d dt=%d dw=%d vca=%.2f box=%.2f,%.2f,%.2f,%.2f\n",
        kind, st->combine_hi & 0xFFFFFF, st->combine_lo, S.oml, S.omh, S.geom, S.tile[S.textile].fmt, S.tile[S.textile].siz, st->tex[0].w, st->tex[0].h, st->tex[1].w, st->tex[1].h,
        st->prim[0], st->prim[1], st->prim[2], st->prim[3], st->env[0], st->env[1], st->env[2], st->env[3], st->blender, st->blend, st->bsrc, st->bdst, st->color_off, st->alpha_compare, st->tex_edge, st->two_cycle, st->depth_test, st->depth_write, v[0].a, x0, y0, x1, y1);
}
EMSCRIPTEN_KEEPALIVE uint32_t port_gfx_tris(void) { return sTris; }
uint32_t port_gfx_rects(void) { return sRects; }
uint32_t port_gfx_ops(void) { return sOps; }
static uint32_t sOpHist[256];
uint32_t *port_gfx_histogram(void) { return sOpHist; }

static void *seg(uint32_t a)
{
    uint32_t s = (a >> 24) & 0xF;
    if (a >= 0x01000000u && s != 0) return (void *)(uintptr_t)(S.seg[s] + (a & 0xFFFFFF));
    return (void *)(uintptr_t)a;
}

static void mat_mul(float r[4][4], float a[4][4], float b[4][4])
{
    float t[4][4]; int i, j;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) t[i][j] = a[i][0]*b[0][j] + a[i][1]*b[1][j] + a[i][2]*b[2][j] + a[i][3]*b[3][j];
    memcpy(r, t, sizeof t);
}

// Mtx: four s32 rows of integer halves, then four rows of fraction halves.
static void mat_load(float o[4][4], const int32_t *m)
{
    int r, c;
    for (r = 0; r < 4; r++) for (c = 0; c < 4; c++) {
        // 16 words: rows of two words of integer halves, then the same of fractions
        uint32_t i = (uint32_t)m[r * 2 + c / 2];
        uint32_t f = (uint32_t)m[8 + r * 2 + c / 2];
        int ip = (int16_t)((c & 1) ? (i & 0xFFFF) : (i >> 16));
        uint32_t fp = (c & 1) ? (f & 0xFFFF) : (f >> 16);
        o[r][c] = (float)ip + (float)fp / 65536.0f;
    }
}

static void recompute_mvp(void)
{
    int i;
    mat_mul(S.mvp, S.mv[S.mvtop], S.proj);
    for (i = 0; i < 16; i++) S.mvpfix[i] = (int32_t)(S.mvp[i / 4][i % 4] * 65536.0f);
}
// G_MW_MATRIX: one word of the MVP, two s15.16 halves (integer parts from 0x00, fractions from 0x20).
// The game's billboards (fireballs, sparks, most effects) build their rows this way after gSPMvpRecalc.
static void mvp_word(int off, uint32_t w)
{
    int e = (off & 0x1F) / 2, k;
    if (off < 0 || off > 0x3C || (off & 3)) return;
    for (k = 0; k < 2; k++) {
        uint32_t half = k ? (w & 0xFFFF) : (w >> 16), v = (uint32_t)S.mvpfix[e + k];
        v = off < 0x20 ? ((half << 16) | (v & 0xFFFF)) : ((v & 0xFFFF0000u) | half);
        S.mvpfix[e + k] = (int32_t)v;
        S.mvp[(e + k) / 4][(e + k) % 4] = (int32_t)v / 65536.0f;
    }
}

static void normalize3(float *v)
{
    float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

static void do_vtx(const N64Vtx *in, int n, int dst)
{
    int i, k;
    float (*m)[4] = S.mv[S.mvtop];
    float ln[8][3], la[2][3];
    int lit = (S.geom & G_LIGHTING_) != 0;
    int gen = (S.geom & G_TEXGEN_) != 0;

    if (lit) {
        for (k = 0; k < S.numlights; k++) {
            float d[3] = { S.lightdir[k][0] / 127.0f, S.lightdir[k][1] / 127.0f, S.lightdir[k][2] / 127.0f };
            ln[k][0] = d[0]*m[0][0] + d[1]*m[0][1] + d[2]*m[0][2];
            ln[k][1] = d[0]*m[1][0] + d[1]*m[1][1] + d[2]*m[1][2];
            ln[k][2] = d[0]*m[2][0] + d[1]*m[2][1] + d[2]*m[2][2];
            normalize3(ln[k]);
        }
        for (k = 0; k < 2; k++) {
            float d[3] = { S.lookat[k][0] / 127.0f, S.lookat[k][1] / 127.0f, S.lookat[k][2] / 127.0f };
            la[k][0] = d[0]*m[0][0] + d[1]*m[0][1] + d[2]*m[0][2];
            la[k][1] = d[0]*m[1][0] + d[1]*m[1][1] + d[2]*m[1][2];
            la[k][2] = d[0]*m[2][0] + d[1]*m[2][1] + d[2]*m[2][2];
            normalize3(la[k]);
        }
    }
    for (i = 0; i < n; i++) {
        const N64Vtx *v = &in[i];
        Vert *d = &S.vtx[(dst + i) & 63];
        float x = v->ob[0], y = v->ob[1], z = v->ob[2];
        float U = v->tc[0] * S.texs, V = v->tc[1] * S.texT;

        d->x = x*S.mvp[0][0] + y*S.mvp[1][0] + z*S.mvp[2][0] + S.mvp[3][0];
        d->y = x*S.mvp[0][1] + y*S.mvp[1][1] + z*S.mvp[2][1] + S.mvp[3][1];
        d->z = x*S.mvp[0][2] + y*S.mvp[1][2] + z*S.mvp[2][2] + S.mvp[3][2];
        d->w = x*S.mvp[0][3] + y*S.mvp[1][3] + z*S.mvp[2][3] + S.mvp[3][3];
        if (lit) {
            float n3[3] = { (int8_t)v->cn[0] / 127.0f, (int8_t)v->cn[1] / 127.0f, (int8_t)v->cn[2] / 127.0f };
            int r = S.lightcol[S.numlights][0], g = S.lightcol[S.numlights][1], b = S.lightcol[S.numlights][2];
            for (k = 0; k < S.numlights; k++) {
                float t = n3[0]*ln[k][0] + n3[1]*ln[k][1] + n3[2]*ln[k][2];
                if (t > 0) { r += (int)(t * S.lightcol[k][0]); g += (int)(t * S.lightcol[k][1]); b += (int)(t * S.lightcol[k][2]); }
            }
            d->r = r > 255 ? 255 : r; d->g = g > 255 ? 255 : g; d->b = b > 255 ? 255 : b;
            if (gen) {
                float dx = n3[0]*la[0][0] + n3[1]*la[0][1] + n3[2]*la[0][2];
                float dy = n3[0]*la[1][0] + n3[1]*la[1][1] + n3[2]*la[1][2];
                U = (dx * 512.0f + 512.0f) * 32.0f * S.texs;
                V = (dy * 512.0f + 512.0f) * 32.0f * S.texT;
                if (sTrace && i == 0) printf("GTG lookat=%d,%d,%d/%d,%d,%d la=%.2f,%.2f,%.2f/%.2f,%.2f,%.2f n=%.2f,%.2f,%.2f dx=%.3f dy=%.3f texs=%.4f,%.4f U=%.1f V=%.1f geom=%06x\n",
                    S.lookat[0][0], S.lookat[0][1], S.lookat[0][2], S.lookat[1][0], S.lookat[1][1], S.lookat[1][2], la[0][0], la[0][1], la[0][2], la[1][0], la[1][1], la[1][2], n3[0], n3[1], n3[2], dx, dy, S.texs, S.texT, U / 32, V / 32, S.geom); if (sTrace && i == 0) printf("GTN numlights=%d col0=%d,%d,%d dir0=%d,%d,%d amb=%d,%d,%d shade=%d,%d,%d\n", S.numlights, S.lightcol[0][0], S.lightcol[0][1], S.lightcol[0][2], S.lightdir[0][0], S.lightdir[0][1], S.lightdir[0][2], S.lightcol[S.numlights][0], S.lightcol[S.numlights][1], S.lightcol[S.numlights][2], d->r, d->g, d->b);
            }
        } else { d->r = v->cn[0]; d->g = v->cn[1]; d->b = v->cn[2]; }
        d->a = v->cn[3];
        if (S.geom & G_FOG_) {
            float w = d->w > 0.0001f ? d->w : 0.0001f;
            float f = (d->z / w) * S.fogm + S.fogo;
            d->a = f < 0 ? 0 : f > 255 ? 255 : (uint8_t)f;
        }
        d->u = U; d->v = V;
        d->ob[0] = v->ob[0]; d->ob[1] = v->ob[1]; d->ob[2] = v->ob[2]; d->tc[0] = v->tc[0]; d->tc[1] = v->tc[1]; d->ls = S.texs; d->lt = S.texT;
    }
}

// ---- textures ----
#define TEXCACHE_BITS 12
typedef struct TexEntry { uint32_t key; int w, h; uint8_t *rgba; uint32_t used; struct TexEntry *next; } TexEntry;
static TexEntry *sTexCache[1 << TEXCACHE_BITS];
typedef struct { const uint8_t *img; int n, fmt, siz, w, h, pal; uint32_t tlut, frame, key; } Memo;
#define MEMO_N 1024
static Memo sMemo[MEMO_N];
static uint32_t sFrameGen, sTlutGen; // the frame being drawn; bumps with every palette load

// Texel content hash, once per texture per frame. Four 32-bit lanes over 16-byte steps (the xxHash32
// round): the byte-at-a-time FNV it replaces was most of the interpreter's time (CPU profile, node runner).
static uint32_t rotl(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
static uint32_t fnv(uint32_t h, const uint8_t *p, int n)
{
    const uint32_t P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u;
    uint32_t a = h + P1 + P2, b = h + P2, c = h, d = h - P1, w[4];
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        __builtin_memcpy(w, p + i, 16);
        a = rotl(a + w[0] * P2, 13) * P1; b = rotl(b + w[1] * P2, 13) * P1;
        c = rotl(c + w[2] * P2, 13) * P1; d = rotl(d + w[3] * P2, 13) * P1;
    }
    h = rotl(a, 1) + rotl(b, 7) + rotl(c, 12) + rotl(d, 18) + (uint32_t)n;
    for (; i < n; i++) h = rotl(h + p[i] * 374761393u, 11) * P1;
    h ^= h >> 15; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
    return h;
}
static uint8_t x5(int v) { return (v << 3) | (v >> 2); }

static void decode(uint8_t *o, const Loaded *L, const Tile *T, int w, int h)
{
    int x, y;
    int fmt = T->fmt, siz = T->siz;
    const uint8_t *img = L->img;
    int stride = L->stride;
    for (y = 0; y < h; y++) {
        const uint8_t *row = img + y * stride;
        // A block loaded with dxt 0 holds odd rows with the 32-bit words of each 64-bit word swapped.
        int xr = (L->shuffled && (y & 1)) ? 4 : 0;
        for (x = 0; x < w; x++, o += 4) {
            int r = 255, g = 255, b = 255, a = 255, t;
            switch (siz) {
            case 0: // 4b
                t = (row[(x >> 1) ^ xr] >> ((x & 1) ? 0 : 4)) & 0xF;
                if (fmt == 2) { const uint8_t *e = S.tlut + (T->pal * 16 + t) * 2; int c = (e[0] << 8) | e[1]; r = x5(c >> 11); g = x5((c >> 6) & 31); b = x5((c >> 1) & 31); a = (c & 1) ? 255 : 0; }
                else if (fmt == 3) { int i = t >> 1; r = g = b = (i << 5) | (i << 2) | (i >> 1); a = (t & 1) ? 255 : 0; }
                else { r = g = b = a = (t << 4) | t; }
                break;
            case 1: // 8b
                t = row[x ^ xr];
                if (fmt == 2) { const uint8_t *e = S.tlut + t * 2; int c = (e[0] << 8) | e[1]; r = x5(c >> 11); g = x5((c >> 6) & 31); b = x5((c >> 1) & 31); a = (c & 1) ? 255 : 0; }
                else if (fmt == 3) { int i = t >> 4; r = g = b = (i << 4) | i; i = t & 15; a = (i << 4) | i; }
                else { r = g = b = a = t; }
                break;
            case 2: // 16b
                { int c = (row[(x * 2) ^ xr] << 8) | row[((x * 2) ^ xr) + 1];
                  if (fmt == 3) { r = g = b = c >> 8; a = c & 0xFF; }
                  else { r = x5(c >> 11); g = x5((c >> 6) & 31); b = x5((c >> 1) & 31); a = (c & 1) ? 255 : 0; } }
                break;
            default: // 32b
                r = row[x * 4]; g = row[x * 4 + 1]; b = row[x * 4 + 2]; a = row[x * 4 + 3];
            }
            o[0] = r; o[1] = g; o[2] = b; o[3] = a;
        }
    }
}

static void import_tile(RbTexture *out, int t)
{
    const Tile *T = &S.tile[t];
    const Loaded *L = &S.tm[T->tmem & 511];
    int w, h, n; uint32_t key; TexEntry *e;

    memset(out, 0, sizeof *out);
    if (L->img == NULL) return;
    w = ((T->lrs - T->uls) >> 2) + 1; h = ((T->lrt - T->ult) >> 2) + 1;
    if (T->masks && w > (1 << T->masks)) w = 1 << T->masks;
    if (T->maskt && h > (1 << T->maskt)) h = 1 << T->maskt;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return;
    Loaded adj = *L;
    if (T->line && adj.stride == 0) adj.stride = T->line * (T->siz == 3 ? 16 : 8);
    if (adj.stride == 0) adj.stride = T->siz == 0 ? (w + 1) / 2 : w << (T->siz - 1);
    n = adj.stride * h;
    // Within one frame nothing writes texel memory, so a tile seen before this frame (same bytes, same
    // size, same palette load) has the key it had. Hashing every triangle's texels was the interpreter's
    // biggest cost: Metal Mario's 2 KB chrome map hashed once per triangle, 14 joints, every frame.
    { Memo *m = &sMemo[(((uintptr_t)adj.img >> 3) ^ (uintptr_t)n * 2654435761u ^ (uintptr_t)T->pal * 97) & (MEMO_N - 1)];
      if (m->frame == sFrameGen && m->img == adj.img && m->n == n && m->fmt == T->fmt && m->siz == T->siz && m->w == w && m->h == h && m->pal == T->pal && m->tlut == sTlutGen) key = m->key;
      else {
        key = fnv(2166136261u, adj.img, n);
        key = (key ^ (T->fmt * 7 + T->siz * 131 + w * 1031 + h * 65599)) * 16777619u;
        if (T->fmt == 2) key = fnv(key, S.tlut + (T->siz == 0 ? T->pal * 32 : 0), T->siz == 0 ? 32 : 512);
        if (key == 0) key = 1;
        m->frame = sFrameGen; m->img = adj.img; m->n = n; m->fmt = T->fmt; m->siz = T->siz; m->w = w; m->h = h; m->pal = T->pal; m->tlut = sTlutGen; m->key = key;
        gPortCount[COUNT_TEXHASH]++;
      } }
    for (e = sTexCache[key >> (32 - TEXCACHE_BITS)]; e; e = e->next) if (e->key == key && e->w == w && e->h == h) break;
    gPortCount[COUNT_TEXLOOKUP]++;
    if (!e) {
        double t0 = port_now();
        e = malloc(sizeof *e);
        e->key = key; e->w = w; e->h = h;
        e->rgba = malloc((unsigned long)w * h * 4);
        decode(e->rgba, &adj, T, w, h);
        gPortCount[COUNT_TEXDECODE]++; gPortProf[PROF_TEXDECODE] += port_now() - t0;
        e->next = sTexCache[key >> (32 - TEXCACHE_BITS)]; sTexCache[key >> (32 - TEXCACHE_BITS)] = e;
        gPortCount[COUNT_TEXLIVE]++;
    }
    e->used = sFrameGen;
    out->key = key; out->w = w; out->h = h; out->rgba = e->rgba;
    // A tile area larger than the mask wraps at the mask and clamps only at the far edge.
    out->clamp_s = (T->cms & 2) != 0 && (((T->lrs - T->uls) >> 2) + 1) <= w;
    out->clamp_t = (T->cmt & 2) != 0 && (((T->lrt - T->ult) >> 2) + 1) <= h;
    out->mirror_s = (T->cms & 1) != 0; out->mirror_t = (T->cmt & 1) != 0;
    out->uls = T->uls / 4.0f; out->ult = T->ult / 4.0f;
}

// LRU: a decoded texture nobody drew for TEX_KEEP frames goes (palette-animated textures make a new
// key every few frames, so without this a long session only grows). The backend keeps its own copy.
#define TEX_KEEP 600
static void tex_evict(void)
{
    int b;
    for (b = 0; b < (1 << TEXCACHE_BITS); b++) {
        TexEntry **pp = &sTexCache[b], *e;
        while ((e = *pp) != NULL) {
            if (sFrameGen - e->used > TEX_KEEP) { *pp = e->next; free(e->rgba); free(e); gPortCount[COUNT_TEXLIVE]--; gPortCount[COUNT_TEXEVICT]++; }
            else pp = &e->next;
        }
    }
}

// ---- draw state ----
// The triangle state is built when an op that feeds it runs, not per triangle: sStDirty for the
// blender/combiner/colours/depth/viewport/scissor, sTexDirty for tiles, loads and palettes.
static RbState sSt; static uint32_t sStSerial, sSerialGen; static int sStDirty = 1, sTexDirty = 1;
static RbState sSent; static uint32_t sSentSerial; // the last state a rectangle sent, to share its serial
#define DIRTY() (sStDirty = 1)
#define TEXDIRTY() (sTexDirty = 1)
// The RDP blender: out = P*A + M*B per cycle (othermode L bits 16-31, cycle 1 in 30/26/22/18 and cycle 2 in
// 28/24/20/16; 1-cycle mode runs cycle 1's fields). In 2-cycle mode the first cycle always blends and feeds
// the second as IN; the shader evaluates it (MEM there is taken as IN: the game never reads memory in it).
// The final cycle blends only with FORCE_BL; without it a pixel inside the triangle (full coverage) is
// written as P (the AA edge blend on partial coverage is not emulated). Of the final cycle the shader
// produces the colour and alpha, and GL blending does what involves memory:
//   P*a + MEM*b -> src SRC_ALPHA, dst f(B)      MEM*a + M*b -> src f(B), dst SRC_ALPHA
//   no MEM      -> blended in the shader          MEM only    -> colour writes off (depth only)
// with f(1-A) = ONE_MINUS_SRC_ALPHA, f(A_MEM) = ONE (memory coverage is full), f(1) = ONE, f(0) = ZERO.
// This replaces "FORCE_BL and 1-A -> SRC_ALPHA/ONE_MINUS_SRC_ALPHA" and the G_FOG-gated fog mix: the game's
// colour animations (hit flashes, invincibility, damage tints: ftdisplaymain.c) are G_RM_FOG_PRIM_A in cycle 1.
// The mapping of every state the game uses is in proof/renderer/README.md.
static void decode_blender(RbState *st, uint32_t L, int two)
{
    uint32_t c1 = ((L >> 30 & 3) << 6) | ((L >> 26 & 3) << 4) | ((L >> 22 & 3) << 2) | (L >> 18 & 3);
    uint32_t c2 = ((L >> 28 & 3) << 6) | ((L >> 24 & 3) << 4) | ((L >> 20 & 3) << 2) | (L >> 16 & 3);
    uint32_t fin = two ? c2 : c1;
    int P = fin >> 6, B = fin & 3, M = (fin >> 2) & 3, force = (L & (1 << 14)) != 0;
    static const int f[4] = { BLF_ONE_MINUS_SRC_ALPHA, BLF_ONE, BLF_ONE, BLF_ZERO };
    st->blender = BL_FIN(fin) | (force ? BL_FORCE : 0) | (two ? BL_C1(c1) : 0);
    if ((L & (1 << 13)) && !(L & (1 << 12))) st->blender |= BL_AIN1;
    st->blend = 0; st->bsrc = BLF_ONE; st->bdst = BLF_ZERO; st->color_off = 0;
    if (!force) { if (P == 1) st->color_off = 1; return; }
    if (P != 1 && M == 1) { st->blend = 1; st->bsrc = BLF_SRC_ALPHA; st->bdst = f[B]; }
    else if (P == 1 && M != 1) { st->blend = 1; st->bsrc = f[B]; st->bdst = BLF_SRC_ALPHA; }
    else if (P == 1 && M == 1) st->color_off = 1;
}

static void fill_state(RbState *st)
{
    uint32_t L = S.oml, H = S.omh;
    int cyc = (H >> 20) & 3;
    int t;
    memset(st, 0, sizeof *st);
    st->combine_hi = S.combhi; st->combine_lo = S.comblo;
    st->two_cycle = cyc == 1;
    decode_blender(st, L, cyc == 1);
    st->alpha_compare = (L & 3) == 1 ? 1 : (L & 3) == 3 ? 2 : 0;
    st->tex_edge = (L & (1 << 12)) != 0; // CVG_X_ALPHA
    st->depth_test = (S.geom & G_ZBUFFER_) && (L & 0x10);
    st->depth_write = (S.geom & G_ZBUFFER_) && (L & 0x20);
    st->decal = ((L >> 10) & 3) == 3;
    memcpy(st->prim, S.prim, sizeof S.prim); memcpy(st->env, S.env, sizeof S.env); memcpy(st->fogcolor, S.fogc, sizeof S.fogc); memcpy(st->blendcolor, S.blendc, sizeof S.blendc);
    st->blend_a = S.blenda; st->prim_lod = S.primlod;
    memcpy(st->scissor, S.sc, sizeof S.sc); memcpy(st->viewport, S.vp, sizeof S.vp);
    st->wide_vp = S.vp[2] >= WIDE_SPAN; st->wide_sc = S.sc[2] - S.sc[0] >= WIDE_SPAN;
    st->rect = 0;
    for (t = 0; t < 2; t++) st->tex[t].key = 0;
}

static void use_textures(RbState *st, int tile)
{
    int filter = (S.omh >> 12) & 3;
    import_tile(&st->tex[0], tile);
    st->tex[0].linear = filter != 0;
    if (st->two_cycle) { import_tile(&st->tex[1], (tile + 1) & 7); st->tex[1].linear = filter != 0; }
}

#define RBMAX 256
static void emit_tri(const Vert *a, const Vert *b, const Vert *c)
{
    static RbVertex rv[3];
    const Vert *p[3] = { a, b, c };
    int i, cull = S.geom & (G_CULL_FRONT_ | G_CULL_BACK_);
    sTris++;
    if (cull) {
        float ax = a->x / a->w, ay = a->y / a->w, bx = b->x / b->w, by = b->y / b->w, cx = c->x / c->w, cy = c->y / c->w;
        float cr = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (a->w > 0 && b->w > 0 && c->w > 0) {
            if ((cull & G_CULL_BACK_) && cr < 0) return;
            if ((cull & G_CULL_FRONT_) && cr > 0) return;
        }
    }
    if (sStDirty || sTexDirty) {
        RbState t;
        if (sStDirty) { RbTexture keep[2]; memcpy(keep, sSt.tex, sizeof keep); fill_state(&t); memcpy(t.tex, keep, sizeof keep); }
        else t = sSt;
        if (sTexDirty || t.two_cycle != sSt.two_cycle) { memset(t.tex, 0, sizeof t.tex); use_textures(&t, S.textile); }
        if (memcmp(&t, &sSt, sizeof t)) { sSt = t; sStSerial = ++sSerialGen; }
        sStDirty = sTexDirty = 0; gPortCount[COUNT_STATEBUILD]++;
    }
    #define st sSt
    for (i = 0; i < 3; i++) {
        const Vert *v = p[(S.geom & G_SMOOTH_) ? i : i];
        const Vert *cv = (S.geom & G_SMOOTH_) ? v : p[0];
        RbVertex *o = &rv[i];
        const Tile *T = &S.tile[S.textile];
        float u = v->u, w = v->v, sx = 1, sy = 1;
        o->x = st.wide_vp ? v->x * sK : v->x; o->y = v->y; o->z = v->z; o->w = v->w;
        u /= 32.0f; w /= 32.0f;
        if (T->shifts) sx = T->shifts <= 10 ? 1.0f / (1 << T->shifts) : (float)(1 << (16 - T->shifts));
        if (T->shiftt) sy = T->shiftt <= 10 ? 1.0f / (1 << T->shiftt) : (float)(1 << (16 - T->shiftt));
        o->u = u * sx - T->uls / 4.0f; o->v = w * sy - T->ult / 4.0f;
        o->r = cv->r / 255.0f; o->g = cv->g / 255.0f; o->b = cv->b / 255.0f; o->a = v->a / 255.0f;
    }
    if (sTrace) printf("GTV %d,%d,%d/%d,%d@%.3f,%.3f %d,%d,%d/%d,%d@%.3f,%.3f %d,%d,%d/%d,%d@%.3f,%.3f\n",
        a->ob[0], a->ob[1], a->ob[2], a->tc[0], a->tc[1], a->ls, a->lt, b->ob[0], b->ob[1], b->ob[2], b->tc[0], b->tc[1], b->ls, b->lt, c->ob[0], c->ob[1], c->ob[2], c->tc[0], c->tc[1], c->ls, c->lt);
    trace_draw("tri", &st, rv, 3);
    rb_draw(&st, sStSerial, rv, 3);
    #undef st
}

static void do_rect(float x0, float y0, float x1, float y1, float s, float t, float ds, float dt, int textured, int flip, int tile)
{
    static RbVertex rv[6];
    RbState st;
    int cyc = (S.omh >> 20) & 3, i;
    float fx0, fy0, fx1, fy1, u0, v0, u1, v1, z;
    float c[4];
    sRects++;
    fill_state(&st);
    st.rect = 1;
    st.viewport[0] = 0; st.viewport[1] = 0; st.viewport[2] = 320; st.viewport[3] = 240; st.wide_vp = 1;
    // Copy and fill modes bypass the blender; copy mode blends with alpha when ALPHA_CVG_SEL is set (as before).
    if (cyc >= 2) { int bl = cyc == 2 && (S.oml & (1 << 13)) != 0; x1 += 1; y1 += 1;
        st.blender = BL_FIN(0x04) /* IN*A_IN + MEM*(1-A) */ | (bl ? BL_FORCE : 0); st.blend = bl; st.bsrc = BLF_SRC_ALPHA; st.bdst = BLF_ONE_MINUS_SRC_ALPHA; st.color_off = 0; st.tex_edge = 0; }
    // Copy mode's alpha compare only rejects alpha 0, whatever the blend alpha says.
    if (cyc == 2) { ds /= 4; st.alpha_compare = (S.oml & 3) == 1 ? 1 : 0; st.blend_a = 0; }
    if (textured) use_textures(&st, tile);
    fx0 = x0 / 160.0f - 1; fx1 = x1 / 160.0f - 1; fy0 = 1 - y0 / 120.0f; fy1 = 1 - y1 / 120.0f;
    // A rectangle as wide as the picture (backdrop strips, fills, flashes) covers the canvas; the HUD stays in the band.
    if (x1 - x0 < WIDE_SPAN) { fx0 *= sK; fx1 *= sK; }
    { const Tile *T = &S.tile[tile];
      u0 = s - T->uls / 4.0f; v0 = t - T->ult / 4.0f; }
    if (flip) { u1 = u0 + ds * (y1 - y0); v1 = v0 + dt * (x1 - x0); }
    else { u1 = u0 + ds * (x1 - x0); v1 = v0 + dt * (y1 - y0); }
    z = ((S.oml >> 2) & 1) ? S.primz : -1.0f;
    st.depth_write = st.depth_write && ((S.oml >> 2) & 1);
    {
      if (cyc == 3) { uint32_t f = S.fillc; c[0] = x5((f >> 11) & 31) / 255.0f; c[1] = x5((f >> 6) & 31) / 255.0f; c[2] = x5((f >> 1) & 31) / 255.0f; c[3] = (f & 1) ? 1 : 0; st.combine_hi = 0xFFFFFFFF; st.combine_lo = 0xFFFFFFFF; st.prim[0] = c[0]; st.prim[1] = c[1]; st.prim[2] = c[2]; st.prim[3] = c[3]; st.blend = 0; st.alpha_compare = 0; st.depth_test = 0; st.depth_write = 0; }
      else if (cyc == 2) { st.combine_hi = 0xFFFFFFFE; st.combine_lo = 0xFFFFFFFE; }
      c[0] = c[1] = c[2] = c[3] = 1;
      for (i = 0; i < 6; i++) { rv[i].z = z; rv[i].w = 1; rv[i].r = c[0]; rv[i].g = c[1]; rv[i].b = c[2]; rv[i].a = c[3]; } }
    rv[0].x = fx0; rv[0].y = fy0; rv[0].u = u0; rv[0].v = v0;
    rv[1].x = fx1; rv[1].y = fy0; rv[1].u = flip ? u0 : u1; rv[1].v = flip ? v1 : v0;
    rv[2].x = fx1; rv[2].y = fy1; rv[2].u = u1; rv[2].v = v1;
    rv[3] = rv[0]; rv[4] = rv[2];
    rv[5].x = fx0; rv[5].y = fy1; rv[5].u = flip ? u1 : u0; rv[5].v = flip ? v0 : v1; rv[5].z = z; rv[5].w = 1; rv[5].r = c[0]; rv[5].g = c[1]; rv[5].b = c[2]; rv[5].a = c[3];
    trace_draw("rect", &st, rv, 6);
    if (memcmp(&st, &sSent, sizeof st)) { sSent = st; sSentSerial = ++sSerialGen; }
    rb_draw(&sSent, sSentSerial, rv, 6);
}

static void run(const Cmd *c, int depth)
{
    const Cmd *start = c;
    if (depth > 18) return;
    for (;; c++) {
        uint32_t w0 = c->w0, w1 = c->w1;
        int op = w0 >> 24;
        sOps++; sOpHist[op]++;
        switch (op) {
        case OP_VTX: {
            int n = (w0 >> 12) & 0xFF; int end = (w0 >> 1) & 0x7F, dst = end - n;
            // The RSP has 64 vertex slots. A load past them would write over this interpreter's own state.
            if (dst < 0 || n <= 0 || end > 64) { sBad[0]++; if (dst < 0) dst = 0; if (dst + n > 64) n = 64 - dst; if (n <= 0) break; }
            gPortCount[COUNT_VTX] += n; do_vtx(seg(w1), n, dst); break; }
        case OP_TRI1: emit_tri(&S.vtx[((w0 >> 16) & 0xFF) / 2], &S.vtx[((w0 >> 8) & 0xFF) / 2], &S.vtx[(w0 & 0xFF) / 2]); break;
        case OP_TRI2: case OP_QUAD:
            emit_tri(&S.vtx[((w0 >> 16) & 0xFF) / 2], &S.vtx[((w0 >> 8) & 0xFF) / 2], &S.vtx[(w0 & 0xFF) / 2]);
            emit_tri(&S.vtx[((w1 >> 16) & 0xFF) / 2], &S.vtx[((w1 >> 8) & 0xFF) / 2], &S.vtx[(w1 & 0xFF) / 2]);
            break;
        case OP_CULLDL: break;
        case OP_MODIFYVTX: {
            // gSPModifyVertex patches one loaded vertex in place. The game's data uses it for texture
            // coordinates only (G_MWO_POINT_ST, s10.5 texels as the RDP sees them, no texture scale):
            // Dream Land's flower strips, fighter eyes and the like share their base vertices between
            // display lists and rewrite s/t here. Screen-space patches are not used by this game.
            int where = (w0 >> 16) & 0xFF;
            Vert *d = &S.vtx[((w0 & 0xFFFF) / 2) & 63];
            if (where == 0x14) { d->u = (int16_t)(w1 >> 16); d->v = (int16_t)(w1 & 0xFFFF); d->ls = d->lt = -1; }
            else if (where == 0x10) { d->r = w1 >> 24; d->g = w1 >> 16; d->b = w1 >> 8; d->a = w1; }
            break; }
        case OP_SPECIAL_1: if ((w0 & 0xFF) == 1) recompute_mvp(); break; // gSPMvpRecalc
        case OP_BRANCH_Z: break;
        case OP_TEXTURE: TEXDIRTY();
            S.texon = w0 & 2 ? 1 : 0; S.textile = (w0 >> 8) & 7; S.texlevel = (w0 >> 11) & 7;
            S.texs = (w1 >> 16) / 65536.0f; S.texT = (w1 & 0xFFFF) / 65536.0f;
            if ((w1 >> 16) == 0xFFFF) S.texs = 1.0f;
            if ((w1 & 0xFFFF) == 0xFFFF) S.texT = 1.0f;
            break;
        case OP_POPMTX: { int n = w1 / 64; S.mvtop -= n; if (S.mvtop < 0) S.mvtop = 0; recompute_mvp(); break; }
        case OP_GEOMETRYMODE: DIRTY(); S.geom = (S.geom & (w0 & 0xFFFFFF)) | w1; break;
        case OP_MTX: {
            int p = (w0 & 0xFF) ^ 1; // G_MTX_PUSH is inverted in the encoding
            float m[4][4];
            mat_load(m, seg(w1));
            if (p & 4) { if (p & 2) memcpy(S.proj, m, sizeof m); else mat_mul(S.proj, m, S.proj); }
            else {
                if (p & 1) { if (S.mvtop < 9) { memcpy(S.mv[S.mvtop + 1], S.mv[S.mvtop], sizeof m); S.mvtop++; } }
                if (p & 2) memcpy(S.mv[S.mvtop], m, sizeof m); else mat_mul(S.mv[S.mvtop], m, S.mv[S.mvtop]);
            }
            recompute_mvp();
            break; }
        case OP_MOVEWORD: {
            int idx = (w0 >> 16) & 0xFF, off = w0 & 0xFFFF;
            if (sCovOn) sMwHist[idx & 15]++;
            switch (idx) {
            case 0x00: mvp_word(off, w1); break;
            case 0x02: S.numlights = w1 / 24; if (S.numlights > 7) { sBad[1]++; S.numlights = 7; } break;
            case 0x06: S.seg[(off / 4) & 15] = w1 & 0xFFFFFF; if (off / 4 == 0) S.seg[0] = 0; break;
            case 0x08: S.fogm = (int16_t)(w1 >> 16); S.fogo = (int16_t)(w1 & 0xFFFF); break;
            case 0x0A: { int l = off / 24; if (l < 8 && !(off % 24)) { S.lightcol[l][0] = w1 >> 24; S.lightcol[l][1] = w1 >> 16; S.lightcol[l][2] = w1 >> 8; } break; }
            default: break;
            }
            break; }
        case OP_MOVEMEM: {
            int idx = w0 & 0xFF, off = ((w0 >> 8) & 0xFF) * 8;
            const uint8_t *p = seg(w1);
            if (idx == 8) { const int16_t *v = (const int16_t *)p; DIRTY();
                int sx = v[0] / 4, sy = v[1] / 4, tx = v[4] / 4, ty = v[5] / 4;
                S.vp[0] = tx - sx; S.vp[1] = ty - sy; S.vp[2] = sx * 2; S.vp[3] = sy * 2;
            } else if (idx == 10) {
                int l = off / 24;
                if (l < 2) { S.lookat[l][0] = p[8]; S.lookat[l][1] = p[9]; S.lookat[l][2] = p[10]; if (sTrace) printf("GTL lookat%d=%d,%d,%d\n", l, S.lookat[l][0], S.lookat[l][1], S.lookat[l][2]); }
                else if (l - 2 < 8) { l -= 2; S.lightcol[l][0] = p[0]; S.lightcol[l][1] = p[1]; S.lightcol[l][2] = p[2]; S.lightdir[l][0] = p[8]; S.lightdir[l][1] = p[9]; S.lightdir[l][2] = p[10]; }
            }
            break; }
        case OP_DL:
            if ((w0 >> 16) & 1) { c = (const Cmd *)seg(w1) - 1; continue; }
            run(seg(w1), depth + 1);
            break;
        case OP_ENDDL: return;
        case OP_RDPHALF_1: S.half1 = w1; break;
        case OP_RDPHALF_2: S.half2 = w1; break;
        case OP_OTHERMODE_L: case OP_OTHERMODE_H: {
            int len = (w0 & 0xFF) + 1, shift = 32 - ((w0 >> 8) & 0xFF) - len;
            uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
            uint32_t *r = op == OP_OTHERMODE_L ? &S.oml : &S.omh;
            DIRTY(); if (op == OP_OTHERMODE_H) TEXDIRTY(); // cycle type and texture filter
            *r = (*r & ~mask) | (w1 & mask);
            break; }
        case OP_TEXRECT: case OP_TEXRECTFLIP: {
            float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
            int tile = (w1 >> 24) & 7;
            float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
            uint32_t h1, h2;
            c++; h1 = c->w1; c++; h2 = c->w1;
            if (!S.cimg_main) break; // a rectangle aimed at the depth image (the magnifier's mask) is not picture
            do_rect(ulx, uly, lrx, lry, (int16_t)(h1 >> 16) / 32.0f, (int16_t)(h1 & 0xFFFF) / 32.0f,
                    (int16_t)(h2 >> 16) / 1024.0f, (int16_t)(h2 & 0xFFFF) / 1024.0f, 1, op == OP_TEXRECTFLIP, tile);
            break; }
        case OP_FILLRECT: {
            float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f, ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
            if (S.cimg_depth) { rb_clear_depth(); break; }
            if (!S.cimg_main) break;
            do_rect(ulx, uly, lrx, lry, 0, 0, 0, 0, 0, 0, 0);
            break; }
        case OP_LOADTLUT: {
            int t = (w1 >> 24) & 7, count = ((w1 >> 14) & 0x3FF) + 1;
            int start = S.tile[t].tmem - 256;
            if (start < 0) start = 0;
            if (start * 2 + count * 2 > 512) count = 256 - start;
            memcpy(S.tlut + start * 2, S.timg, count * 2); sTlutGen++; TEXDIRTY();
            if (sTrace) { const uint8_t *q = S.timg; printf("GTP tlut count=%d start=%d bytes=%02x%02x %02x%02x %02x%02x %02x%02x\n", count, start, q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7]); }
            break; }
        case OP_LOADBLOCK: {
            int t = (w1 >> 24) & 7;
            Loaded *L = &S.tm[S.tile[t].tmem & 511];
            int dxt = w1 & 0xFFF;
            L->img = S.timg; L->fmt = S.timgfmt; L->siz = S.timgsiz; L->uls = 0; L->ult = 0;
            L->stride = 0; L->shuffled = dxt == 0; TEXDIRTY();
            if (sTrace) { const uint8_t *q = S.timg; printf("GTB tile=%d fmt=%d siz=%d dxt=%03x img=%p bytes=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n", t, S.timgfmt, S.timgsiz, dxt, (void *)q, q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7],q[8],q[9],q[10],q[11],q[12],q[13],q[14],q[15]); }
            break; }
        case OP_LOADTILE: {
            int t = (w1 >> 24) & 7;
            Loaded *L = &S.tm[S.tile[t].tmem & 511];
            int uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF;
            int bpp = S.timgsiz == 0 ? 0 : 1 << (S.timgsiz - 1);
            L->stride = S.timgw * (bpp ? bpp : 1); if (!bpp) L->stride = (S.timgw + 1) / 2;
            L->img = S.timg + (ult >> 2) * L->stride + (bpp ? (uls >> 2) * bpp : (uls >> 2) / 2);
            L->fmt = S.timgfmt; L->siz = S.timgsiz; L->shuffled = 0; TEXDIRTY();
            break; }
        case OP_SETTILE: {
            Tile *T = &S.tile[(w1 >> 24) & 7]; TEXDIRTY();
            T->fmt = (w0 >> 21) & 7; T->siz = (w0 >> 19) & 3; T->line = (w0 >> 9) & 0x1FF; T->tmem = w0 & 0x1FF;
            T->pal = (w1 >> 20) & 15; T->cmt = (w1 >> 18) & 3; T->maskt = (w1 >> 14) & 15; T->shiftt = (w1 >> 10) & 15;
            T->cms = (w1 >> 8) & 3; T->masks = (w1 >> 4) & 15; T->shifts = w1 & 15;
            break; }
        case OP_SETTILESIZE: {
            Tile *T = &S.tile[(w1 >> 24) & 7]; TEXDIRTY();
            T->uls = (w0 >> 12) & 0xFFF; T->ult = w0 & 0xFFF; T->lrs = (w1 >> 12) & 0xFFF; T->lrt = w1 & 0xFFF;
            break; }
        case OP_FILLCOLOR: S.fillc = w1; break;
        case OP_FOGCOLOR: DIRTY(); S.fogc[0] = (w1 >> 24) / 255.0f; S.fogc[1] = ((w1 >> 16) & 255) / 255.0f; S.fogc[2] = ((w1 >> 8) & 255) / 255.0f; S.fogc[3] = (w1 & 255) / 255.0f; break;
        case OP_BLENDCOLOR: DIRTY(); S.blendc[0] = (w1 >> 24) / 255.0f; S.blendc[1] = ((w1 >> 16) & 255) / 255.0f; S.blendc[2] = ((w1 >> 8) & 255) / 255.0f; S.blenda = (w1 & 255) / 255.0f; break;
        case OP_PRIMCOLOR: DIRTY(); S.prim[0] = (w1 >> 24) / 255.0f; S.prim[1] = ((w1 >> 16) & 255) / 255.0f; S.prim[2] = ((w1 >> 8) & 255) / 255.0f; S.prim[3] = (w1 & 255) / 255.0f; S.primlod = (w0 & 255) / 255.0f; break;
        case OP_ENVCOLOR: DIRTY(); S.env[0] = (w1 >> 24) / 255.0f; S.env[1] = ((w1 >> 16) & 255) / 255.0f; S.env[2] = ((w1 >> 8) & 255) / 255.0f; S.env[3] = (w1 & 255) / 255.0f; break;
        case OP_COMBINE: DIRTY(); S.combhi = w0 & 0xFFFFFF; S.comblo = w1; break;
        case OP_SETTIMG: S.timg = seg(w1); S.timgfmt = (w0 >> 21) & 7; S.timgsiz = (w0 >> 19) & 3; S.timgw = (w0 & 0xFFF) + 1; break;
        case OP_SETZIMG: S.zimg = seg(w1); break;
        case OP_SETCIMG: {
            int isseg = (w1 >> 24) == 0x0F;
            S.cimg_main = isseg;
            S.cimg_depth = !isseg && (const void *)seg(w1) == S.zimg;
            break; }
        case OP_PRIMDEPTH: DIRTY(); S.primz = (int16_t)(w1 >> 16) / 16384.0f - 1.0f; if (S.primz > 1) S.primz = 1; break;
        case OP_SCISSOR: DIRTY();
            S.sc[0] = ((w0 >> 12) & 0xFFF) / 4; S.sc[1] = (w0 & 0xFFF) / 4; S.sc[2] = ((w1 >> 12) & 0xFFF) / 4; S.sc[3] = (w1 & 0xFFF) / 4;
            break;
        default: if (sCovOn) sDefault[op]++;
            // Trace and coverage runs only: the first unknown non-sync opcodes, with the list they sit in.
            { static int seen; if ((sTrace || sCovOn) && seen < 24 && (op < 0xE6 || op > 0xE9)) { seen++; printf("GTOP op=%02x w0=%08x w1=%08x depth=%d at=%d start=%p first=%08x:%08x %08x:%08x %08x:%08x %08x:%08x prev=%08x:%08x %08x:%08x %08x:%08x %08x:%08x\n", op, w0, w1, depth, (int)(c - start), (void *)start,
                start[0].w0, start[0].w1, start[1].w0, start[1].w1, start[2].w0, start[2].w1, start[3].w0, start[3].w1,
                c[-4].w0, c[-4].w1, c[-3].w0, c[-3].w1, c[-2].w0, c[-2].w1, c[-1].w0, c[-1].w1); } }
            break;
        }
    }
}

void port_gfx_run(void *dl)
{
    static int init;
    if (!init) {
        init = 1;
        S.vp[2] = 320; S.vp[3] = 240; S.sc[2] = 320; S.sc[3] = 240;
    }
    S.mvtop = 0; S.cimg_main = 1; S.cimg_depth = 0;
    sFrameGen++;
    sStDirty = sTexDirty = 1; // texel memory may have changed since the last frame: keys are per frame
    if (sFrameGen % 60 == 0) tex_evict();
    rb_begin_frame();
    if (sTrace) printf("GT frame\n");
    { uint32_t o = sOps, t = sTris, r = sRects;
      run((const Cmd *)dl, 0);
      gPortCount[COUNT_OPS] += sOps - o; gPortCount[COUNT_TRIS] += sTris - t; gPortCount[COUNT_RECTS] += sRects - r; }
    rb_end_frame();
    if (sTrace > 0) sTrace--;
}
