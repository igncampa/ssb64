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
    int tile_w, tile_h;  // > 0: clamp to this many texels in the shader (G_TX_CLAMP on a tile larger than its mask)
} RbTexture;

// Blender field codes: P/M colour 0 IN, 1 MEM, 2 BL, 3 FOG; A 0 A_IN, 1 A_FOG, 2 SHADE, 3 zero;
// B 0 1-A, 1 A_MEM, 2 one, 3 zero. A cycle's code is P<<6 | A<<4 | M<<2 | B.
#define BL_C1(code)   ((uint32_t)(code) | 0x100u)
#define BL_FIN(code)  ((uint32_t)(code) << 9)
#define BL_FORCE      (1u << 17)
#define BL_AIN1       (1u << 18) // ALPHA_CVG_SEL without CVG_X_ALPHA: the blender's A_IN is coverage (1)
enum { BLF_ZERO, BLF_ONE, BLF_SRC_ALPHA, BLF_ONE_MINUS_SRC_ALPHA };
typedef struct RbState {
    uint32_t combine_hi, combine_lo;  // G_SETCOMBINE words
    int two_cycle;
    // The RDP blender, decoded by the interpreter (f3dex2.c decode_blender). blender is the shader's part:
    // BL_C1 (2-cycle first cycle, evaluated in the shader), BL_FIN (the final cycle's P,A,M,B), BL_FORCE,
    // BL_AIN1. blend/bsrc/bdst/color_off are what GL does with the shader's output (BLF_* factors).
    uint32_t blender;
    int blend, bsrc, bdst, color_off;
    int alpha_compare;       // 0 none, 1 threshold against blend alpha, 2 dither (noise)
    int tex_edge;            // CVG_X_ALPHA: a pixel whose coverage x alpha rounds to 0 is not written
    int depth_test, depth_write, decal;
    float prim[4], env[4], fogcolor[4], blendcolor[3], blend_a, prim_lod;
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
// serial names the state's contents: the interpreter gives a new one only when the state changed, so the
// backend batches on an integer compare instead of comparing the whole struct per triangle.
void rb_draw(const RbState *st, uint32_t serial, const RbVertex *v, int n);
void port_set_canvas_size(int w, int h);
#endif
