// WebGL2 backend. Combiner modes become fragment shaders, cached by mode.
#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gfx.h"

typedef struct { uint32_t hi, lo; int flags; GLuint prog; GLint uPrim, uEnv, uFog, uBlendA, uLod, uS0, uS1, uScale0, uScale1; } Prog;
#define MAXP 512
static Prog sP[MAXP]; static int sNP;
static GLuint sVbo, sVao; static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE sCtx;
static int sCanvasW = 960, sCanvasH = 720;

typedef struct Tx { uint32_t key; int w, h; GLuint id; struct Tx *next; } Tx;
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

enum { F_FOG = 1, F_ATEST = 2, F_EDGE = 4, F_2CYC = 8, F_TEX0 = 16, F_TEX1 = 32 };

static Prog *program(uint32_t hi, uint32_t lo, int flags)
{
    static char fs[8192];
    int i, n;
    GLuint p;
    Prog *pr;
    for (i = 0; i < sNP; i++) if (sP[i].hi == hi && sP[i].lo == lo && sP[i].flags == flags) return &sP[i];
    if (sNP >= MAXP) sNP = 0;
    n = sprintf(fs, "#version 300 es\nprecision highp float; in vec2 vt; in vec4 vc; out vec4 o;\n"
        "uniform vec4 uPrim,uEnv; uniform vec3 uFog; uniform float uBlendA,uLod; uniform sampler2D uS0,uS1; uniform vec2 uScale0,uScale1;\n"
        "float noise(){ return fract(sin(dot(gl_FragCoord.xy,vec2(12.9898,78.233)))*43758.5453); }\n"
        "void main(){ vec4 t0=%s; vec4 t1=%s; vec4 cmb=vec4(0.0);\n",
        (flags & F_TEX0) ? "texture(uS0,vt*uScale0)" : "vec4(1.0)", (flags & F_TEX1) ? "texture(uS1,vt*uScale1)" : "t0");
    // the interpreter marks copy mode 0xFFFFFFFE and fill mode 0xFFFFFFFF, neither a real combine word
    if (hi >= 0xFFFFFFFE) n += sprintf(fs + n, "cmb=%s;\n", hi == 0xFFFFFFFE ? "t0" : "uPrim");
    else {
        // G_SETCOMBINE field layout
        int a0=(hi>>20)&15, c0=(hi>>15)&31, Aa0=(hi>>12)&7, Ac0=(hi>>9)&7, a1=(hi>>5)&15, c1=hi&31;
        int b0=(lo>>28)&15, b1=(lo>>24)&15, Aa1=(lo>>21)&7, Ac1=(lo>>18)&7, d0=(lo>>15)&7, Ab0=(lo>>12)&7, Ad0=(lo>>9)&7, d1=(lo>>6)&7, Ab1=(lo>>3)&7, Ad1=lo&7;
        n += sprintf(fs + n, "cmb.rgb=(%s-%s)*%s+%s; cmb.a=(%s-%s)*%s+%s;\n", rgb_a(a0), rgb_b(b0), rgb_c(c0), rgb_d(d0), a_abd(Aa0), a_abd(Ab0), a_c(Ac0), a_abd(Ad0));
        if (flags & F_2CYC) n += sprintf(fs + n, "{ vec4 prev=cmb; cmb.rgb=(%s-%s)*%s+%s; cmb.a=(%s-%s)*%s+%s; }\n", rgb_a(a1), rgb_b(b1), rgb_c(c1), rgb_d(d1), a_abd(Aa1), a_abd(Ab1), a_c(Ac1), a_abd(Ad1));
    }
    if (flags & F_FOG) n += sprintf(fs + n, "cmb.rgb=mix(cmb.rgb,uFog,vc.a);\n");
    if (flags & F_ATEST) n += sprintf(fs + n, "if(cmb.a<=uBlendA) discard;\n");
    if (flags & F_EDGE) n += sprintf(fs + n, "if(cmb.a<0.125) discard;\n");
    sprintf(fs + n, "o=clamp(cmb,0.0,1.0); }\n");
    p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, VS)); glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    pr = &sP[sNP++];
    pr->hi = hi; pr->lo = lo; pr->flags = flags; pr->prog = p;
    pr->uPrim = glGetUniformLocation(p, "uPrim"); pr->uEnv = glGetUniformLocation(p, "uEnv"); pr->uFog = glGetUniformLocation(p, "uFog");
    pr->uBlendA = glGetUniformLocation(p, "uBlendA"); pr->uLod = glGetUniformLocation(p, "uLod");
    pr->uS0 = glGetUniformLocation(p, "uS0"); pr->uS1 = glGetUniformLocation(p, "uS1");
    pr->uScale0 = glGetUniformLocation(p, "uScale0"); pr->uScale1 = glGetUniformLocation(p, "uScale1");
    return pr;
}

