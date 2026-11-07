# What the ssb64 renderer can take from Fast3D

Research only. The question: which practices and ideas in libultraship's Fast3D (the renderer under the
mk64 port) are worth replicating in `port/gfx/f3dex2.c` + `rb_webgl2.c`, without adopting libultraship
(rejected in `spec/renderer.md` section 4B). Every idea is checked against what the game actually draws,
using the coverage dump the in-progress renderer work produced (`~/sb64/ssb64-render/proof/renderer/coverage.txt`:
12 stages x 2,400 frames, 8 fighters, items on; 149 distinct draw states, 6.25 M draws).

Sources, with the line numbers used below:

- Fast3D: `/home/desktop/mk64/SpaghettiKart-wasm/libultraship/src/fast/` at libultraship `1.3.1-462-g67042a05`
  (the Kart64 fork with `__EMSCRIPTEN__` paths): `interpreter.cpp` (5,391 lines), `backends/gfx_opengl.cpp`
  (1,119), `shaders/opengl/default.shader.glsl` (296), `include/fast/interpreter.h`.
- ssb64, branch `gfx-metal-crystal` (this worktree, commit `3d049486d`): `port/gfx/f3dex2.c` (622),
  `port/gfx/rb_webgl2.c` (229), `port/gfx/gfx.h` (38).
- ssb64, branch `renderer` (`~/sb64/ssb64-render`, read only; commit `5862b9ab9` "renderer phase 1" plus
  uncommitted phase 2/3 work): referred to as "in progress". It already has: state built on change with a
  serial for batching, texture and program LRU, an xxHash-style texel hash, a full blender decode
  (`decode_blender`, `blender_glsl`, colour mask for depth-only states, dither alpha compare), and
  Sutherland-Hodgman near-plane clipping with the cull test after the clip.

License: libultraship is MIT (`libultraship/LICENSE`, kenix3 2022) and the Fast3D directory carries its own
MIT notice (`src/fast/LICENSE.txt`, Emill and MaikelChan 2020). Copying code is allowed provided the
copyright and permission notice travel with it: any literal copy (the 3-point filter GLSL is the one
candidate) goes with a `// From libultraship Fast3D, MIT, (c) 2020 Emill, MaikelChan` block and the notice
text in `port/gfx/THIRD_PARTY.md`. Ideas re-implemented in our own code need no notice.

## 1. Ranked table

Benefit is for this game, not in general. Size is lines in `port/gfx`. "Spec" is the phase in
`spec/renderer.md` section 5 that already covers it, if any.

