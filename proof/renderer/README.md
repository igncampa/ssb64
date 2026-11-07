# Renderer: spec/renderer.md phases 0-5

Branch `renderer` (worktree `~/sb64/ssb64-render`) on top of `3d049486d`. Not pushed, not deployed. Every
run here used this worktree's own `web/` and relay, test ports 18400-18499; the live site was not touched.

## Commits

| commit | phase | what |
|---|---|---|
| `5862b9ab9` | 1 | triangle state rebuilt only when an op that feeds it ran; backend batches on a state serial; per-batch timing; LRU for decoded textures, GL textures (600 frames) and programs; four-lane texel hash |
| `dfd5df3d0` | (data) | packdata keeps palettes named by material-animation tables big-endian |
| `efed8dc6b` | 2 | the RDP blender decoded (both cycles, FORCE_BL, ALPHA_CVG_SEL, CVG_X_ALPHA, dither compare, fog alpha, blend colour) |
| `1ff7da9eb` | 3 | near-plane clipping before the cull test |
| `75c22e23e` | 4 | 3-point texture filter, 4x MSAA through a framebuffer, N64-resolution picture, the page's Picture button |
| `d8e453bbf` | 5 | one vertex upload per frame, batches replayed at offsets |
| `599b26ea9` | lessons 2.1 | half-texel offset under bilinear; coverage records texture addressing cases |
| `52ad96cee` | lessons 2.4, 2.7 | import only the tiles the combiner reads; copy-mode rectangles unfiltered |
| `455553227` | lessons 2.5 | clamp to the tile in the shader when GL's wrap modes cannot |
| `ebab64936` | lessons 2.8-2.11 | trivial rejection, the frame's first depth clear skipped, decal offset scaled; split histogram |

## Phase by phase

**1. Cheap wins.** `fill_state`/`use_textures` ran per triangle; now `emit_tri` rebuilds `sSt` only when
`sStDirty` (combine, othermode, geometry mode, colours, viewport, scissor, prim depth) or `sTexDirty`
(texture, tiles, loads, TLUT, othermode H) is set, and gives `rb_draw` a serial that changes only when the
state's bytes change: the backend's per-triangle 250-byte `memcmp` is an integer compare. `port_now()` is per
batch, in the backend. A node CPU profile then showed 59% of the interpreter in `import_tile`'s byte-wise FNV
over the texels (once per texture per frame, by design: texel memory can change between frames); a four-lane
32-bit hash (xxHash32 round, `__builtin_memcpy` loads since the port compiles with `-fno-builtin`) took the
node interpreter from 0.31 to 0.09 ms per drawn frame (FD, Metal Mario + polygons). LRU: decoded textures and
GL textures carry a frame stamp and go after 600 unused frames; programs are hashed (was a linear scan of up
to 512), evicted least-recently-used when the table is full, and their shader objects are deleted after
linking (one shared vertex shader). Memory: four 11,287-frame Sector Z matches in one process (45k frames,
12.5 min of game time) keep 268 decoded textures live throughout (4,427 evicted); in the browser the GL
texture gauge equals the decoded one (perf lines `texLive`/`glTexLive`). Pixels: identical to the base at all
17 fixed frames.

