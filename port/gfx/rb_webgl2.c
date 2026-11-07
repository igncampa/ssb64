// WebGL2 backend. Combiner modes become fragment shaders, cached by mode.
#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#include <emscripten.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gfx.h"
#include "port_prof.h"

typedef struct { uint32_t hi, lo, bl; int flags; GLuint prog; GLint uPrim, uEnv, uFog, uBlendC, uBlendA, uLod, uS0, uS1, uScale0, uScale1; uint32_t used; int next; } Prog;
// Programs: a hash on (combine words, flags) and, when the table is full, the least recently used one goes.
#define MAXP 512
#define PHASH 1024
static Prog sP[MAXP]; static int sNP, sPHead[PHASH];
static uint32_t sFrame; // frames drawn, for the LRU stamps
static GLuint sVbo, sVao; static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE sCtx;
static int sCanvasW = 960, sCanvasH = 720;

typedef struct Tx { uint32_t key; int w, h; GLuint id; int params; uint32_t used; struct Tx *next; } Tx;
static Tx *sTx[4096];
static GLuint sWhite;

static const char *VS =
"#version 300 es\nlayout(location=0) in vec4 p; layout(location=1) in vec2 t; layout(location=2) in vec4 c;\n"
"out vec2 vt; out vec4 vc; void main(){ gl_Position=p; vt=t; vc=c; }\n";

// N64 combiner inputs
static const char *rgb_a(int i) { static const char *n[]={"cmb.rgb","t0.rgb","t1.rgb","uPrim.rgb","vc.rgb","uEnv.rgb","vec3(1.0)","vec3(noise())"}; return i<8?n[i]:"vec3(0.0)"; }
static const char *rgb_b(int i) { static const char *n[]={"cmb.rgb","t0.rgb","t1.rgb","uPrim.rgb","vc.rgb","uEnv.rgb","vec3(0.0)","vec3(0.0)"}; return i<8?n[i]:"vec3(0.0)"; }
static const char *rgb_c(int i) { static const char *n[]={"cmb.rgb","t0.rgb","t1.rgb","uPrim.rgb","vc.rgb","uEnv.rgb","vec3(0.0)","vec3(cmb.a)","vec3(t0.a)","vec3(t1.a)","vec3(uPrim.a)","vec3(vc.a)","vec3(uEnv.a)","vec3(uLod)","vec3(uLod)","vec3(0.0)"}; return i<16?n[i]:"vec3(0.0)"; }
static const char *rgb_d(int i) { static const char *n[]={"cmb.rgb","t0.rgb","t1.rgb","uPrim.rgb","vc.rgb","uEnv.rgb","vec3(1.0)","vec3(0.0)"}; return i<8?n[i]:"vec3(0.0)"; }
static const char *a_abd(int i) { static const char *n[]={"cmb.a","t0.a","t1.a","uPrim.a","vc.a","uEnv.a","1.0","0.0"}; return n[i&7]; }
static const char *a_c(int i) { static const char *n[]={"uLod","t0.a","t1.a","uPrim.a","vc.a","uEnv.a","uLod","0.0"}; return n[i&7]; }

static GLuint compile(GLenum ty, const char *src)
{
    GLuint s = glCreateShader(ty); GLint ok;
    glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[2048]; glGetShaderInfoLog(s, 2048, NULL, b); fprintf(stderr, "shader: %s\n%s\n", b, src); }
    return s;
}

enum { F_DITHER = 1, F_ATEST = 2, F_EDGE = 4, F_2CYC = 8, F_TEX0 = 16, F_TEX1 = 32, F_3PT0 = 64, F_3PT1 = 128 };

// Picture settings (PortSetPicture, from the page): render only, every client simulates the same game.
//   aa:     4x MSAA through a multisampled framebuffer resolved into the canvas (off if unsupported)
//   filter: the RDP's 3-point texture filter for G_TF_BILERP tiles instead of GL's 4-point bilinear
//   n64:    draw at 240 lines (the N64's 320x240, wider with the widescreen view) and scale up unfiltered
static int sAA = 1, sFilter3 = 1, sN64 = 0;
EMSCRIPTEN_KEEPALIVE void PortSetPicture(int aa, int filter, int n64) { sAA = aa != 0; sFilter3 = filter != 0; sN64 = n64 != 0; }

