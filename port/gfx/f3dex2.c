// F3DEX2 display list interpreter. Reads what the game's own RSP/RDP would have read and draws it
// through a small backend. Render only: nothing here writes game state.
#include "port.h"
#include "gfx.h"
void *memset(void *, int, unsigned long);
void *memcpy(void *, const void *, unsigned long);
float sqrtf(float);
void *malloc(unsigned long);

typedef struct { uint32_t w0, w1; } Cmd;
typedef struct { float x, y, z, w, u, v; uint8_t r, g, b, a; } Vert;
typedef struct { int16_t ob[3]; uint16_t flag; int16_t tc[2]; uint8_t cn[4]; } N64Vtx;

enum {
    OP_VTX = 0x01, OP_MODIFYVTX = 0x02, OP_CULLDL = 0x03, OP_BRANCH_Z = 0x04, OP_TRI1 = 0x05, OP_TRI2 = 0x06, OP_QUAD = 0x07,
    OP_TEXTURE = 0xD7, OP_POPMTX = 0xD8, OP_GEOMETRYMODE = 0xD9, OP_MTX = 0xDA, OP_MOVEWORD = 0xDB, OP_MOVEMEM = 0xDC,
    OP_DL = 0xDE, OP_ENDDL = 0xDF, OP_RDPHALF_1 = 0xE1, OP_OTHERMODE_L = 0xE2, OP_OTHERMODE_H = 0xE3, OP_TEXRECT = 0xE4,
    OP_TEXRECTFLIP = 0xE5, OP_LOADTLUT = 0xF0, OP_RDPHALF_2 = 0xF1, OP_SETTILESIZE = 0xF2, OP_LOADBLOCK = 0xF3, OP_LOADTILE = 0xF4,
    OP_SETTILE = 0xF5, OP_FILLRECT = 0xF6, OP_FILLCOLOR = 0xF7, OP_FOGCOLOR = 0xF8, OP_BLENDCOLOR = 0xF9, OP_PRIMCOLOR = 0xFA,
    OP_ENVCOLOR = 0xFB, OP_COMBINE = 0xFC, OP_SETTIMG = 0xFD, OP_SETZIMG = 0xFE, OP_SETCIMG = 0xFF, OP_PRIMDEPTH = 0xEE, OP_SCISSOR = 0xED,
};
enum { G_ZBUFFER_ = 1, G_SHADE_ = 4, G_CULL_FRONT_ = 0x200, G_CULL_BACK_ = 0x400, G_FOG_ = 0x10000, G_LIGHTING_ = 0x20000, G_TEXGEN_ = 0x40000, G_TEXGENLIN_ = 0x80000, G_SMOOTH_ = 0x200000 };

typedef struct { int fmt, siz, line, tmem, pal, cms, cmt, masks, maskt, shifts, shiftt; int uls, ult, lrs, lrt; } Tile;
typedef struct { const uint8_t *img; int stride; int siz; int fmt; int uls, ult; int shuffled; } Loaded;

static struct {
    float mv[10][4][4], proj[4][4], mvp[4][4]; int mvtop;
    Vert vtx[64];
    uint32_t geom, omh, oml;
    uint32_t seg[16];
    uint32_t combhi, comblo;
    float prim[4], env[4], fogc[3], blenda, primlod; uint32_t fillc;
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

static void recompute_mvp(void) { mat_mul(S.mvp, S.mv[S.mvtop], S.proj); }

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
            }
        } else { d->r = v->cn[0]; d->g = v->cn[1]; d->b = v->cn[2]; }
        d->a = v->cn[3];
        if (S.geom & G_FOG_) {
            float w = d->w > 0.0001f ? d->w : 0.0001f;
            float f = (d->z / w) * S.fogm + S.fogo;
            d->a = f < 0 ? 0 : f > 255 ? 255 : (uint8_t)f;
        }
        d->u = U; d->v = V;
    }
}