**Data fix found on the way.** Phase 2 stopped discarding alpha-0 texels in opaque AA modes (see below), and
Final Destination's platform seams turned bright green. The trace showed their CI4 palette as `0100 0108
0308 0310`: halfword-swapped, true values `0001 0801 0803 1003`, opaque dark red, which is what the reference
pictures show (SmashWiki `FinalDestinationSSB.png`, `FD64.jpg`: red seams). The palettes are named only in
`void *` material-animation tables, a form `packdata.rs rdp_block_names` did not know. Fixed for names
whose definition in the same file is a palette or texture block; 1,496 bytes of the image change (fighters'
and stages' animated palettes: 334 names, `RDPNAMES=1` lists them). Hash logs unchanged with the new image.

**2. Blender.** `decode_blender` (f3dex2.c) replaces `alpha_blend` (FORCE_BL and 1-A), the G_FOG-gated
fog mix and the ALPHA_CVG_SEL edge discard. Rules: 1-cycle runs cycle 1's fields; in 2-cycle the first
cycle always blends and is evaluated in the shader; the final cycle blends only with FORCE_BL (without it a
pixel inside a triangle has full coverage and is written as P); A_IN is coverage (1) under ALPHA_CVG_SEL
without CVG_X_ALPHA; CVG_X_ALPHA discards alpha < 1/8 (coverage rounds to 0); memory terms become GL blend
factors. Every state the game uses (12 stages x 2,400 frames, `coverage.txt`: 429 rows, 149 before texture addressing
was recorded; G_FOG is set in none, so all fog is the blender's G_RM_FOG_PRIM_A) maps as follows
(`node blendmap.mjs coverage.txt`):

| kind | othermode L | cycle | draws | cycle 1 | cycle 2 | flags | shader | GL |
|---|---|---|---|---|---|---|---|---|
| tri | c4112078 | 2-cycle | 3085294 | FOG*A_FOG + IN*1-A | IN*A_IN + MEM*A_MEM | AA_EN, ALPHA_CVG_SEL, ZMODE_OPA | cin = FOG*A_FOG + IN*1-A; out = IN; A_IN = 1 (coverage) | off |
| tri | 00552078 | 1-cycle | 450530 | IN*A_IN + MEM*A_MEM | - | AA_EN, ALPHA_CVG_SEL, ZMODE_OPA | out = IN; A_IN = 1 (coverage) | off |
| tri | 005041c9 | 1-cycle | 261162 | IN*A_IN + MEM*1-A | - | alpha compare, AA_EN, FORCE_BL, ZMODE_OPA | out = IN, alpha A_IN; A_IN = combined alpha | SRC_ALPHA, 1-SRC_ALPHA |
| rect | 0f0a4001 | 1-cycle | 188727 | IN*0 + IN*1 | - | alpha compare, FORCE_BL, ZMODE_OPA | out = IN*0 + IN*1; A_IN = combined alpha | off |
| rect | 00504241 | 1-cycle | 166442 | IN*A_IN + MEM*1-A | - | alpha compare, FORCE_BL, ZMODE_OPA | out = IN, alpha A_IN; A_IN = combined alpha | SRC_ALPHA, 1-SRC_ALPHA |
| tri | 00552048 | 1-cycle | 159242 | IN*A_IN + MEM*A_MEM | - | AA_EN, ALPHA_CVG_SEL, ZMODE_OPA | out = IN; A_IN = 1 (coverage) | off |
| tri | 00553079 | 1-cycle | 91084 | IN*A_IN + MEM*A_MEM | - | alpha compare, AA_EN, CVG_X_ALPHA (edge discard), ALPHA_CVG_SEL, ZMODE_OPA | out = IN; A_IN = combined alpha | off |
| tri | 00553049 | 1-cycle | 39245 | IN*A_IN + MEM*A_MEM | - | alpha compare, AA_EN, CVG_X_ALPHA (edge discard), ALPHA_CVG_SEL, ZMODE_OPA | out = IN; A_IN = combined alpha | off |
| tri | 005049d9 | 1-cycle | 35798 | IN*A_IN + MEM*1-A | - | alpha compare, AA_EN, FORCE_BL, ZMODE_XLU | out = IN, alpha A_IN; A_IN = combined alpha | SRC_ALPHA, 1-SRC_ALPHA |
| rect | 00504341 | 1-cycle | 32379 | IN*A_IN + MEM*1-A | - | alpha compare, FORCE_BL, ZMODE_OPA | out = IN, alpha A_IN; A_IN = combined alpha | SRC_ALPHA, 1-SRC_ALPHA |
| rect | 00504345 | 1-cycle | 22709 | IN*A_IN + MEM*1-A | - | alpha compare, FORCE_BL, ZMODE_OPA, ZS_PRIM | out = IN, alpha A_IN; A_IN = combined alpha | SRC_ALPHA, 1-SRC_ALPHA |
| tri | 005049d8 | 1-cycle | 21746 | IN*A_IN + MEM*1-A | - | AA_EN, FORCE_BL, ZMODE_XLU | out = IN, alpha A_IN; A_IN = combined alpha | SRC_ALPHA, 1-SRC_ALPHA |
| rect | 0f0a4001 | copy | 20944 | IN*0 + IN*1 | - | alpha compare, FORCE_BL, ZMODE_OPA | copy/fill: texel or fill colour; A_IN = combined alpha | off |
| tri | 00504341 | 1-cycle | 8621 | IN*A_IN + MEM*1-A | - | alpha compare, FORCE_BL, ZMODE_OPA | out = IN, alpha A_IN; A_IN = combined alpha | SRC_ALPHA, 1-SRC_ALPHA |
| tri | 005049d9 | 2-cycle | 8568 | IN*A_IN + MEM*1-A | IN*A_IN + MEM*1-A | alpha compare, AA_EN, FORCE_BL, ZMODE_XLU | cin = IN*A_IN + IN*1-A; out = IN, alpha A_IN; A_IN = combined alpha | SRC_ALPHA, 1-SRC_ALPHA |
| tri | 0c193049 | 2-cycle | 5532 | IN*0 + IN*1 | IN*A_IN + MEM*A_MEM | alpha compare, AA_EN, CVG_X_ALPHA (edge discard), ALPHA_CVG_SEL, ZMODE_OPA | cin = IN*0 + IN*1; out = IN; A_IN = combined alpha | off |
| tri | 0c192078 | 2-cycle | 5504 | IN*0 + IN*1 | IN*A_IN + MEM*A_MEM | AA_EN, ALPHA_CVG_SEL, ZMODE_OPA | cin = IN*0 + IN*1; out = IN; A_IN = 1 (coverage) | off |
| tri | 0c1849d9 | 2-cycle | 3444 | IN*0 + IN*1 | IN*A_IN + MEM*1-A | alpha compare, AA_EN, FORCE_BL, ZMODE_XLU | cin = IN*0 + IN*1; out = IN, alpha A_IN; A_IN = combined alpha | SRC_ALPHA, 1-SRC_ALPHA |
| tri | 005041c8 | 1-cycle | 2702 | IN*A_IN + MEM*1-A | - | AA_EN, FORCE_BL, ZMODE_OPA | out = IN, alpha A_IN; A_IN = combined alpha | SRC_ALPHA, 1-SRC_ALPHA |
| tri | c4113079 | 2-cycle | 2651 | FOG*A_FOG + IN*1-A | IN*A_IN + MEM*A_MEM | alpha compare, AA_EN, CVG_X_ALPHA (edge discard), ALPHA_CVG_SEL, ZMODE_OPA | cin = FOG*A_FOG + IN*1-A; out = IN; A_IN = combined alpha | off |
| rect | 00504245 | 1-cycle | 1213 | IN*A_IN + MEM*1-A | - | alpha compare, FORCE_BL, ZMODE_OPA, ZS_PRIM | out = IN, alpha A_IN; A_IN = combined alpha | SRC_ALPHA, 1-SRC_ALPHA |
| rect | 005041c8 | 1-cycle | 213 | IN*A_IN + MEM*1-A | - | AA_EN, FORCE_BL, ZMODE_OPA | out = IN, alpha A_IN; A_IN = combined alpha | SRC_ALPHA, 1-SRC_ALPHA |
| tri | 00553048 | 1-cycle | 158 | IN*A_IN + MEM*A_MEM | - | AA_EN, CVG_X_ALPHA (edge discard), ALPHA_CVG_SEL, ZMODE_OPA | out = IN; A_IN = combined alpha | off |
| rect | 005041c9 | 1-cycle | 98 | IN*A_IN + MEM*1-A | - | alpha compare, AA_EN, FORCE_BL, ZMODE_OPA | out = IN, alpha A_IN; A_IN = combined alpha | SRC_ALPHA, 1-SRC_ALPHA |
| rect | 00504340 | 1-cycle | 45 | IN*A_IN + MEM*1-A | - | FORCE_BL, ZMODE_OPA | out = IN, alpha A_IN; A_IN = combined alpha | SRC_ALPHA, 1-SRC_ALPHA |
| tri | 00504340 | 1-cycle | 14 | IN*A_IN + MEM*1-A | - | FORCE_BL, ZMODE_OPA | out = IN, alpha A_IN; A_IN = combined alpha | SRC_ALPHA, 1-SRC_ALPHA |

Not represented, and why: AA edge blending on partial coverage (the RDP's coverage AA; MSAA in phase 4 stands
in for it); CLR_ON_CVG and CVG_DST (coverage memory only); ZMODE_XLU/INTER (depth comparisons are the same in
GL; decal keeps the polygon offset and is never used by this game); G_ZS_PRIM on triangles (only rectangles
use it here, and those already take the prim depth); MEM inside the first of two cycles (never used).

What it changed on screen: the fighters' colour animations (`ftdisplaymain.c`: hit flashes, respawn and
invincibility tints, damage colours) are G_RM_FOG_PRIM_A in cycle 1 with the fog colour's alpha as the
amount; they were never drawn. Opaque surfaces with alpha-0 texels now draw them (the hardware does: alpha
is not coverage in those modes), which is what exposed the palette bug above. Meta Crystal's girder panels
come out pale yellow-green as in SmashWiki `MetaCrystal.png` (they were magenta).

**3. Near clipping.** Sutherland-Hodgman against z = -w and w = 1e-5 in `emit_tri`, attributes linear in
clip space, the cull test on the clipped polygon, a fan out. GFXTRACE at FD frame 296: largest trace box
|NDC| 27 (the floor running off screen, which GL clips) instead of 4,093. 4,368 triangles clipped in 3,000 FD
frames. No pixel changed at the fixed frames: GL's homogeneous clipping already drew these the same, and the
streaks at GO in `meta_crystal_go_before.png` / FD frames 250-400 are still there in every build. They are
game geometry (thin beams from the top right across FD at the countdown, a brown crystal shard over Meta
Crystal's fourth damage counter); the old renderer drew the FD beams black and the new one draws them in
their vertex colours. Whether the hardware shows them as here could not be checked without a capture.

**4. Picture.** `PortSetPicture(aa, filter, n64)`; the lobby's Picture button cycles Smooth (4x MSAA, the
default), Sharp (none) and N64 pixels (drawn at 240 lines, 320 wide at 4:3, scaled up unfiltered); 3-point
filtering is on in all three. `?picture=`, `?aa=0|1`, `?filter=0|1`, `?n64=0|1` override. MSAA renders into a
multisampled RGBA8 + depth24 framebuffer, resolves into a single-sample one (a multisampled blit cannot
convert to the RGB canvas or scale) and blits to the canvas. No multisampling or an incomplete framebuffer
falls back to drawing into the canvas (`fallback.mjs`: 5/5, the match draws in each case). With
`aa=0&filter=0` the picture is identical to phase 3. Defaults chosen without a real GPU: MSAA costs GPU fill
only, which SwiftShader's numbers below do not show; if a weak laptop struggles, Sharp is one click.

**5. Vertex uploads.** The batches of a frame are recorded and replayed after one `glBufferData` (was one per
batch). Identical pixels.

## spec/fast3d-lessons.md (owner-approved addition)

Each idea checked against the game first (coverage now records texture addressing; the node runner prints
rejects, far-plane cuts and why state serials change). No libultraship code was copied: the 3-point filter,
framebuffer target and tile clamp are this branch's own code written from the ideas, so no notice is needed.

| idea | done | evidence / effect |
|---|---|---|
| 2.1 half-texel offset under bilinear | yes | every textured triangle in a match is G_TF_BILERP; subtle shift everywhere, largest on the 16x8 crystal env maps |
| 2.2 3-point filter, NEAREST sampler | yes (phase 4) | own GLSL `tex3`, now on the RDP lattice |
| 2.3 MSAA / native-res FBO + blit | yes (phase 4) | MSAA -> same-size resolve -> blit, as the lessons note |
| 2.4 only the tiles the combiner reads | yes | texture lookups 260 -> 178 per frame (FD); no cycle-2 combiner reads a texel, one 1-cycle crossfade reads TEXEL1 |
| 2.5 shader clamp to the tile | yes | 641 k clamp+mirror and 525 k clamp-larger-than-mask draws sample outside their tile; Mario's cap emblem and Hyrule's wall edges fixed |
| 2.6 mask 0 + wrap = clamp | no | 0 draws in the coverage dump |
| 2.7 copy mode at NEAREST | yes | Yoshi's Island backdrop (the only copy-mode state) crisp (`copymode_*`) |
| 2.8 decal offset scaled to height | yes | no ZMODE_DEC state in matches: no visible change |
| 2.9 depth clears | yes, part | the one Z fill a frame comes before any draw: skipped. Later ones still clear everything (scissoring to the N64 rectangle would miss the widescreen bands) |
| 2.10 trivial frustum rejection | yes | 41-221 triangles a frame dropped before state work; far-plane crossings 0 on all 7 stages |
| 2.11 prim/env as vertex attributes | no | serial changes: texture 25-41 a frame, combiner 6-14, colour only 0.1-3: nothing to gain |
| 2.12 address-keyed texture cache | no | the lessons rank it last; the four-lane hash made hashing cheap (node interpreter 0.09 ms) and the content hash is what catches data-image errors |

## Skipped, and why

- WebGPU backend (spec 4C, phase 5's other option): ~1,000 WebGL calls a frame after phase 1 is no longer
  where time goes, a second shader generator, partial Safari/Firefox support, and the SwiftShader proofs.
- libultraship adoption (4B) and frame interpolation: the spec says not to; nothing found here changes that.
- Noise as the RDP's animated speckle (gap 6), LOD/mipmaps (5), G_TEXTURE_GEN_LINEAR (7), texture-coordinate
  detail (8), 16-bit depth emulation (9), SIMD vertices (11), rectangles under a viewport (12): the spec ranks
  them cosmetic, unused by the game, or not a bottleneck, and nothing in this work contradicted that.

## Proofs

Perf (headless Chromium + SwiftShader, 900 frames between sim frames 600 and 1500, `hhcc`, one run each,
before = the branch base `3d049486d`, after = `ebab64936` with the default Smooth picture, after-sharp (aa=0) =
Sharp; GL calls counted in JS by wrapping every WebGL2 method, so both builds are measured the same way;
"interpreter" is `gfxrun - rbdraw` and the two builds split that line differently: before, `rbdraw` timed
every per-triangle `rb_draw` call; after, only the batch submission):

| build | stage | fighters | ms per drawn frame (JS) | interpreter ms | GL submit ms | draws | binds | program switches | GL calls | texture hashes |
|---|---|---|---|---|---|---|---|---|---|---|
| before | Meta Crystal | 13,0,2,3 | 2.124 | 0.86 | 0.631 | 133 | 133 | 43 | 1064 | 180 |
| after | Meta Crystal | 13,0,2,3 | 1.18 | 0.37 | 0.31 | 132 | 108 | 47 | 1006 | 126 |
| after-sharp | Meta Crystal | 13,0,2,3 | 1.295 | 0.40 | 0.342 | 132 | 108 | 47 | 996 | 126 |
| before | Final Destination | 13,14,17,3 | 2.213 | 1.01 | 0.626 | 149 | 161 | 27 | 968 | 209 |
| after | Final Destination | 13,14,17,3 | 0.984 | 0.33 | 0.25 | 107 | 103 | 31 | 731 | 143 |
| after-sharp | Final Destination | 13,14,17,3 | 1.314 | 0.54 | 0.257 | 107 | 103 | 31 | 721 | 143 |
| before | Dream Land | 0,1,2,3 | 2.015 | 0.93 | 0.612 | 172 | 158 | 67 | 1472 | 187 |
| after | Dream Land | 0,1,2,3 | 1.305 | 0.52 | 0.303 | 169 | 133 | 80 | 1499 | 138 |
| after-sharp | Dream Land | 0,1,2,3 | 1.245 | 0.37 | 0.439 | 169 | 133 | 79 | 1487 | 138 |
| before | Peach's Castle | 4,5,6,7 | 2.024 | 0.83 | 0.682 | 188 | 200 | 72 | 1675 | 207 |
| after | Peach's Castle | 4,5,6,7 | 1.51 | 0.58 | 0.473 | 188 | 144 | 98 | 1772 | 143 |
| after-sharp | Peach's Castle | 4,5,6,7 | 1.414 | 0.52 | 0.446 | 188 | 144 | 98 | 1762 | 143 |
| before | Hyrule | 8,9,10,11 | 1.666 | 0.69 | 0.556 | 146 | 139 | 53 | 1245 | 153 |
| after | Hyrule | 8,9,10,11 | 1.042 | 0.27 | 0.399 | 126 | 105 | 49 | 1014 | 107 |
| after-sharp | Hyrule | 8,9,10,11 | 1.098 | 0.31 | 0.346 | 126 | 106 | 49 | 1004 | 107 |
| before | Sector Z | 1,5,9,11 | 1.897 | 0.79 | 0.681 | 215 | 214 | 68 | 1763 | 229 |
| after | Sector Z | 1,5,9,11 | 1.239 | 0.41 | 0.415 | 181 | 139 | 93 | 1664 | 140 |
| after-sharp | Sector Z | 1,5,9,11 | 1.283 | 0.44 | 0.389 | 181 | 139 | 93 | 1654 | 140 |
| before | Duel Zone | 14,15,16,17 | 1.364 | 0.62 | 0.388 | 73 | 72 | 14 | 497 | 177 |
| after | Duel Zone | 14,15,16,17 | 1.048 | 0.40 | 0.248 | 74 | 69 | 16 | 464 | 118 |
| after-sharp | Duel Zone | 14,15,16,17 | 0.838 | 0.23 | 0.237 | 74 | 69 | 16 | 455 | 118 |

Reading: a drawn frame's JS time is 25-55% lower (1.37-2.21 ms -> 0.98-1.51 ms), texture binds 10-35%
fewer, texture hashes ~30% fewer, draws equal or fewer (FD 149 -> 107, Sector Z 215 -> 181: the trivial
rejection and the dropped second tile). Program switches rose 5-35% (more shader variants: 3-point, tile
clamp, blender keys). One run per row on a loaded 16-core machine (other agents' tests ran): rows move by
~0.1-0.3 ms between runs (Sharp is sometimes slower than Smooth, which cannot be a real effect: MSAA's
cost is GPU fill, which SwiftShader does in the GPU process and the JS timer does not see). Node runner,
no GL, FD: interpreter 0.31 -> 0.09 ms per drawn frame. `perf_final.txt` and `perf_phase1.txt` hold the
JSON lines (rafFps 60 everywhere).

Hash logs: `proof/state-machines/hashlogs.sh` 57/57 identical to the base build after every phase (the base
itself 57/57 identical to `~/sb64/ssb64-spec`'s build, the baseline `proof/gfx` used). That script never
draws (the runner's draw flag is 0), so `hashlogs_draw.sh` runs the same 54 matches with every frame through
the interpreter: 54/54 equal to the undrawn base hashes after phases 1, 2, 3, 5 and the last lessons step
(the final build).

Suites (`suites.sh`, logs in `suites/`, this worktree's relay and web/, PORT_BASE=18400, final build), counted
as lines starting `ok`/`FAIL`: net_proof 16/16, away_proof 9/9, modes_proof 9/9, lobby_proof 13/13,
lobby8_proof 5/5, lobby_look 11/11, extras_proof 23/23, extras_lobby_proof 16/16, sound_proof 1/1 (its one
check: both pages heard the same 139 sounds), bgm_proof 1/1 (its one check, old/new rule restarts 5 vs 1).
All exit 0. Plus `fallback.mjs` 5/5 and `picture_button.mjs` 4/4 (the button shows, cycles, survives a
reload; `suites/shots/lobby_go_row.png`).

Screenshots (`shots/`, 1000x750 canvas, the offline seeded match, frame held exactly by `shots.mjs`):
`before_s<stage>_f<frame>.png` and `after_...` for Meta Crystal (13: Metal Mario, Mario, DK, Samus; GO at 250
and 400), Final Destination (16: Metal Mario, two polygons, Samus; 250, 296, 400 for the GO intro), Dream
Land (6), Peach's Castle (0), Hyrule (4), Sector Z (1), Duel Zone (14: four polygons), each at 400 and 1200;
`n64_*` the N64-pixels picture, `plain_*` aa=0&filter=0 (both taken at phase 4, before the lessons steps),
`copymode_*` Yoshi's Island before/after the copy-mode fix, `tileclamp_mario_cap_s13_f1200.png` (before,
after, changed pixels). All viewed. Verdict per stage, before -> after:

- Meta Crystal: crystals unchanged in shape; girder panels pale yellow-green instead of magenta (matches the
  SmashWiki picture); Mario's cap emblem right; hit flashes on Mario at 400/1200; edges anti-aliased. The
  brown shard over the fourth damage counter at GO is in both (game geometry).
- Final Destination: platform seams and the underside tip dark red (were missing / black), as in the
  reference pictures; the GO-intro beams are drawn in their vertex colours (were black); polygons and
  Metal Mario flash on hits; polygon Samus's palette-animated limbs paler (the palette fix). No streak
  artefacts beyond the beams, which both builds draw.
- Dream Land: respawning fighters on the revival platform tinted white (the respawn colour animation);
  tree and stage textures sharper by the half-texel/3-point change; otherwise the same.
- Peach's Castle: Yoshi's hit flash at 400; bridge truss edges a few pixels different (alpha-tested edges);
  otherwise the same.
- Hyrule: wall and roof tile edges no longer bleed the opposite texel row (shader tile clamp); otherwise
  the same.
- Sector Z, Duel Zone: the same picture, smoother edges and slightly different texel lattice.
- GO intro (FD 250/296/400, Meta Crystal 250/400): as above; near clipping changed no pixel there.

Regressions seen: none that I could identify as wrong. Honest uncertainties: (1) the FD beams at GO used to
be black and are now coloured; I have no hardware capture of that moment. (2) Opaque AA surfaces now draw
alpha-0 texels (hardware semantics as I read the RDP and as libultraship does); FD's seams were the one
place this showed garbage, and that was bad data, now fixed; another undiscovered bad palette would show
the same way. (3) 3-point filtering and MSAA are on by default without a look on a real GPU.

## Tools

`build.sh` (page and node builds into this worktree's web/), `shots.mjs`/`shots_all.sh`/`diff_all.sh` (the 17
fixed frames, pixel counts by ImageMagick AE), `perf.mjs`/`table.mjs`, `coverage.mjs`/`blendmap.mjs`,
`hashlogs_draw.sh`/`hashcmp.sh`, `fallback.mjs`, `picture_button.mjs`, `suites.sh`. Final build id `49c2190f7b082d3f`, data `16ffa10ab963edb9` (sha256 prefixes, `web/build.js`).