// Blender inputs in the shader (gfx.h codes). MEM is never read here: GL blending supplies it.
static const char *bl_col(int i) { static const char *n[] = { "cin", "cin", "uBlendC", "uFog.rgb" }; return n[i & 3]; }
static const char *bl_a(int i) { static const char *n[] = { "ain", "uFog.a", "vc.a", "0.0" }; return n[i & 3]; }
static const char *bl_b(int i) { static const char *n[] = { "(1.0-a)", "1.0", "1.0", "0.0" }; return n[i & 3]; }
static int blender_glsl(char *o, uint32_t bl)
{
    int n = 0, fin = (bl >> 9) & 0xFF, P = fin >> 6, A = (fin >> 4) & 3, M = (fin >> 2) & 3, B = fin & 3;
    n += sprintf(o + n, "vec3 cin=cmb.rgb; float ain=%s; float a;\n", (bl & BL_AIN1) ? "1.0" : "cmb.a");
    if (bl & 0x100) { int c = bl & 0xFF; // first cycle of two
        n += sprintf(o + n, "a=%s; cin=%s*a+%s*%s;\n", bl_a(c >> 4), bl_col(c >> 6), bl_col(c >> 2), bl_b(c)); }
    n += sprintf(o + n, "a=%s;\n", bl_a(A));
    if (!(bl & BL_FORCE)) n += sprintf(o + n, "o=vec4(%s,1.0);\n", bl_col(P));
    else if (P != 1 && M != 1) n += sprintf(o + n, "o=vec4(%s*a+%s*%s,1.0);\n", bl_col(P), bl_col(M), bl_b(B));
    else if (P != 1) n += sprintf(o + n, "o=vec4(%s,a);\n", bl_col(P));
    else n += sprintf(o + n, "o=vec4(%s,a);\n", bl_col(M));
    return n;
}

