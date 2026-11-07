# The renderer: N64 display lists to WebGL2

Analysis of the whole path from the game's `Gfx` lists to the canvas, with measurements, what is wrong or
missing against the real F3DEX2/RDP, and an ordered plan. Analysis only: the two prototype fixes on this
branch (`proof/gfx/README.md`) are kept separate and are the branch's only code changes.

What must not change, in every phase: the simulation. Rendering reads the game's lists and memory and
writes nothing the game reads; `proof/state-machines/hashlogs.sh` must give the same 57 logs before and
after, and `web/tests/net_proof.mjs` must still agree hash for hash. Render-only state (caches, batches,
timers) lives in `port/gfx` and `port/loop/prof.c`, never in a snapshot.

## 1. Where the time goes

Method: `port/loop/prof.c` accumulates milliseconds per phase and counts per frame; `PortProf(i)` reads it,
`PROF=1` prints it from the node runner, `proof/gfx/perf.mjs` from a headless browser. Numbers below are
from `proof/gfx/README.md` (full tables there). CPU time inside the wasm, per frame, one machine.

| phase | node, no GL | browser, before | browser, after this branch |
|---|---|---|---|
| sim update (`task_update`) | 0.02 ms | 0.10-0.21 ms | same |
| display-list build (`task_draw`) | 0.01 | 0.06-0.14 | same |
| interpreter without GL (`port_gfx_run` minus `rb_draw`) | 0.34-0.75 (0.24-0.30 after the memo) | 1.0-1.5 | 0.75-0.95 |
| GL submission (`rb_draw`) | 0 | 1.7-2.4 | 0.44-0.67 |
| one drawn frame, JS wall time | 0.5-0.8 | 2.9-4.5 | 1.5-2.1 |
| 12-frame rollback (restore + 12 resims, no draw) | 0.12-0.16 | | |

Counts per frame (Final Destination, 4 fighters): 4,800-5,400 list ops, 950-1,220 triangles, 64 rectangles,
1,000-1,300 vertices loaded, ~1,000-1,200 texture lookups; before: 583-778 GL draws, ~1,000 texture binds,
40-70 program switches; after: 130-180 draws, ~150-180 binds.

Reading: the simulation and the game's own draw pass are negligible, and rollbacks cost less than a tenth
of a frame; a frame's CPU is the renderer, and most of the renderer was GL call overhead from drawing one
triangle at a time with the whole state re-sent. The remaining interpreter time is per-vertex transform in
scalar C (`do_vtx`), per-triangle `fill_state` + `use_textures` + state compare, and texture key lookups.
Under SwiftShader the GL half is CPU; on a real GPU it is driver/GPU-process time of the same order per
call, so the browser "before" column is a fair picture of a laptop without a discrete GPU.

