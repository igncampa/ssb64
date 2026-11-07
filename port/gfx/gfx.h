// Interpreter to backend interface.
#ifndef PORT_GFX_H
#define PORT_GFX_H
#include <stdint.h>

typedef struct RbVertex { float x, y, z, w, u, v, r, g, b, a; } RbVertex;

typedef struct RbTexture {
    uint32_t key;      // content hash, 0 = none
    int w, h;
    const uint8_t *rgba;
    int clamp_s, clamp_t, mirror_s, mirror_t;
    int linear;
    float uls, ult;
} RbTexture;

typedef struct RbState {
    uint32_t combine_hi, combine_lo;  // G_SETCOMBINE words
    int two_cycle;
    int alpha_blend;
    int fog;
    int alpha_compare;       // 0 none, 1 threshold against blend alpha, 2 cvg (texture edge)
    int depth_test, depth_write, decal;
    float prim[4], env[4], fogcolor[3], blend_a, prim_lod;
    int scissor[4];          // x0, y0, x1, y1 in N64 pixels
    int viewport[4];         // x, y, w, h in N64 pixels, y down
    int rect;                // screen-space quad, positions already in NDC
    int wide_vp, wide_sc;    // widescreen: the viewport / scissor spans the picture (>= 300 px) and is stretched
                             // to the canvas; otherwise it sits in the centred 4:3 band (proof/widescreen-spec.md)
    RbTexture tex[2];
} RbState;

void rb_begin_frame(void);
void rb_end_frame(void);   // the last batch of the frame goes out
void rb_clear_depth(void);
void rb_draw(const RbState *st, const RbVertex *v, int n);
void port_set_canvas_size(int w, int h);
#endif