static Prog *lp; // the program GL has bound
static Prog *program(uint32_t hi, uint32_t lo, uint32_t bl, int flags)
{
    static char fs[8192];
    int i, n;
    GLuint p;
    Prog *pr;
    uint32_t hb = ((hi * 2654435761u) ^ (lo * 40503u) ^ (bl * 2246822519u) ^ ((uint32_t)flags * 97u)) & (PHASH - 1);
    for (i = sPHead[hb] - 1; i >= 0; i = sP[i].next - 1) if (sP[i].hi == hi && sP[i].lo == lo && sP[i].bl == bl && sP[i].flags == flags) { sP[i].used = sFrame; return &sP[i]; }
    if (sNP < MAXP) i = sNP++;
    else { // full: drop the least recently used program
        int j, *pp; uint32_t ob;
        for (i = 0, j = 1; j < MAXP; j++) if (sFrame - sP[j].used > sFrame - sP[i].used) i = j;
        ob = ((sP[i].hi * 2654435761u) ^ (sP[i].lo * 40503u) ^ (sP[i].bl * 2246822519u) ^ ((uint32_t)sP[i].flags * 97u)) & (PHASH - 1);
        for (pp = &sPHead[ob]; *pp; pp = &sP[*pp - 1].next) if (*pp - 1 == i) { *pp = sP[i].next; break; }
        if (lp == &sP[i]) lp = NULL;
        glDeleteProgram(sP[i].prog); gPortCount[COUNT_PROGLIVE]--;
    }
    n = sprintf(fs, "#version 300 es\nprecision highp float; in vec2 vt; in vec4 vc; out vec4 o;\n"
        "uniform vec4 uPrim,uEnv,uFog; uniform vec3 uBlendC; uniform float uBlendA,uLod; uniform sampler2D uS0,uS1; uniform vec2 uScale0,uScale1;\n"
        "float noise(){ return fract(sin(dot(gl_FragCoord.xy,vec2(12.9898,78.233)))*43758.5453); }\n"
        // The RDP's bilinear is three texels: the triangle of the 2x2 quad the sample falls in. Texel
        // centres are fetched through a nearest sampler, so wrap, mirror and clamp stay the sampler's.
        "vec4 tex3(sampler2D s,vec2 st,vec2 k){ vec2 q=st-0.5; vec2 f=fract(q); vec2 b=floor(q)+0.5;\n"
        " if(f.x+f.y<1.0){ vec4 c0=texture(s,b*k); return c0+f.x*(texture(s,(b+vec2(1.0,0.0))*k)-c0)+f.y*(texture(s,(b+vec2(0.0,1.0))*k)-c0); }\n"
        " vec4 c3=texture(s,(b+1.0)*k); return c3+(1.0-f.x)*(texture(s,(b+vec2(0.0,1.0))*k)-c3)+(1.0-f.y)*(texture(s,(b+vec2(1.0,0.0))*k)-c3); }\n"
        "void main(){ vec4 t0=%s; vec4 t1=%s; vec4 cmb=vec4(0.0);\n",
        !(flags & F_TEX0) ? "vec4(1.0)" : (flags & F_3PT0) ? "tex3(uS0,vt,uScale0)" : "texture(uS0,vt*uScale0)",
        !(flags & F_TEX1) ? "t0" : (flags & F_3PT1) ? "tex3(uS1,vt,uScale1)" : "texture(uS1,vt*uScale1)");
    // the interpreter marks copy mode 0xFFFFFFFE and fill mode 0xFFFFFFFF, neither a real combine word
    if (hi >= 0xFFFFFFFE) n += sprintf(fs + n, "cmb=%s;\n", hi == 0xFFFFFFFE ? "t0" : "uPrim");
    else {
        // G_SETCOMBINE field layout
        int a0=(hi>>20)&15, c0=(hi>>15)&31, Aa0=(hi>>12)&7, Ac0=(hi>>9)&7, a1=(hi>>5)&15, c1=hi&31;
        int b0=(lo>>28)&15, b1=(lo>>24)&15, Aa1=(lo>>21)&7, Ac1=(lo>>18)&7, d0=(lo>>15)&7, Ab0=(lo>>12)&7, Ad0=(lo>>9)&7, d1=(lo>>6)&7, Ab1=(lo>>3)&7, Ad1=lo&7;
        n += sprintf(fs + n, "cmb.rgb=(%s-%s)*%s+%s; cmb.a=(%s-%s)*%s+%s;\n", rgb_a(a0), rgb_b(b0), rgb_c(c0), rgb_d(d0), a_abd(Aa0), a_abd(Ab0), a_c(Ac0), a_abd(Ad0));
        if (flags & F_2CYC) n += sprintf(fs + n, "{ vec4 prev=cmb; cmb.rgb=(%s-%s)*%s+%s; cmb.a=(%s-%s)*%s+%s; }\n", rgb_a(a1), rgb_b(b1), rgb_c(c1), rgb_d(d1), a_abd(Aa1), a_abd(Ab1), a_c(Ac1), a_abd(Ad1));
    }
    n += sprintf(fs + n, "cmb=clamp(cmb,0.0,1.0);\n");
    if (flags & F_ATEST) n += sprintf(fs + n, "if(cmb.a<=uBlendA) discard;\n");
    if (flags & F_DITHER) n += sprintf(fs + n, "if(cmb.a<noise()) discard;\n");
    if (flags & F_EDGE) n += sprintf(fs + n, "if(cmb.a<0.125) discard;\n");
    n += blender_glsl(fs + n, bl);
    sprintf(fs + n, "o=clamp(o,0.0,1.0); }\n");
    p = glCreateProgram();
    { static GLuint vs; GLuint f = compile(GL_FRAGMENT_SHADER, fs);
      if (!vs) vs = compile(GL_VERTEX_SHADER, VS);
      glAttachShader(p, vs); glAttachShader(p, f); glLinkProgram(p); glDetachShader(p, f); glDeleteShader(f); }
    pr = &sP[i];
    pr->hi = hi; pr->lo = lo; pr->bl = bl; pr->flags = flags; pr->prog = p; pr->used = sFrame;
    pr->next = sPHead[hb]; sPHead[hb] = i + 1; gPortCount[COUNT_PROGLIVE]++;
    pr->uPrim = glGetUniformLocation(p, "uPrim"); pr->uEnv = glGetUniformLocation(p, "uEnv"); pr->uFog = glGetUniformLocation(p, "uFog"); pr->uBlendC = glGetUniformLocation(p, "uBlendC");
    pr->uBlendA = glGetUniformLocation(p, "uBlendA"); pr->uLod = glGetUniformLocation(p, "uLod");
    pr->uS0 = glGetUniformLocation(p, "uS0"); pr->uS1 = glGetUniformLocation(p, "uS1");
    pr->uScale0 = glGetUniformLocation(p, "uScale0"); pr->uScale1 = glGetUniformLocation(p, "uScale1");
    return pr;
}