| # | idea | Fast3D | ssb64 today | benefit | size | risk | spec / in progress |
|---|---|---|---|---|---|---|---|
| 1 | half-texel offset under bilinear filtering | `interpreter.cpp:2049-2055` | none (`f3dex2.c:393`) | visual, every textured triangle (the game is 100% `G_TF_BILERP` in matches) | 3 | low | not in spec; prerequisite for phase 4a |
| 2 | 3-point filter in the fragment shader, sampler at `NEAREST` | `default.shader.glsl:147-154`, `gfx_opengl.cpp:609-612` | `GL_LINEAR` (`rb_webgl2.c:101-102`) | visual, magnified textures | 20 GLSL + 1 key bit | low | phase 4a (this is the code to copy) |
| 3 | MSAA via multisampled renderbuffer FBO + `glBlitFramebuffer`; the same FBO at 320x240 is the "N64 resolution" mode | `gfx_opengl.cpp:792-841, 887-907`; `interpreter.cpp:4958-4975, 5065-5077` | `antialias = 0`, draws to the default framebuffer (`rb_webgl2.c:113`) | visual, edges | 60 | low | phase 4b, 4c (reference) |
| 4 | import and bind only the texels the combiner reads | `GenerateCC` `usedTextures` `interpreter.cpp:302-334`, `1884` | tile 1 imported and bound whenever 2-cycle (`f3dex2.c:362`) | perf: 67% of draws are 2-cycle and none of them reads a texel in cycle 2 | 15 | low | not in spec |
| 5 | clamp to the tile rectangle in the shader when the tile is smaller than the loaded texture, or clamp+mirror | `interpreter.cpp:1941-1963, 2060-2069`; `glsl:170-185` | GL clamp only when the tile area is at most the texture (`f3dex2.c:327-329`), else `REPEAT` | visual, seams where the game uses it (spec section 3 item 8) | 25 | low | spec item 8, no phase |
| 6 | mask 0 with `G_TX_WRAP` means clamp | `interpreter.cpp:2390-2395` | `REPEAT` (`f3dex2.c:330, 569-574`) | visual, only where the game relies on it | 2 | low | not in spec |
| 7 | copy-mode rectangles sample at `NEAREST` | `interpreter.cpp:2726-2728` | filter from `omh`, so bilinear (`f3dex2.c:359-361, 415`) | visual, 28 k copy-mode rects on one stage | 1 | none | not in spec |
| 8 | decal polygon offset scaled to the render height | `gfx_opengl.cpp:667-701` | constant `(-2, -2)` at canvas resolution (`rb_webgl2.c:202`) | visual, decals (shadows) on tall canvases | 2 | low | spec item 9, no phase |
| 9 | skip the redundant full-screen depth clear, scissor partial ones | `interpreter.cpp:2924-2952`, `gfx_opengl.cpp:868-885` | any fill to the Z image clears the whole depth buffer after a flush (`f3dex2.c:540`, `rb_webgl2.c:142-147`) | perf (one clear + one flush a frame) and correctness if a partial clear ever happens | 10 | low | not in spec |
| 10 | trivial frustum rejection per vertex | `interpreter.cpp:1651-1668, 1714-1717` | GL clips; the interpreter still builds and batches the vertices | perf, small | 12 | low | phase 3 touches the same code |
| 11 | prim/env colours as vertex attributes so a colour change does not split a batch | `interpreter.cpp:2094-2181`, `gfx_opengl.cpp:486-492` | uniforms; a colour change is a new batch (`rb_draw` serial) | perf, unknown until counted (`COUNT_STATEBUILD` per reason) | 40 | medium | phase 5 territory |
| 12 | address-keyed texture cache, no content hash | `interpreter.h:179-196`, `interpreter.cpp:1195-1213` | content hash with a per-frame memo (`f3dex2.c:305-314`) | perf, 0.1-0.3 ms a frame at most | 30 | medium (in-place writes) | phase 1 memo already took most of it |

Not applicable (section 3): near clipping (Fast3D has none), frame interpolation, frame pacing, combiner
normalisation (21 to 19 programs), animated noise (no noise state in matches), prim-depth as depth-test
source (never compared), HD textures, framebuffer effects, positional lights, S2DEX, chroma key, grayscale,
Fast3D's blender shortcuts and fog, its texture LOD heuristics, its vertex buffer handling.

## 2. Details

### 2.1 Half-texel offset under bilinear filtering

Fast3D, `interpreter.cpp:2049-2055`: when the texture filter is not `G_TF_POINT` and the primitive is a
triangle (not a rectangle), `u += 0.5; v += 0.5` before dividing by the texture size. The RDP's bilinear
filter treats texel `i` as covering `[i, i+1)` and interpolates towards `i+1` as the coordinate's fraction
grows; GL's `GL_LINEAR` puts texel centres at `i + 0.5`. Without the offset every filtered texture is
sampled half a texel up and left of where hardware samples it. Rectangles are excluded because their
coordinates already describe the copy the RDP performs.

ssb64, `f3dex2.c:388-394`: `o->u = u * sx - T->uls / 4.0f` with no offset; `rb_webgl2.c:55` samples
`texture(uS0, vt * uScale0)`. The coverage dump shows `omh` taking only three values in matches
(`0x00002000`, `0x00102000`, `0x00202000`): the filter field is `G_TF_BILERP` in every state. So the
shift applies to every textured triangle in the game: 32x32 fighter textures are half a texel off, eyes
and mouth edges land between the wrong texels, and the 3-point filter (next item) would be built on the
wrong lattice.