// ---- textures ----
#define TEXCACHE_BITS 12
typedef struct TexEntry { uint32_t key; int w, h; uint8_t *rgba; struct TexEntry *next; } TexEntry;
static TexEntry *sTexCache[1 << TEXCACHE_BITS];

static uint32_t fnv(uint32_t h, const uint8_t *p, int n) { int i; for (i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; } return h; }
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
    key = fnv(2166136261u, adj.img, n);
    key = (key ^ (T->fmt * 7 + T->siz * 131 + w * 1031 + h * 65599)) * 16777619u;
    if (T->fmt == 2) key = fnv(key, S.tlut + (T->siz == 0 ? T->pal * 32 : 0), T->siz == 0 ? 32 : 512);
    if (key == 0) key = 1;
    for (e = sTexCache[key >> (32 - TEXCACHE_BITS)]; e; e = e->next) if (e->key == key && e->w == w && e->h == h) break;
    if (!e) {
        e = malloc(sizeof *e);
        e->key = key; e->w = w; e->h = h;
        e->rgba = malloc((unsigned long)w * h * 4);
        decode(e->rgba, &adj, T, w, h);
        e->next = sTexCache[key >> (32 - TEXCACHE_BITS)]; sTexCache[key >> (32 - TEXCACHE_BITS)] = e;
    }
    out->key = key; out->w = w; out->h = h; out->rgba = e->rgba;
    // A tile area larger than the mask wraps at the mask and clamps only at the far edge.
    out->clamp_s = (T->cms & 2) != 0 && (((T->lrs - T->uls) >> 2) + 1) <= w;
    out->clamp_t = (T->cmt & 2) != 0 && (((T->lrt - T->ult) >> 2) + 1) <= h;
    out->mirror_s = (T->cms & 1) != 0; out->mirror_t = (T->cmt & 1) != 0;
    out->uls = T->uls / 4.0f; out->ult = T->ult / 4.0f;
}