// One GL texture per decoded N64 texture; its sampler state is set when it changes, not per draw.
static GLuint sBound[2] = { ~0u, ~0u };
static void bind_tex(int unit, const RbTexture *t)
{
    Tx *e;
    int params, lin;
    if (!t->key) { if (sBound[unit] != sWhite) { glActiveTexture(GL_TEXTURE0 + unit); glBindTexture(GL_TEXTURE_2D, sWhite); sBound[unit] = sWhite; } return; }
    for (e = sTx[t->key >> 20]; e; e = e->next) if (e->key == t->key && e->w == t->w && e->h == t->h) break;
    if (!e) {
        e = malloc(sizeof *e); e->key = t->key; e->w = t->w; e->h = t->h; e->params = -1; gPortCount[COUNT_GLTEXLIVE]++;
        glActiveTexture(GL_TEXTURE0 + unit);
        glGenTextures(1, &e->id); glBindTexture(GL_TEXTURE_2D, e->id); sBound[unit] = e->id;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->w, t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t->rgba);
        gPortCount[COUNT_GLUPLOAD]++;
        e->next = sTx[t->key >> 20]; sTx[t->key >> 20] = e;
    } else if (sBound[unit] != e->id) { glActiveTexture(GL_TEXTURE0 + unit); glBindTexture(GL_TEXTURE_2D, e->id); sBound[unit] = e->id; gPortCount[COUNT_TEXBIND]++; }
    e->used = sFrame;
    lin = t->linear && !sFilter3; // 3-point filtering samples texel centres itself
    params = lin | (t->clamp_s != 0) << 1 | (t->mirror_s != 0) << 2 | (t->clamp_t != 0) << 3 | (t->mirror_t != 0) << 4;
    if (e->params != params) {
        e->params = params;
        glActiveTexture(GL_TEXTURE0 + unit);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, lin ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, lin ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, t->clamp_s ? GL_CLAMP_TO_EDGE : t->mirror_s ? GL_MIRRORED_REPEAT : GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, t->clamp_t ? GL_CLAMP_TO_EDGE : t->mirror_t ? GL_MIRRORED_REPEAT : GL_REPEAT);
    }
}

// GL textures unused for TX_KEEP frames are deleted (the interpreter's decoded copy has its own LRU).
#define TX_KEEP 600
static void tx_evict(void)
{
    int b;
    for (b = 0; b < 4096; b++) {
        Tx **pp = &sTx[b], *e;
        while ((e = *pp) != NULL) {
            if (sFrame - e->used > TX_KEEP) {
                int u; for (u = 0; u < 2; u++) if (sBound[u] == e->id) sBound[u] = ~0u;
                glDeleteTextures(1, &e->id); *pp = e->next; free(e);
                gPortCount[COUNT_GLTEXLIVE]--; gPortCount[COUNT_TEXEVICT]++;
            } else pp = &e->next;
        }
    }
}

static void init(void)
{
    EmscriptenWebGLContextAttributes a;
    static const uint8_t w[4] = {255,255,255,255};
    emscripten_webgl_init_context_attributes(&a);
    a.majorVersion = 2; a.minorVersion = 0; a.depth = 1; a.alpha = 0; a.antialias = 0; a.preserveDrawingBuffer = 1;
    sCtx = emscripten_webgl_create_context("#game", &a);
    emscripten_webgl_make_context_current(sCtx);
    emscripten_get_canvas_element_size("#game", &sCanvasW, &sCanvasH);
    glGenVertexArrays(1, &sVao); glBindVertexArray(sVao);
    glGenBuffers(1, &sVbo); glBindBuffer(GL_ARRAY_BUFFER, sVbo);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(RbVertex), (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(RbVertex), (void*)16);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, 0, sizeof(RbVertex), (void*)24);
    glGenTextures(1, &sWhite); glBindTexture(GL_TEXTURE_2D, sWhite);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, w);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