Applies: yes, everywhere. Benefit: fidelity on every texture; subtle but universal. Size: 3 lines in
`emit_tri` (add 0.5 when `(S.omh >> 12 & 3) != 0`, not in `do_rect`). Risk: low; prove with
`compare -metric AE` on a frame with a hard-edged texture (the HUD portraits are rectangles and must not
move; a fighter's eye should). Overlap: none; the spec's phase 4a (3-point) depends on it.

### 2.2 3-point filtering

Fast3D, `default.shader.glsl:147-154`:

```glsl
vec4 filter3point(in sampler2D tex, in vec2 texCoord, in vec2 texSize) {
    vec2 offset = fract(texCoord*texSize - vec2(0.5));
    offset -= step(1.0, offset.x + offset.y);
    vec4 c0 = TEX_OFFSET(offset);
    vec4 c1 = TEX_OFFSET(vec2(offset.x - sign(offset.x), offset.y));
    vec4 c2 = TEX_OFFSET(vec2(offset.x, offset.y - sign(offset.y)));
    return c0 + abs(offset.x)*(c1-c0) + abs(offset.y)*(c2-c0);
}
```

with `TEX_OFFSET(off) = texture(tex, texCoord - off / texSize)` (`glsl:132`). The sampler is set to
`GL_NEAREST` when the three-point mode is active (`gfx_opengl.cpp:609`: `linear && mode == FILTER_LINEAR ?
GL_LINEAR : GL_NEAREST`), so the three taps are exact texels and the shader does the weights. The filter
mode is a per-texture integer uniform (`texture_filtering[2]`, `gfx_opengl.cpp:67-69`) rather than a shader
variant; for ssb64 one bit in the program key is simpler and avoids three per-draw uniform arrays.

ssb64: `rb_webgl2.c:101-102` sets `GL_LINEAR`. The spec's phase 4a estimate (~30 lines of GLSL, one flag)
is right. Two details to carry over: the `- 0.5` in `fract` assumes item 2.1's half-texel convention, and
the texture size must be the decoded size (`uScale0` already carries `1/w, 1/h`). Literal copy: keep the
MIT notice. Risk: low; opt-in toggle as the spec says. Expected: diagonal rather than square gradients on
magnified textures (the "GO!" text, portraits, Dream Land's tree).

### 2.3 MSAA and the native-resolution mode

Fast3D renders into an FBO whenever MSAA is on or the viewport differs from the window
(`interpreter.cpp:4958-4975`): colour as `glRenderbufferStorageMultisample(GL_RGB8)` and depth as
`GL_DEPTH24_STENCIL8` multisampled (`gfx_opengl.cpp:810-825`), then at frame end
`glBlitFramebuffer(..., GL_COLOR_BUFFER_BIT, GL_NEAREST)` into the default framebuffer
(`gfx_opengl.cpp:887-907`, called at `interpreter.cpp:5065-5077`). The scissor test is disabled around the
blit (`gfx_opengl.cpp:894-897`). Viewport and scissor are scaled by the FBO/native ratio
(`AdjustVIewportOrScissor`, `interpreter.cpp:2206-2236`), which ssb64's `apply()` already does for the
canvas (`rb_webgl2.c:166-168, 203-206`).

ssb64 draws to the default framebuffer with `antialias = 0` (`rb_webgl2.c:113`). The spec's phase 4b
(multisampled FBO + blit, ~60 lines) and 4c (320x240 FBO, nearest upscale) are the same mechanism with two
parameter sets; Fast3D's code is the reference for the GL calls and the order (clear the FBO, draw, resolve,
then the page composes). WebGL2 supports all of it (`renderbufferStorageMultisample`, `blitFramebuffer`).
Risk: low; GPU cost only. One caveat from Fast3D: the resolve target must be the same size as the MSAA
buffer (`interpreter.cpp:4964-4967`), so the 320x240 mode and MSAA compose as MSAA FBO -> same-size
resolve -> nearest blit to the canvas, two blits.

### 2.4 Import only the texels the combiner reads

Fast3D decides per combiner which texture slots are read (`GenerateCC`, `interpreter.cpp:302-334` and
`376-392`, `usedTextures[2]`) and imports/binds a slot only if used (`interpreter.cpp:1884-1887`). In
2-cycle mode a TEXEL0 reference in cycle 2 counts as slot 1 and vice versa (`gfx_opengl.cpp:119-140`, the
`first_cycle` swap), which is the RDP's pipelining quirk; `gfx_cc_get_features` additionally marks both
slots used in 2-cycle (`interpreter.cpp:5327-5338`).

ssb64 `use_textures` (`f3dex2.c:357-363`) imports tile `textile + 1` whenever the cycle type is 2-cycle
and the backend binds both (`rb_webgl2.c:190`). The coverage dump: 4,180,877 of 6,251,290 draws (67%) are
2-cycle; decoding every 2-cycle combiner in the dump, none reads TEXEL0/TEXEL1 (colour or alpha) in cycle
2, and no 1-cycle combiner reads TEXEL1. Only one state reads TEXEL1 at all: `cc=272c04:1f0c93ff`
(cycle 1: `(TEXEL1 - TEXEL0) * PRIM_LOD_FRAC + TEXEL0`, a two-texture crossfade, 11,880 draws on two
stages). So for two thirds of the game's draws the second tile is imported (hash memo + cache lookup),
compared in the state, and bound in GL for nothing.

Proposal: derive `uses_t0`/`uses_t1` from the combiner words when `OP_COMBINE` runs (16 field reads, as
`rb_webgl2.c:60-61` already does for the shader), import tile 1 only when `uses_t1`, and leave
`tex[1].key = 0` otherwise so the backend binds the white texture. Benefit: fewer texture lookups and binds
per state change, fewer distinct states (a second tile that differs between materials no longer splits
batches), and the shader stops sampling `uS1`. Size: ~15 lines. Risk: low; the cycle-2 swap can be ignored
for this game (zero draws) but should be written down as the rule in case a menu uses it. Not in the spec;
the in-progress branch keeps the "both tiles in 2-cycle" rule.

### 2.5 Clamp to the tile rectangle in the shader

Fast3D computes the loaded texture size and the tile size separately (`interpreter.cpp:1900-1951`) and,
when `G_TX_CLAMP` is set but the tile is smaller than the texture, or clamp is combined with mirror
(GLES has no `GL_MIRROR_CLAMP_TO_EDGE`), it clears the GL clamp and passes the upper bound
`(tile_w - 0.5) / tex_w` as a vertex attribute (`interpreter.cpp:1956-1963, 2060-2069`); the fragment
shader does `clamp(vTexCoord, 0.5/texSize, vTexClamp)` (`glsl:175-185`). The lower bound `0.5/texSize`
keeps bilinear taps inside the first texel row.

ssb64 `import_tile` (`f3dex2.c:327-331`) decodes only as many texels as the tile covers (capped by the
mask), sets GL clamp if `cms & 2` and the tile area is at most the decoded size, otherwise `REPEAT`. The
cases that differ: a tile narrower than the loaded image with clamp (the shader clamp would be exact), and
clamp+mirror (GL `MIRRORED_REPEAT` ignores the clamp). The spec lists this as item 8, "the place to look if
a texture seam shows", with no known wrong picture. The coverage dump does not record `cms/cmt/masks`, so
whether the game hits either case is unknown; `GfxTrace` prints them per draw (`GTT` line, `f3dex2.c:108`).

Proposal: add `cms, cmt, masks, maskt, tile-vs-texture size` to `cover()` (3 lines), run the coverage once,
and implement the shader clamp (a per-texture `vec4 uClamp` = lo.s, lo.t, hi.s, hi.t and a flag bit in the
program key; ~25 lines) only if a state needs it. Risk: low.

### 2.6 Mask 0 with wrap means clamp

Fast3D, `GfxDpSetTile` (`interpreter.cpp:2390-2395`): `if (cms == G_TX_WRAP && masks == G_TX_NOMASK) cms
= G_TX_CLAMP` (same for t). With a zero mask the RDP does not wrap the coordinate; the texel address is
taken as is, which for in-range tiles behaves as a clamp to the tile and never as a repeat.

ssb64: `OP_SETTILE` stores the fields (`f3dex2.c:569-574`); `import_tile:330` sets `clamp_s` only from
`cms & 2`, so mask 0 + wrap gives `GL_REPEAT`. Two lines to fix at `import_tile`. Applies only if the
game sets such tiles; same coverage extension as 2.5 tells. Risk: none when the evidence is there.

### 2.7 Copy-mode rectangles are not filtered

Fast3D forces `G_TF_POINT` for the duration of a copy-mode rectangle (`GfxDrawRectangle`,
`interpreter.cpp:2726-2728, 2804-2806`), divides `dsdx` by 4 and adds one pixel to the far edges
(`interpreter.cpp:2813-2823`), and replaces the combiner by `TEXEL0` (`2818-2819`).

ssb64 `do_rect` (`f3dex2.c:413-415, 428`) does the `+1`, the `ds /= 4` and the combiner marker, but
`use_textures:359-361` takes `linear` from the othermode filter field, which is `G_TF_BILERP` in the copy
state the game uses (`omh = 0x00202000`, 28,468 rectangles on stage id 5, the only copy-mode state in the
dump). Hardware copies texels; ours smears them. One line: `st.tex[t].linear = 0` when `cyc == 2`.
Risk: none.

### 2.8 Decal polygon offset scaled to the render height

Fast3D, `DrawTriangles` (`gfx_opengl.cpp:667-701`): `glPolygonOffset(SSDB, -2)` with `SSDB = -2` by
default, or `-height / 120` in its "N64 mode" (so that at 240 px tall it is -2 and it grows with the
framebuffer), together with `GL_LEQUAL` for decals (`gfx_opengl.cpp:661`).

ssb64 `apply()` (`rb_webgl2.c:198, 202`): `GL_LEQUAL` and a fixed `(-2, -2)` at canvas resolution, which
`engine.js fitCanvas` sets up to 1440 px tall. The slope factor is in window units, so at 6x the N64 height
the offset is 6x too small relative to the geometry's depth slope. The spec says shadows and platform
decals look right today; Fast3D's scaling is the two-line fix if they ever flicker on a large canvas:
`glPolygonOffset(-2.0f * sCanvasH / 240.0f, -2)`. Risk: low; check Dream Land's shadows at 1440p.

### 2.9 Depth clears

Fast3D, `GfxDpFillRectangle` (`interpreter.cpp:2924-2952`): a fill rectangle aimed at the Z image is
skipped when it covers the whole screen (the frame start already cleared depth) and otherwise becomes a
scissored `glClear(GL_DEPTH_BUFFER_BIT)` of that rectangle (`ClearDepthRegion`, `gfx_opengl.cpp:868-885`).

ssb64 `OP_FILLRECT` (`f3dex2.c:538-543`): any fill while `cimg_depth` is set calls `rb_clear_depth`
(`rb_webgl2.c:142-147`): flush, clear the whole depth buffer, forget the depth state. If the game's clear
is the usual full-screen one at the start of the frame, that is one redundant clear and one flush per frame
(cheap but free to remove, and the flush is a batch boundary). If the game ever clears a sub-rectangle
(the magnifier's Z-image writes are texture rectangles and are skipped at `f3dex2.c:534`, so probably
not), ours wipes depth under geometry already drawn. Proposal: compare the rectangle with the full picture;
skip if full and nothing has been drawn since `rb_begin_frame`, else scissor the clear. ~10 lines.
Risk: low; confirm with a trace which fill rectangles reach the Z image in a match.

### 2.10 Trivial frustum rejection

Fast3D marks each vertex with the frustum planes it is outside of (`clip_rej`, `interpreter.cpp:1651-1668`)
and drops a triangle whose three vertices share a plane (`1714-1717`), before any state work. ssb64 lets GL
clip; `emit_tri` still builds three `RbVertex` and copies them into the batch. The in-progress clipper
already rejects triangles fully behind the near plane. Fighters knocked off-screen and the stage's
far-side geometry are the candidates. With widescreen the x test must use the scaled x (`sK`). Benefit:
a few percent of 1,000-1,300 vertices a frame; measure with `COUNT_TRIS` before deciding. ~12 lines.

### 2.11 Per-vertex combiner inputs

Fast3D feeds prim colour, env colour, prim LOD fraction and the like to the shader as vertex attributes,
one `aInputN` per distinct input the combiner uses (`interpreter.cpp:2094-2181`, attribute setup
`gfx_opengl.cpp:486-492`), so a `gDPSetPrimColor` between two draws does not end the batch; only a shader
change, a texture change, blend/depth/viewport changes and a 256-triangle cap (`MAX_TRI_BUFFER`,
`interpreter.cpp:111, 2190-2193`) do.

ssb64 passes them as uniforms with a cache (`rb_webgl2.c:185-189`) and starts a new batch on any state
difference. The fighter material is `G_RM_FOG_PRIM_A` in cycle 1 (hit flashes and tints through the fog
colour register) plus per-joint prim/env colours, so colour-only splits are plausible. Before building this
(RbVertex grows from 40 to 72 bytes, more upload per frame), count them: a histogram of "why did the serial
change" in `emit_tri` (combiner, blender, colours, texture, viewport). If colours cause a large share of the
130-180 batches, move prim and env into the vertex. Size ~40 lines; medium risk (shader inputs change for
every program). Phase 5 territory in the spec.

### 2.12 Address-keyed texture cache

Fast3D never hashes texels: the key is the texture's DRAM address, the palette's DRAM address(es), format,
size, palette index and byte count (`TextureCacheKey`, `interpreter.h:179-196`; built at
`interpreter.cpp:1195-1213`), with explicit invalidation by address (`TextureCacheDelete`,
`interpreter.cpp:512-534`) for games that write textures at runtime. The cache is a 1,024-entry LRU with
recycled GL texture ids (`interpreter.cpp:473-491`).

ssb64 hashes the texel block and the palette (`import_tile`, `f3dex2.c:305-314`), memoised per frame; the
in-progress branch made the hash 4-lane. This game's palette animation swaps pointers
(`src/sys/objdisplay.c:1189`, `mobj->sub.palettes[palette_id]`), so address keys would work for its data,
and the content hash is what caught the byte-swapped data image in `proof/gfx/README.md`. Keep the hash;
revisit only if `PROF_TEXDECODE`/texture hashing still shows in a profile after phase 1, and then add an
address-keyed first level that re-hashes every N frames rather than dropping the hash. Medium risk, small
gain: last in the table.

## 3. Considered and not applicable

- **Near-plane clipping.** This Fast3D does not clip: `CLIP_NEAR` is commented out
  (`interpreter.cpp:1665`), it relies on `GL_DEPTH_CLAMP` on desktop (`gfx_opengl.cpp:724`), a
  `gl_Position.z *= 0.3` hack in the GLES vertex shader (`glsl:77-79`) and negates the cull cross product
  when one vertex has `w < 0` (`interpreter.cpp:1730-1734`). `spec/renderer.md` item 2 credits libultraship
  with a clip; that is not true of this copy. The in-progress Sutherland-Hodgman clip with the cull test on
  the clipped polygon is the better answer. One thing to carry from Fast3D's workaround: WebGL2 has no
  depth clamp, so triangles with `z/w > 1` are cut at the far plane; check with `GFXTRACE` whether any
  stage backdrop reaches it before worrying.
- **Frame interpolation.** Fast3D's hook is `mtx_replacements` in `Run()` (`interpreter.cpp:5022`) and the
  custom `G_SETTARGETINTERPINDEX` opcode (`4238`); the work is in the game: Spaghetti Kart calls
  `FrameInterpolation_*` at 417 sites in 65 game source files (`src/render_player.c:701, 1213, 1339, ...`)
  to tag matrices with object identities. That is game-side instrumentation, which this port's rule
  (render reads, never changes the simulation or its sources) forbids and the spec already declined.
- **Frame pacing.** `SyncFramerateWithTime` (`gfx_sdl2.cpp:655-699`) sleeps to a target FPS before
  `SDL_GL_SwapWindow`; in the browser `requestAnimationFrame` plus the fixed-step accumulator in
  `web/engine.js:111-126` (max 4 catch-up steps, draw only on the last) is the right tool. Nothing to borrow.
- **Combiner normalisation for the shader key** (`GenerateCC`, `interpreter.cpp:211-276`: `a == b` or
  `c == 0` collapses to zero, `TEXEL1 -> TEXEL0` in 1-cycle, unused first cycle cleared;
  `gfx_cc_get_features:5342-5349` picks single/multiply/mix forms). Applied to the dump's 21 distinct
  (combiner, cycle) keys it yields 19. The game has few materials; shader compile hitches are better
  addressed by compiling the known list at load (an ssb64 idea, not a Fast3D one: the coverage dump is
  that list).
- **Animated, resolution-scaled noise** (`RAND_NOISE`, `gfx_opengl.cpp:101`; `frame_count` and
  `noise_scale` uniforms, `gfx_opengl.cpp:59-62`; `glsl:267-269` for dither alpha compare). The dump has
  no combiner reading `NOISE` and no `G_AC_DITHER` state in 6.25 M match draws (`lbparticle.c:2088` can
  select dither for a particle kind that did not occur). Spec item 6 called it cosmetic; it is also unused in
  matches. If a menu or the results screen shows noise, the two uniforms are a 6-line change.
- **Prim depth as a depth-test source without `G_ZBUFFER`** (`interpreter.cpp:1760-1762`). The game sets
  `G_ZS_PRIM` for particles (`lbparticle.c:1665`) and the opening (`mvopeningroom.c:591`); in the dump every
  `ZS_PRIM` state (`oml 0x00504345`, `0x00504245`) has `Z_CMP` clear, so no depth compare ever uses it.
- **Fast3D's blender shortcuts.** `use_alpha` is "some cycle has `CLR_MEM` with `1MA`", fog is a shader
  `mix` by a per-vertex fog attribute (`interpreter.cpp:1794-1800`, `glsl:255-261`), `invisible` is `G_BL_0`
  with `CLR_MEM` (`1805-1806`), alpha threshold is a constant `8/256` (`glsl:279`) rather than the blend
  colour alpha, texture edge is `a > 0.19` (`glsl:264`), and shade alpha is forced to 1.0 under fog
  (`interpreter.cpp:2172-2176`) although the RSP really writes the fog factor into vertex alpha. The
  in-progress `decode_blender` keeps both cycles' P/A/M/B, uses the blend colour register for the threshold
  and a colour mask for depth-only states; it is closer to hardware than Fast3D here. Also of note: the dump
  shows `G_FOG` set in zero geometry modes during matches, so the game's "fog" is entirely the blender's
  `G_RM_FOG_PRIM_A` tint (4.18 M draws), which the in-progress decode handles and the old `F_FOG` vertex
  mix never did.
- **9-bit wraparound of combiner intermediates** (`WRAP(x, -0.51, 1.51)`, `glsl:225-252`). Matters only
  for a 2-cycle combiner whose cycle-1 result leaves [0, 1]; the game's 2-cycle combiners are
  `T0*SHADE -> COMBINED*ENV` and `(PRIM-ENV)*T0+ENV -> COMBINED*SHADE`, all inside range. Revisit if a
  2-cycle material looks wrong.
- **Texture LOD heuristics**: the `pyramidLike` test (`interpreter.cpp:571-586`) that guesses a mipmap
  pyramid from the loaded/rendered ratio and the `LOD_FRACTION` distance hack (`2125-2141`) are for
  SM64/OoT. The game runs `G_TL_TILE` (`src/sys/rdp.c:39`), never `G_TL_LOD`.
- **HD texture replacement, masks, blended textures** (`TEXEL0_MASK/BLEND`, `ImportTextureMask`,
  `interpreter.cpp:1294`), **framebuffer effects** (`gfx_set_framebuffer`, `CopyFrameBuffer`,
  `ReadFramebufferToCPU`, `GetPixelDepth`), **positional lights** (`G_LIGHTING_POSITIONAL`,
  `interpreter.cpp:1557-1593`, an MM ucode feature), **S2DEX** (`Gfxs2dex*`; the game's sprites are
  F3DEX2 texture rectangles from `src/libultra/sp/sprite.c`), **chroma key and colour convert**
  (`G_CCMUX_KEY_*`, `CONVERT_K*`; the game's only `G_TC_CONV` is in the sprite library's YUV path,
  `sprite.c:519-526`, unused by matches), **grayscale**, **sRGB**, the Kart64 split-screen remap: features
  the game does not use or the project does not want.
- **Texture cache bookkeeping** (`TextureCacheLookup`, `interpreter.cpp:461-502`: count-capped LRU,
  recycled texture ids, `textures_changed` per TMEM slot). The in-progress branch has the equivalent
  (600-frame LRU, `TEXDIRTY`). Id recycling saves `glGenTextures/glDeleteTextures` pairs, not worth a change.
- **Vertex buffer handling**: Fast3D also does `glBufferData(GL_STREAM_DRAW)` per flush
  (`gfx_opengl.cpp:706`) with a 256-triangle cap; ssb64 batches up to 16,384 vertices. The spec's phase 5
  ring-buffer idea is not a Fast3D practice; nothing to copy.
- **GL state cache** (`SelectTexture`, `SetUseAlpha`, lazy depth/decal in `DrawTriangles`,
  `gfx_opengl.cpp:564-575, 641-701`): ssb64's `apply()` already does the same (phase 0).
- **Rectangle details**: viewport bypass by a saved/restored native viewport (`interpreter.cpp:2779-2802`;
  ssb64 `do_rect:412`), the `-1024..2048` expansion of full-screen fills for widescreen
  (`2956-2967`; ssb64's `x1 - x0 < WIDE_SPAN` rule at `do_rect:419` is more general), fill-mode combiner
  override (`2982-2984`; ssb64 `do_rect:427`). Already equivalent.
- **TLUT staging by DRAM address** (`GfxDpLoadTlut`, `interpreter.cpp:2425-2464`): ssb64 keeps a 512-byte
  palette area at the TMEM offset (`OP_LOADTLUT`, `f3dex2.c:544-551`), which handles partial loads at any
  offset. Ours is the more exact model.

## 4. Suggested addendum to the phase plan

Changes to `spec/renderer.md` section 5, in the order the work is already going. Nothing here changes a
phase's acceptance test; each adds a small item or a measurement.

- **Phase 1 (in progress) add**: 2.7 copy-mode `NEAREST` (one line in `do_rect`); 2.4 import only the
  texels the combiner reads (derive `uses_t0/uses_t1` at `OP_COMBINE`, drop tile 1 otherwise). Extend
  `cover()` with `cms, cmt, masks, maskt` and whether the tile is smaller than the loaded image, rerun
  `proof/renderer/coverage.mjs` once, and decide 2.5 and 2.6 from the result (2.6 is two lines if any
  state has mask 0 with wrap). Acceptance addition: texture binds per frame drop (the 2-cycle states no
  longer bind a second texture); pixels identical except copy-mode rectangles on stage id 5.
- **Phase 2 (blender) note**: Fast3D's decode is the coarser one; keep `decode_blender`. Record in the
  phase's documentation that the game sets `G_FOG` in no match state and that all fog is
  `G_RM_FOG_PRIM_A`, so the vertex fog path can be left as is and need not be proven.
- **Phase 3 (geometry) note**: Fast3D has no near clip; the spec's reference to it should be corrected.
  Optionally add 2.10 trivial rejection in the same function, counted with `COUNT_TRIS`. Add a one-off
  `GFXTRACE` check for triangles with `z/w > 1` (far plane, no depth clamp in WebGL2).
- **Phase 4 (picture) add 2.1 first**: the half-texel offset is a correctness fix on every frame and
  changes the lattice 3-point filtering (4a) builds on; land it, screenshot-diff a fighter close-up, then
  4a from `default.shader.glsl:147-154` with the sampler at `NEAREST` and the MIT notice; 4b/4c per
  `gfx_opengl.cpp:792-907`; 2.8 scaled decal offset alongside, checked at 1440p.
- **Phase 5 (only if phase 1 numbers are still too high)**: before any backend work, histogram the
  reasons a batch ends; if colours dominate, 2.11 (prim/env as vertex attributes); if texture hashing
  shows, 2.12; 2.9 (skip the redundant Z clear) is cheap enough to do at any point once a trace confirms
  the game's Z fills are full-screen.
- **Not planned, now with reasons measured**: combiner normalisation (21 to 19 programs), animated noise
  (no noise or dither state in 6.25 M draws), prim-depth compare (never with `Z_CMP`), frame interpolation
  (417 game-side call sites in mk64), libultraship adoption (unchanged from section 4B).
