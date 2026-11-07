// Frame profile counters (prof.c). Milliseconds by phase, then counts of what the renderer did.
#ifndef PORT_PROF_H
#define PORT_PROF_H
#include <stdint.h>
enum {
    PROF_UPDATE,     // the game's own frame update (simulation), ms
    PROF_DLBUILD,    // the game's draw pass: building display lists, ms
    PROF_GFXRUN,     // port_gfx_run: interpreting the lists and submitting GL, ms
    PROF_RBDRAW,     // of which inside rb_draw (GL calls), ms
    PROF_TEXDECODE,  // of which decoding texels for textures first seen, ms
    PROF_RESIM,      // frames re-simulated by a rollback (no draw), ms
    PROF_STEP,       // all of port_step, ms
    PROF_AUDIO,      // the audio tick after a step, ms
    PROF_N
};
enum {
    COUNT_FRAMES_DRAWN, COUNT_OPS, COUNT_TRIS, COUNT_RECTS, COUNT_DRAWS, COUNT_VTX, COUNT_TEXDECODE,
    COUNT_TEXLOOKUP, COUNT_GLUPLOAD, COUNT_PROGSWITCH, COUNT_TEXBIND, COUNT_BUFBYTES, COUNT_RESIM_FRAMES,
    COUNT_SIM_FRAMES, COUNT_STATE_BLEND, COUNT_STATE_DEPTH, COUNT_TEXHASH,
    // gauges, not running totals: what the caches hold now
    COUNT_TEXLIVE, COUNT_GLTEXLIVE, COUNT_PROGLIVE,
    COUNT_TEXEVICT, // running total: textures (decoded or GL) dropped by the LRU
    COUNT_STATEBUILD, // running total: times the interpreter rebuilt the draw state
    COUNT_CLIPPED,    // running total: triangles cut by the near plane
    COUNT_REJECTED,   // running total: triangles wholly outside one side of the view volume, dropped
    COUNT_FARCUT,     // running total: triangles partly beyond the far plane (WebGL2 has no depth clamp: GL cuts them)
    // why a new state serial began (the batch boundaries the interpreter causes), running totals
    COUNT_SPLIT_COMBINE, COUNT_SPLIT_BLEND, COUNT_SPLIT_COLOUR, COUNT_SPLIT_TEXTURE, COUNT_SPLIT_DEPTH, COUNT_SPLIT_VIEW, COUNT_SPLIT_OTHER,
    COUNT_N
};
extern double gPortProf[PROF_N];
extern uint32_t gPortCount[COUNT_N];
double port_now(void);
#endif