static void flush(void);
static void state_forget_depth(void);
static int lcm = -1; // colour mask GL holds: 1 off, 0 on

// ---- render target: the canvas, or a framebuffer (multisampled and/or 240 lines) blitted to it ----
static int sTW = 960, sTH = 720;          // the size the frame is drawn at
static GLuint sFbo, sFboColor, sFboDepth;  // the target when not drawing to the canvas directly
static GLuint sRes, sResColor;             // single-sample resolve target, for multisampled at 240 lines
static int sFboW, sFboH, sFboSamples, sFboN64, sFboBroken;
static void target_free(void)
{
    if (sFbo) { glDeleteFramebuffers(1, &sFbo); glDeleteRenderbuffers(1, &sFboColor); glDeleteRenderbuffers(1, &sFboDepth); }
    if (sRes) { glDeleteFramebuffers(1, &sRes); glDeleteRenderbuffers(1, &sResColor); }
    sFbo = sFboColor = sFboDepth = sRes = sResColor = 0;
}
static GLuint rbuf(int samples, GLenum fmt, int w, int h)
{
    GLuint r; glGenRenderbuffers(1, &r); glBindRenderbuffer(GL_RENDERBUFFER, r);
    if (samples) glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, fmt, w, h); else glRenderbufferStorage(GL_RENDERBUFFER, fmt, w, h);
    return r;
}
// Picks the target for this frame. Anything unsupported (no multisampling, an incomplete framebuffer)
// falls back to drawing into the canvas as before, once, for good.
static void target_begin(void)
{
    int samples = 0, w = sCanvasW, h = sCanvasH;
    static GLint mx = -1;
    if (mx < 0) glGetIntegerv(GL_MAX_SAMPLES, &mx);
    if (sAA && !sFboBroken) samples = mx >= 4 ? 4 : mx >= 2 ? mx : 0;
    if (sN64 && !sFboBroken) { h = 240; w = (int)(240.0f * sCanvasW / (sCanvasH ? sCanvasH : 1) + 0.5f); if (w > sCanvasW) w = sCanvasW; if (h > sCanvasH) h = sCanvasH; }
    if (!samples && !(sN64 && !sFboBroken)) { if (sFbo) target_free(); sFboW = 0; glBindFramebuffer(GL_FRAMEBUFFER, 0); sTW = sCanvasW; sTH = sCanvasH; return; }
    if (!sFbo || sFboW != w || sFboH != h || sFboSamples != samples || sFboN64 != sN64) {
        target_free();
        glGenFramebuffers(1, &sFbo); glBindFramebuffer(GL_FRAMEBUFFER, sFbo);
        sFboColor = rbuf(samples, GL_RGBA8, w, h); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, sFboColor);
        sFboDepth = rbuf(samples, GL_DEPTH_COMPONENT24, w, h); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sFboDepth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) goto broken;
        if (samples) { // a multisampled blit cannot scale or convert formats (the canvas is RGB): resolve first
            glGenFramebuffers(1, &sRes); glBindFramebuffer(GL_FRAMEBUFFER, sRes);
            sResColor = rbuf(0, GL_RGBA8, w, h); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, sResColor);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) goto broken;
        }
        sFboW = w; sFboH = h; sFboSamples = samples; sFboN64 = sN64;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, sFbo); sTW = w; sTH = h;
    return;
broken:
    fprintf(stderr, "renderer: framebuffer target unavailable, drawing to the canvas\n");
    target_free(); sFboBroken = 1; sFboW = 0; glBindFramebuffer(GL_FRAMEBUFFER, 0); sTW = sCanvasW; sTH = sCanvasH;
}
// The frame goes to the canvas: resolve (and scale, unfiltered) what the framebuffer holds.
static void target_end(void)
{
    if (!sFbo) return;
    glDisable(GL_SCISSOR_TEST); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); lcm = 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sFbo);
    if (sRes) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sRes);
        glBlitFramebuffer(0, 0, sFboW, sFboH, 0, 0, sFboW, sFboH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sRes);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, sFboW, sFboH, 0, 0, sCanvasW, sCanvasH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, sFbo);
    glEnable(GL_SCISSOR_TEST);
}