// ---- draw state ----
static void fill_state(RbState *st)
{
    uint32_t L = S.oml, H = S.omh;
    int cyc = (H >> 20) & 3;
    int t;
    st->combine_hi = S.combhi; st->combine_lo = S.comblo;
    st->two_cycle = cyc == 1;
    st->alpha_blend = ((L & (1 << 14)) != 0 /* FORCE_BL */) && ((L >> 18 & 3) == 0 || (L >> 16 & 3) == 0) ;
    st->fog = (L >> 30) == 3 /* blender P = fog */ && (S.geom & G_FOG_);
    st->alpha_compare = (L & 3) == 1 ? 1 : (L & (1 << 13)) ? 2 : 0;
    st->depth_test = (S.geom & G_ZBUFFER_) && (L & 0x10);
    st->depth_write = (S.geom & G_ZBUFFER_) && (L & 0x20);
    st->decal = ((L >> 10) & 3) == 3;
    memcpy(st->prim, S.prim, sizeof S.prim); memcpy(st->env, S.env, sizeof S.env); memcpy(st->fogcolor, S.fogc, sizeof S.fogc);
    st->blend_a = S.blenda; st->prim_lod = S.primlod;
    memcpy(st->scissor, S.sc, sizeof S.sc); memcpy(st->viewport, S.vp, sizeof S.vp);
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
    RbState st;
    sTris++;
    if (cull) {
        float ax = a->x / a->w, ay = a->y / a->w, bx = b->x / b->w, by = b->y / b->w, cx = c->x / c->w, cy = c->y / c->w;
        float cr = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (a->w > 0 && b->w > 0 && c->w > 0) {
            if ((cull & G_CULL_BACK_) && cr < 0) return;
            if ((cull & G_CULL_FRONT_) && cr > 0) return;
        }
    }
    fill_state(&st);
    use_textures(&st, S.textile);
    for (i = 0; i < 3; i++) {
        const Vert *v = p[(S.geom & G_SMOOTH_) ? i : i];
        const Vert *cv = (S.geom & G_SMOOTH_) ? v : p[0];
        RbVertex *o = &rv[i];
        const Tile *T = &S.tile[S.textile];
        float u = v->u, w = v->v, sx = 1, sy = 1;
        o->x = v->x; o->y = v->y; o->z = v->z; o->w = v->w;
        u /= 32.0f; w /= 32.0f;
        if (T->shifts) sx = T->shifts <= 10 ? 1.0f / (1 << T->shifts) : (float)(1 << (16 - T->shifts));
        if (T->shiftt) sy = T->shiftt <= 10 ? 1.0f / (1 << T->shiftt) : (float)(1 << (16 - T->shiftt));
        o->u = u * sx - T->uls / 4.0f; o->v = w * sy - T->ult / 4.0f;
        o->r = cv->r / 255.0f; o->g = cv->g / 255.0f; o->b = cv->b / 255.0f; o->a = v->a / 255.0f;
    }
    rb_draw(&st, rv, 3);
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
    st.viewport[0] = 0; st.viewport[1] = 0; st.viewport[2] = 320; st.viewport[3] = 240;
    if (cyc >= 2) { x1 += 1; y1 += 1; st.alpha_blend = (cyc == 2) ? (S.oml & (1<<13)) != 0 : st.alpha_blend; }
    // Copy mode's alpha compare only rejects alpha 0, whatever the blend alpha says.
    if (cyc == 2) { ds /= 4; st.alpha_compare = (S.oml & 3) == 1 ? 1 : 0; st.blend_a = 0; }
    if (textured) use_textures(&st, tile);
    fx0 = x0 / 160.0f - 1; fx1 = x1 / 160.0f - 1; fy0 = 1 - y0 / 120.0f; fy1 = 1 - y1 / 120.0f;
    { const Tile *T = &S.tile[tile];
      u0 = s - T->uls / 4.0f; v0 = t - T->ult / 4.0f; }
    if (flip) { u1 = u0 + ds * (y1 - y0); v1 = v0 + dt * (x1 - x0); }
    else { u1 = u0 + ds * (x1 - x0); v1 = v0 + dt * (y1 - y0); }
    z = ((S.oml >> 2) & 1) ? S.primz : -1.0f;
    st.depth_write = st.depth_write && ((S.oml >> 2) & 1);
    {
      if (cyc == 3) { uint32_t f = S.fillc; c[0] = x5((f >> 11) & 31) / 255.0f; c[1] = x5((f >> 6) & 31) / 255.0f; c[2] = x5((f >> 1) & 31) / 255.0f; c[3] = (f & 1) ? 1 : 0; st.combine_hi = 0xFFFFFFFF; st.combine_lo = 0xFFFFFFFF; st.prim[0] = c[0]; st.prim[1] = c[1]; st.prim[2] = c[2]; st.prim[3] = c[3]; st.alpha_blend = 0; st.alpha_compare = 0; st.depth_test = 0; st.depth_write = 0; }
      else if (cyc == 2) { st.combine_hi = 0xFFFFFFFE; st.combine_lo = 0xFFFFFFFE; }
      c[0] = c[1] = c[2] = c[3] = 1;
      for (i = 0; i < 6; i++) { rv[i].z = z; rv[i].w = 1; rv[i].r = c[0]; rv[i].g = c[1]; rv[i].b = c[2]; rv[i].a = c[3]; } }
    rv[0].x = fx0; rv[0].y = fy0; rv[0].u = u0; rv[0].v = v0;
    rv[1].x = fx1; rv[1].y = fy0; rv[1].u = flip ? u0 : u1; rv[1].v = flip ? v1 : v0;
    rv[2].x = fx1; rv[2].y = fy1; rv[2].u = u1; rv[2].v = v1;
    rv[3] = rv[0]; rv[4] = rv[2];
    rv[5].x = fx0; rv[5].y = fy1; rv[5].u = flip ? u1 : u0; rv[5].v = flip ? v0 : v1; rv[5].z = z; rv[5].w = 1; rv[5].r = c[0]; rv[5].g = c[1]; rv[5].b = c[2]; rv[5].a = c[3];
    rb_draw(&st, rv, 6);
}