static void bind_tex(int unit, const RbTexture *t)
{
    Tx *e;
    glActiveTexture(GL_TEXTURE0 + unit);
    if (!t->key) { glBindTexture(GL_TEXTURE_2D, sWhite); return; }
    for (e = sTx[t->key >> 20]; e; e = e->next) if (e->key == t->key && e->w == t->w && e->h == t->h) break;
    if (!e) {
        e = malloc(sizeof *e); e->key = t->key; e->w = t->w; e->h = t->h;
        glGenTextures(1, &e->id); glBindTexture(GL_TEXTURE_2D, e->id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->w, t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t->rgba);
        e->next = sTx[t->key >> 20]; sTx[t->key >> 20] = e;
    } else glBindTexture(GL_TEXTURE_2D, e->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, t->linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, t->linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, t->clamp_s ? GL_CLAMP_TO_EDGE : t->mirror_s ? GL_MIRRORED_REPEAT : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, t->clamp_t ? GL_CLAMP_TO_EDGE : t->mirror_t ? GL_MIRRORED_REPEAT : GL_REPEAT);
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

void rb_begin_frame(void)
{
    if (!sCtx) init();
    glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
}

void rb_clear_depth(void)
{
    glDisable(GL_SCISSOR_TEST); glDepthMask(GL_TRUE); glClearDepthf(1.0f); glClear(GL_DEPTH_BUFFER_BIT); glEnable(GL_SCISSOR_TEST);
}

void rb_draw(const RbState *st, const RbVertex *v, int n)
{
    int flags = 0;
    float sx = sCanvasW / 320.0f, sy = sCanvasH / 240.0f;
    Prog *p;
    if (st->fog) flags |= F_FOG;
    if (st->alpha_compare == 1) flags |= F_ATEST;
    if (st->alpha_compare == 2) flags |= F_EDGE;
    if (st->two_cycle) flags |= F_2CYC;
    if (st->tex[0].key) flags |= F_TEX0;
    if (st->tex[1].key) flags |= F_TEX1;
    p = program(st->combine_hi, st->combine_lo, flags);
    glUseProgram(p->prog);
    glUniform4fv(p->uPrim, 1, st->prim); glUniform4fv(p->uEnv, 1, st->env); glUniform3fv(p->uFog, 1, st->fogcolor);
    glUniform1f(p->uBlendA, st->blend_a); glUniform1f(p->uLod, st->prim_lod);
    glUniform1i(p->uS0, 0); glUniform1i(p->uS1, 1);
    bind_tex(0, &st->tex[0]); bind_tex(1, &st->tex[1]);
    glUniform2f(p->uScale0, st->tex[0].w ? 1.0f / st->tex[0].w : 1, st->tex[0].h ? 1.0f / st->tex[0].h : 1);
    glUniform2f(p->uScale1, st->tex[1].w ? 1.0f / st->tex[1].w : 1, st->tex[1].h ? 1.0f / st->tex[1].h : 1);
    if (st->alpha_blend) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); } else glDisable(GL_BLEND);
    if (st->depth_test) { glEnable(GL_DEPTH_TEST); glDepthFunc(st->decal ? GL_LEQUAL : GL_LESS); } else if (st->depth_write) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); } else glDisable(GL_DEPTH_TEST);
    glDepthMask(st->depth_write ? GL_TRUE : GL_FALSE);
    if (st->decal) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-2, -2); } else glDisable(GL_POLYGON_OFFSET_FILL);
    glViewport((GLint)(st->viewport[0] * sx), (GLint)((240 - st->viewport[1] - st->viewport[3]) * sy), (GLsizei)(st->viewport[2] * sx), (GLsizei)(st->viewport[3] * sy));
    glScissor((GLint)(st->scissor[0] * sx), (GLint)((240 - st->scissor[3]) * sy), (GLsizei)((st->scissor[2] - st->scissor[0]) * sx), (GLsizei)((st->scissor[3] - st->scissor[1]) * sy));
    glBufferData(GL_ARRAY_BUFFER, n * sizeof(RbVertex), v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, n);
}

void port_set_canvas_size(int w, int h) { sCanvasW = w; sCanvasH = h; }