void rb_begin_frame(void)
{
    if (!sCtx) init();
    flush();
    if (++sFrame % 60 == 0) tx_evict();
    target_begin();
    glDepthMask(GL_TRUE); state_forget_depth();
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); lcm = 0;
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
}

// A depth clear in mid-frame: draw what came before it first. The depth mask it sets is re-applied by the next batch.
static void state_forget_depth(void);
void rb_clear_depth(void)
{
    flush();
    glDisable(GL_SCISSOR_TEST); glDepthMask(GL_TRUE); glClearDepthf(1.0f); glClear(GL_DEPTH_BUFFER_BIT); glEnable(GL_SCISSOR_TEST);
    state_forget_depth();
}

// ---- batching: triangles and rectangles with the same state go out in one draw, in order ----
#define MAXV 16384
static RbVertex sBuf[MAXV]; static int sN;
static RbState sCur; static int sHave; static uint32_t sCurSerial;
static Prog *sProg;

// What GL holds now, so a batch only sets what differs from the batch before it.
static int lb = -1, ld = -1, ldecal = -1; static GLint lvp[4] = {-1,-1,-1,-1}, lsc[4] = {-1,-1,-1,-1};
static float lprim[4], lenv[4], lfog[4], lblc[3], lblend, llod, lscale[2][2];
static void state_forget_depth(void) { ld = -1; }