static void run(const Cmd *c, int depth)
{
    if (depth > 18) return;
    for (;; c++) {
        uint32_t w0 = c->w0, w1 = c->w1;
        int op = w0 >> 24;
        sOps++; sOpHist[op]++;
        switch (op) {
        case OP_VTX: { int n = (w0 >> 12) & 0xFF; int end = (w0 >> 1) & 0x7F; do_vtx(seg(w1), n, end - n); break; }
        case OP_TRI1: emit_tri(&S.vtx[((w0 >> 16) & 0xFF) / 2], &S.vtx[((w0 >> 8) & 0xFF) / 2], &S.vtx[(w0 & 0xFF) / 2]); break;
        case OP_TRI2: case OP_QUAD:
            emit_tri(&S.vtx[((w0 >> 16) & 0xFF) / 2], &S.vtx[((w0 >> 8) & 0xFF) / 2], &S.vtx[(w0 & 0xFF) / 2]);
            emit_tri(&S.vtx[((w1 >> 16) & 0xFF) / 2], &S.vtx[((w1 >> 8) & 0xFF) / 2], &S.vtx[(w1 & 0xFF) / 2]);
            break;
        case OP_CULLDL: case OP_MODIFYVTX: break;
        case OP_BRANCH_Z: break;
        case OP_TEXTURE:
            S.texon = w0 & 2 ? 1 : 0; S.textile = (w0 >> 8) & 7; S.texlevel = (w0 >> 11) & 7;
            S.texs = (w1 >> 16) / 65536.0f; S.texT = (w1 & 0xFFFF) / 65536.0f;
            if ((w1 >> 16) == 0xFFFF) S.texs = 1.0f;
            if ((w1 & 0xFFFF) == 0xFFFF) S.texT = 1.0f;
            break;
        case OP_POPMTX: { int n = w1 / 64; S.mvtop -= n; if (S.mvtop < 0) S.mvtop = 0; recompute_mvp(); break; }
        case OP_GEOMETRYMODE: S.geom = (S.geom & (w0 & 0xFFFFFF)) | w1; break;
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
            switch (idx) {
            case 0x02: S.numlights = w1 / 24; break;
            case 0x06: S.seg[(off / 4) & 15] = w1 & 0xFFFFFF; if (off / 4 == 0) S.seg[0] = 0; break;
            case 0x08: S.fogm = (int16_t)(w1 >> 16); S.fogo = (int16_t)(w1 & 0xFFFF); break;
            case 0x0A: { int l = off / 24; if (l < 8 && !(off % 24)) { S.lightcol[l][0] = w1 >> 24; S.lightcol[l][1] = w1 >> 16; S.lightcol[l][2] = w1 >> 8; } break; }
            default: break;
            }
            break; }
        case OP_MOVEMEM: {
            int idx = w0 & 0xFF, off = ((w0 >> 8) & 0xFF) * 8;
            const uint8_t *p = seg(w1);
            if (idx == 8) { const int16_t *v = (const int16_t *)p;
                int sx = v[0] / 4, sy = v[1] / 4, tx = v[4] / 4, ty = v[5] / 4;
                S.vp[0] = tx - sx; S.vp[1] = ty - sy; S.vp[2] = sx * 2; S.vp[3] = sy * 2;
            } else if (idx == 10) {
                int l = off / 24;
                if (l < 2) { S.lookat[l][0] = p[8]; S.lookat[l][1] = p[9]; S.lookat[l][2] = p[10]; }
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
            memcpy(S.tlut + start * 2, S.timg, count * 2);
            break; }
        case OP_LOADBLOCK: {
            int t = (w1 >> 24) & 7;
            Loaded *L = &S.tm[S.tile[t].tmem & 511];
            int dxt = w1 & 0xFFF;
            L->img = S.timg; L->fmt = S.timgfmt; L->siz = S.timgsiz; L->uls = 0; L->ult = 0;
            L->stride = 0; L->shuffled = dxt == 0;
            break; }
        case OP_LOADTILE: {
            int t = (w1 >> 24) & 7;
            Loaded *L = &S.tm[S.tile[t].tmem & 511];
            int uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF;
            int bpp = S.timgsiz == 0 ? 0 : 1 << (S.timgsiz - 1);
            L->stride = S.timgw * (bpp ? bpp : 1); if (!bpp) L->stride = (S.timgw + 1) / 2;
            L->img = S.timg + (ult >> 2) * L->stride + (bpp ? (uls >> 2) * bpp : (uls >> 2) / 2);
            L->fmt = S.timgfmt; L->siz = S.timgsiz; L->shuffled = 0;
            break; }
        case OP_SETTILE: {
            Tile *T = &S.tile[(w1 >> 24) & 7];
            T->fmt = (w0 >> 21) & 7; T->siz = (w0 >> 19) & 3; T->line = (w0 >> 9) & 0x1FF; T->tmem = w0 & 0x1FF;
            T->pal = (w1 >> 20) & 15; T->cmt = (w1 >> 18) & 3; T->maskt = (w1 >> 14) & 15; T->shiftt = (w1 >> 10) & 15;
            T->cms = (w1 >> 8) & 3; T->masks = (w1 >> 4) & 15; T->shifts = w1 & 15;
            break; }
        case OP_SETTILESIZE: {
            Tile *T = &S.tile[(w1 >> 24) & 7];
            T->uls = (w0 >> 12) & 0xFFF; T->ult = w0 & 0xFFF; T->lrs = (w1 >> 12) & 0xFFF; T->lrt = w1 & 0xFFF;
            break; }
        case OP_FILLCOLOR: S.fillc = w1; break;
        case OP_FOGCOLOR: S.fogc[0] = (w1 >> 24) / 255.0f; S.fogc[1] = ((w1 >> 16) & 255) / 255.0f; S.fogc[2] = ((w1 >> 8) & 255) / 255.0f; break;
        case OP_BLENDCOLOR: S.blenda = (w1 & 255) / 255.0f; break;
        case OP_PRIMCOLOR: S.prim[0] = (w1 >> 24) / 255.0f; S.prim[1] = ((w1 >> 16) & 255) / 255.0f; S.prim[2] = ((w1 >> 8) & 255) / 255.0f; S.prim[3] = (w1 & 255) / 255.0f; S.primlod = (w0 & 255) / 255.0f; break;
        case OP_ENVCOLOR: S.env[0] = (w1 >> 24) / 255.0f; S.env[1] = ((w1 >> 16) & 255) / 255.0f; S.env[2] = ((w1 >> 8) & 255) / 255.0f; S.env[3] = (w1 & 255) / 255.0f; break;
        case OP_COMBINE: S.combhi = w0 & 0xFFFFFF; S.comblo = w1; break;
        case OP_SETTIMG: S.timg = seg(w1); S.timgfmt = (w0 >> 21) & 7; S.timgsiz = (w0 >> 19) & 3; S.timgw = (w0 & 0xFFF) + 1; break;
        case OP_SETZIMG: S.zimg = seg(w1); break;
        case OP_SETCIMG: {
            int isseg = (w1 >> 24) == 0x0F;
            S.cimg_main = isseg;
            S.cimg_depth = !isseg && (const void *)seg(w1) == S.zimg;
            break; }
        case OP_PRIMDEPTH: S.primz = (int16_t)(w1 >> 16) / 16384.0f - 1.0f; if (S.primz > 1) S.primz = 1; break;
        case OP_SCISSOR:
            S.sc[0] = ((w0 >> 12) & 0xFFF) / 4; S.sc[1] = (w0 & 0xFFF) / 4; S.sc[2] = ((w1 >> 12) & 0xFFF) / 4; S.sc[3] = (w1 & 0xFFF) / 4;
            break;
        default: break;
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
    rb_begin_frame();
    run((const Cmd *)dl, 0);
}