What this says about the owner's "slightly laggy" Meta Crystal with Metal Mario: Meta Crystal is one of
the cheaper stages; Metal Mario added 7-15% draws and 0.2-0.3 ms of texel hashing a frame (his chrome map
is 5x a normal fighter's texture and was hashed once per triangle). On a machine near its budget that is
a visible step. The branch halves the frame; phase 1 below takes most of the rest.

## 2. The path today

```
game (src/) ──Gfx list in gSYTaskmanGraphicsHeap──► port/loop/sched.c port_sched_drain
   ──► port/gfx/f3dex2.c port_gfx_run: run() walks the list
         vertices: do_vtx (MVP, lighting, texgen, fog) → S.vtx[64]
         triangles: emit_tri → fill_state (RbState) + use_textures (import_tile → decode → RGBA8 cache) → rb_draw
         rectangles: do_rect → rb_draw (positions already NDC)
   ──► port/gfx/rb_webgl2.c: program(combine words, flags) builds one GLSL fragment shader per combiner;
         rb_draw batches by RbState (this branch), apply() sets GL, glDrawArrays
```

`RbState` (`port/gfx/gfx.h`) is the whole contract: combiner words, two-cycle, blend on/off, fog, alpha
compare mode, depth test/write/decal, prim/env/fog colours, blend alpha, prim LOD, scissor, viewport,
rect flag, widescreen flags, two textures (key, size, RGBA pointer, clamp/mirror, linear, tile origin).

## 3. What is wrong or missing against F3DEX2/RDP

Ranked by what a player sees, then by risk. Line numbers are this branch's.

1. **Blender modes are reduced to on/off** (`f3dex2.c:343-348`, `rb_webgl2.c apply()`). `alpha_blend` is
   `FORCE_BL && (b-input is 1MA)` and always `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`; `decal` is ZMODE_DEC via
   polygon offset; `fog` is a vertex-alpha mix. Not represented: `CLR_ON_CVG`, `CVG_X_ALPHA`,
   `ALPHA_CVG_SEL`, `IM_RD` with `AA_EN` (coverage-as-alpha, which many N64 translucent effects use),
   additive blends (`G_BL_1` with `G_BL_CLR_MEM`), `ZMODE_XLU`/`ZMODE_INTER`, `Z_CMP` without `Z_UPD`
   handled only through `depth_test`/`depth_write` (fine), and the fog cycle in two-cycle modes
   (`st->fog` is taken only when cycle 1's P is fog; cycle 2 `G_BL_CLR_FOG` is ignored).
   Evidence: the crystal material is `oml=00552048` (AA_EN, IM_RD, ALPHA_CVG_SEL, CVG_DST_CLAMP, no
   FORCE_BL): opaque here, as on hardware because its palette alpha is 1-bit; a texture with soft alpha in
   that mode would be opaque here and translucent on the N64. libultraship decodes the same words into
   `use_alpha`, `use_fog`, `use_noise`, `alpha_threshold` plus `ZMODE` handling in the shader key.
2. **No near-plane clipping** (`emit_tri`, `f3dex2.c:366`). F3DEX2 clips triangles against the near plane
   (and the guard band); the port hands clip-space triangles to GL and lets GL clip. GL clips the same
   volume, but vertices with `w <= 0` are passed through with the trace's cull test skipped
   (`f3dex2.c:376`). At match start on Final Destination the appear effects draw triangles whose
   trace boxes reach y = -4124 NDC (`GFXTRACE=296`): oversized orange streaks for a few frames
   (`proof/gfx/meta_crystal_go_before.png` shows the same on Meta Crystal). Needs a check against
   hardware; a Sutherland-Hodgman near clip in `emit_tri` (libultraship `GfxSpTriUpdateState`/clip) would
   make this exact.
3. **Texture filtering is bilinear, not 3-point** (`rb_webgl2.c:101`, `use_textures f3dex2.c:361`).
   `G_TF_BILERP` and `G_TF_AVERAGE` both become `GL_LINEAR`. The N64 samples three texels; the look is a
   known difference (diagonal gradients on magnified textures: the "GO!" text, HUD portraits, stage
   textures up close).
4. **No anti-aliasing / coverage** (`rb_webgl2.c:113 antialias = 0`). The RDP's edge AA and the VI's
   divot/dither filter are absent. Edges are hard at canvas resolution.
5. **Texture LOD / `G_TL_LOD` / mipmaps / `prim_lod` as LOD fraction** are not implemented
   (`st->prim_lod` is only the combiner's `PRIM_LOD_FRAC`; `G_CCMUX_LOD_FRACTION` maps to `uLod` too,
   `rb_webgl2.c:27`). The game uses LOD rarely; low priority.
6. **Noise** (`rb_webgl2.c:53`) is a per-pixel hash, not the RDP's dithered noise; `G_ACMUX`/`G_CCMUX`
   noise users (the GO! text) look grainy rather than animated speckle. Cosmetic.
7. **`G_TEXTURE_GEN_LINEAR`** is defined (`f3dex2.c:23 G_TEXGENLIN_`) and never used; the game's data
   does not set it in the matches traced, so no visible effect today.
8. **Texture coordinate detail**: `shifts > 10` and `uls/ult` offsets are handled (`f3dex2.c:391-393`),
   but `G_TX_CLAMP` with a tile smaller than the mask is approximated (`import_tile:315`), and
   `LOADTILE` of 4-bit images assumes `(w+1)/2` strides (`f3dex2.c:560`). No known wrong picture from
   these, but they are the places to look if a texture seam shows.
9. **Depth**: GL depth is clip `z/w` in `[-1,1]`; the N64's 16-bit Z with `dz` and `G_ZS_PRIM`
   (`OP_PRIMDEPTH f3dex2.c:592`) is mapped linearly. Decal uses `glPolygonOffset(-2,-2)`. Shadows and
   platform decals look right; z-fighting on coplanar stage pieces may differ from hardware.
10. **Caches never evict** (`sTexCache` in `f3dex2.c`, `sTx` and `sP` in `rb_webgl2.c:11 MAXP 512`, wrap
    without deleting programs). A long session with palette-animated textures grows memory (336-366 new
    decodes per 1500 frames in the profile, each a GL texture). Needs an LRU with a frame stamp.
11. **Per-vertex work in scalar C** (`do_vtx`): fine at 1,300 vertices a frame; not a bottleneck.
12. **Rectangles bypass the viewport** (`do_rect f3dex2.c:402`, positions in NDC of a 320x240 picture):
    correct for this game, which never draws rectangles under a non-full viewport.

Verified correct on this branch (do not re-investigate): texgen formula and lookat transform
(`proof/gfx/README.md` section 1), light direction transform, combiner decode (`rb_webgl2.c:56-62`
matches the G_SETCOMBINE layout), CI/IA/I/RGBA texel decode and the dxt-0 row shuffle, TLUT loads,
`G_MW_MATRIX` billboard words (`mvp_word`), matrix stack, `gSPModifyVertex` for texture coordinates.

## 4. Options

### A. Keep `port/gfx/f3dex2.c` + `rb_webgl2.c`, fix and extend (recommended)

851 lines today, all of it readable, the whole contract in one struct, no dependencies, builds in the
existing `Makefile.port`. The prototype on this branch already removes the main cost (batching, state
cache, texture memo) in ~150 lines. The missing pieces in section 3 are each a bounded change:
blender decode into a shader key (1), near clip (2), 3-point filter in the fragment shader (3), MSAA by
rendering into a multisampled FBO (4), an LRU (10). Estimated: 1-2 weeks of focused work for phases 1-3
below, each independently provable by screenshot diff and the hash logs. Risk: reproducing RDP blender
semantics by hand; mitigated by taking the decode tables from libultraship, which is BSD-licensed and
well tested on these exact games (SM64, OoT, MK64).

### B. Adopt libultraship's Fast3D (`/home/desktop/mk64/SpaghettiKart/libultraship/src/fast`)

`interpreter.cpp` 5,468 lines plus `backends/gfx_opengl.cpp` 1,111 lines (the `SpaghettiKart-wasm`
copy has `__EMSCRIPTEN__` paths for WebGL2), C++17, tied to the rest of libultraship: `Ship::Context`,
resource manager, ImGui windows, SDL2 window/audio, spdlog, nlohmann json, the framebuffer-effects
pipeline. It brings: full blender/combiner decode, 3-point filtering (`FILTER_THREE_POINT`), near
clipping, texture cache with eviction, batching by shader state, F3DEX2 + S2DEX + extended opcodes,
framebuffer capture (used for OoT's pause screen), MSAA, HD texture replacement. Cost: carving it out of
libultraship (or linking the whole library into the wasm: +1-2 MB, C++ runtime, exceptions off, SDL
shims), replacing its window/context management with the page's canvas, keeping the port's widescreen
and the `cimg`/`zimg` magnifier logic, and the proof harness (`GfxTrace`, `GfxCoverage`, `port_gfx_tris`)
would have to be rebuilt on its hooks. Estimated 2-4 weeks before it draws a frame as well as the current
renderer does, and the codebase doubles in surface for a game that uses a small subset of what it
offers. Its interpreter is also not faster per triangle than the batched one on this branch (same
structure: state key, buffered triangles, flush on change). Worth it only if the project wants its
feature set wholesale (HD textures, framebuffer effects) rather than correctness on this game.

### C. WebGPU

A new backend behind the same `RbState` contract would be ~400-600 lines (pipeline cache keyed by the
shader key + blend/depth state, bind groups per texture pair, one vertex buffer per frame, WGSL
combiner generation). Gains: fewer per-call crossings (render passes, pipeline objects), explicit
multisampling, and no WebGL state machine. Costs: a second shader generator (WGSL), Safari/Firefox
availability still partial, the lobby's SwiftShader-based proofs need `--enable-unsafe-webgpu`, and the
draw-call count after phase 1 is already low enough (130-180 a frame) that WebGL2 overhead stops
mattering. Not worth it now; the contract makes it possible later.

### Visual upgrades, honestly

- **Internal resolution**: already native. `web/engine.js fitCanvas` sizes the canvas at CSS size times
  devicePixelRatio (capped at 1440 px tall) and the viewport maps 320x240 onto it; 3D is rasterised at
  canvas resolution. Nothing to do except a "N64 resolution" toggle (render to a 320x240 FBO and
  upscale with nearest) for people who want the original look: ~40 lines in `rb_webgl2.c`.
- **MSAA**: `antialias` in the context attributes gives it for free on the default framebuffer but is
  ignored by many browsers for `preserveDrawingBuffer` contexts; the dependable way is a multisampled
  renderbuffer FBO (4x) and `glBlitFramebuffer` at frame end: ~60 lines, GPU cost only, no visual
  risk. Pair with (4).
- **3-point filtering**: in the fragment shader, 3 `texelFetch`/`texture` reads and a triangle weight
  (libultraship's `o_three_point_filtering` block in `shaders/opengl`): ~30 lines of GLSL, one flag in
  the shader key. Fidelity win on every magnified texture.
- **Frame interpolation (render only)**: the game draws at 60 Hz and the loop runs at 60 Hz
  (`engine.js runLoop`, `net.js` tick); a 120/144 Hz display would need interpolating object matrices
  between two sim frames. The lists are built by the game per frame with matrices baked in
  (`gSPMatrix` words from `gSYTaskmanGraphicsHeap`), so interpolation means keeping the previous
  frame's matrices per `Mtx` address and lerping in `OP_MTX`: fragile (matrices are reallocated each
  frame from a heap, the address is not identity) and the game's own animation is 60 Hz steps. Not
  worth it; do not do it.
- **Widescreen**: already done (`proof/widescreen-spec.md`), keep its two flags in `RbState`.

## 5. Phase plan (for option A)

Every phase: port code only (`port/gfx`, `port/loop/prof.c`); `make -f Makefile.port -j web
WEB=build/us/port/devweb`; prove with `proof/gfx/perf.mjs` (numbers), `proof/gfx/shots2.mjs` + `compare
-metric AE` on the canvas region (pixels), `proof/state-machines/hashlogs.sh` (57/57 same), and
`web/tests/net_proof.mjs` (pages agree). Never write `web/`.

**Phase 0 (this branch, done).** Profile counters; texture key memo; batched draws; GL state cache.
Acceptance: pixels identical at the same frame (met), draws 5x fewer (met), hash logs same (met).

**Phase 1. Finish the cheap wins in the interpreter.** (a) `fill_state` once per state change, not per
triangle: keep an `RbState` in `S` and a `dirty` flag set by the ops that change it (`OP_COMBINE`,
`OP_OTHERMODE_*`, colours, `OP_TEXTURE`, `OP_SETTILE*`, `OP_LOAD*`, `OP_GEOMETRYMODE`, viewport,
scissor), so `emit_tri` only compares a pointer. (b) `import_tile` only on texture state change (same
dirty flag). (c) Move the per-draw `port_now()` out of the hot path (keep per-batch). (d) LRU eviction
for `sTexCache`/`sTx` with a frame stamp (evict entries unused for 600 frames; delete GL textures) and a
real LRU for programs. Acceptance: interpreter time per frame under 0.5 ms in the browser profile;
pixels identical; memory flat over a 10-minute match (`performance.memory` or `HEAPU8.length` stable).
Risk: low. ~1 day.

**Phase 2. Blender decode.** Replace `alpha_blend/fog/decal/alpha_compare` with a decoded blender:
cycle-1 and cycle-2 (P, A, M, B) muxes, `FORCE_BL`, `IM_RD`, `CLR_ON_CVG`, `CVG_X_ALPHA`,
`ALPHA_CVG_SEL`, `ZMODE`, and the two alpha-compare modes, mapped to GL blend equations exactly the way
libultraship's `GfxSpTriUpdateState` does it (`use_alpha`, `use_fog`, `zmode_decal`, `alpha_threshold`,
coverage-as-alpha); fog as `G_BL_CLR_FOG` in either cycle. Keep the shader key = combiner words +
these bits. Acceptance: `GfxCoverage` dump of a full match on each stage lists every (combine, oml, omh,
geom) state and each state's mapping is documented; screenshots of Meta Crystal, Dream Land, Hyrule and
Sector Z (fog, translucent effects, shadows) compared by eye to hardware references; hash logs same.
Risk: medium (semantic), bounded by the libultraship tables. ~2-3 days.

**Phase 3. Geometry fidelity.** Near-plane clipping in `emit_tri` (Sutherland-Hodgman against
`z > -w`, interpolating u/v/colour/fog alpha), and the trace cull test only after clipping.
Acceptance: `GFXTRACE` at match start shows no triangle box beyond [-2, 2] NDC; appear effects on Final
Destination and Meta Crystal look like hardware; `net_proof` unchanged. Risk: low. ~1 day.

**Phase 4. Picture quality (opt-in toggles in the page, defaults chosen after a look on a real GPU).**
(a) 3-point filtering in the fragment shader with `G_TF_BILERP`; (b) 4x MSAA via a multisampled FBO and
blit; (c) a "320x240" mode. Acceptance: side-by-side screenshots; frame time under the phase-1 numbers
plus the FBO blit; the `wide` toggle still works. Risk: low. ~1-2 days.

**Phase 5 (optional). Second backend behind `RbState`**: WebGPU or a batching rewrite with persistent
vertex buffers (ring buffer + `glBufferSubData`). Only if phase 1 numbers are still too high on a
target machine. Acceptance: same screenshots, lower `rbdraw` ms.

Not planned: libultraship adoption (section 4B) and frame interpolation.