// Apply a draw state to GL. Called once per batch, and skips what the previous batch already set.
static void apply(const RbState *st)
{
    int flags = 0;
    // N64 pixels to canvas pixels: a span of the whole picture stretches to the canvas, anything else sits in
    // the centred 4:3 band. At 4:3 both are the same mapping.
    float sy = sTH / 240.0f, band = sTH * 4.0f / 3.0f, off = (sTW - band) * 0.5f;
    float vsx = st->wide_vp ? sTW / 320.0f : sy, vox = st->wide_vp ? 0 : off;
    float ssx = st->wide_sc ? sTW / 320.0f : sy, sox = st->wide_sc ? 0 : off;
    GLint vp[4], sc[4];
    Prog *p;
    int b, d;
    if (st->alpha_compare == 1) flags |= F_ATEST;
    if (st->alpha_compare == 2) flags |= F_DITHER;
    if (st->tex_edge) flags |= F_EDGE;
    if (st->two_cycle) flags |= F_2CYC;
    if (st->tex[0].key) flags |= F_TEX0 | (sFilter3 && st->tex[0].linear ? F_3PT0 : 0);
    if (st->tex[1].key) flags |= F_TEX1 | (sFilter3 && st->tex[1].linear ? F_3PT1 : 0);
    p = program(st->combine_hi, st->combine_lo, st->blender, flags);
    if (p != lp) {
        glUseProgram(p->prog); glUniform1i(p->uS0, 0); glUniform1i(p->uS1, 1);
        gPortCount[COUNT_PROGSWITCH]++; lp = p;
        // uniforms live per program: forget what was set
        memset(lprim, 0x7f, sizeof lprim); memset(lenv, 0x7f, sizeof lenv); memset(lfog, 0x7f, sizeof lfog); memset(lblc, 0x7f, sizeof lblc); memset(lscale, 0x7f, sizeof lscale); lblend = llod = -1;
    }
    if (memcmp(lprim, st->prim, sizeof lprim)) { glUniform4fv(p->uPrim, 1, st->prim); memcpy(lprim, st->prim, sizeof lprim); }
    if (memcmp(lenv, st->env, sizeof lenv)) { glUniform4fv(p->uEnv, 1, st->env); memcpy(lenv, st->env, sizeof lenv); }
    if (memcmp(lfog, st->fogcolor, sizeof lfog)) { glUniform4fv(p->uFog, 1, st->fogcolor); memcpy(lfog, st->fogcolor, sizeof lfog); }
    if (memcmp(lblc, st->blendcolor, sizeof lblc)) { glUniform3fv(p->uBlendC, 1, st->blendcolor); memcpy(lblc, st->blendcolor, sizeof lblc); }
    if (lblend != st->blend_a) { glUniform1f(p->uBlendA, st->blend_a); lblend = st->blend_a; }
    if (llod != st->prim_lod) { glUniform1f(p->uLod, st->prim_lod); llod = st->prim_lod; }
    bind_tex(0, &st->tex[0]); bind_tex(1, &st->tex[1]);
    { float s0[2] = { st->tex[0].w ? 1.0f / st->tex[0].w : 1, st->tex[0].h ? 1.0f / st->tex[0].h : 1 }, s1[2] = { st->tex[1].w ? 1.0f / st->tex[1].w : 1, st->tex[1].h ? 1.0f / st->tex[1].h : 1 };
      if (memcmp(lscale[0], s0, sizeof s0)) { glUniform2f(p->uScale0, s0[0], s0[1]); memcpy(lscale[0], s0, sizeof s0); }
      if (memcmp(lscale[1], s1, sizeof s1)) { glUniform2f(p->uScale1, s1[0], s1[1]); memcpy(lscale[1], s1, sizeof s1); } }
    b = st->blend ? 1 | st->bsrc << 1 | st->bdst << 3 : 0;
    if (b != lb) {
        static const GLenum F[4] = { GL_ZERO, GL_ONE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA };
        if (b) { if (!(lb > 0)) glEnable(GL_BLEND); glBlendFunc(F[st->bsrc], F[st->bdst]); } else glDisable(GL_BLEND);
        lb = b; gPortCount[COUNT_STATE_BLEND]++;
    }
    if (st->color_off != lcm) { GLboolean c = st->color_off ? GL_FALSE : GL_TRUE; glColorMask(c, c, c, c); lcm = st->color_off; }
    d = (st->depth_test << 2) | (st->depth_write << 1) | st->decal;
    if (d != ld) {
        if (st->depth_test) { glEnable(GL_DEPTH_TEST); glDepthFunc(st->decal ? GL_LEQUAL : GL_LESS); } else if (st->depth_write) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); } else glDisable(GL_DEPTH_TEST);
        glDepthMask(st->depth_write ? GL_TRUE : GL_FALSE);
        ld = d; gPortCount[COUNT_STATE_DEPTH]++;
    }
    if (st->decal != ldecal) { if (st->decal) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-2, -2); } else glDisable(GL_POLYGON_OFFSET_FILL); ldecal = st->decal; }
    vp[0] = (GLint)(vox + st->viewport[0] * vsx); vp[1] = (GLint)((240 - st->viewport[1] - st->viewport[3]) * sy); vp[2] = (GLsizei)(st->viewport[2] * vsx); vp[3] = (GLsizei)(st->viewport[3] * sy);
    sc[0] = (GLint)(sox + st->scissor[0] * ssx); sc[1] = (GLint)((240 - st->scissor[3]) * sy); sc[2] = (GLsizei)((st->scissor[2] - st->scissor[0]) * ssx); sc[3] = (GLsizei)((st->scissor[3] - st->scissor[1]) * sy);
    if (memcmp(vp, lvp, sizeof vp)) { glViewport(vp[0], vp[1], vp[2], vp[3]); memcpy(lvp, vp, sizeof vp); }
    if (memcmp(sc, lsc, sizeof sc)) { glScissor(sc[0], sc[1], sc[2], sc[3]); memcpy(lsc, sc, sizeof sc); }
}

static void flush(void)
{
    double t0;
    if (!sN) return;
    t0 = port_now();
    apply(&sCur);
    glBufferData(GL_ARRAY_BUFFER, sN * sizeof(RbVertex), sBuf, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, sN);
    gPortCount[COUNT_DRAWS]++; gPortCount[COUNT_BUFBYTES] += sN * sizeof(RbVertex);
    sN = 0;
    gPortProf[PROF_RBDRAW] += port_now() - t0;
}

void rb_draw(const RbState *st, uint32_t serial, const RbVertex *v, int n)
{
    // The state behind a serial never changes, so a new serial is the only reason to start a batch.
    if (!sHave || serial != sCurSerial) { flush(); sCur = *st; sCurSerial = serial; sHave = 1; }
    if (sN + n > MAXV) flush();
    memcpy(&sBuf[sN], v, n * sizeof *v);
    sN += n;
}

void rb_end_frame(void) { flush(); target_end(); }

void port_set_canvas_size(int w, int h) { sCanvasW = w; sCanvasH = h; }
