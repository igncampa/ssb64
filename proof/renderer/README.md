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
factors. Every state the game uses (12 stages, `coverage.txt`, 149 states) maps as follows
(`node blendmap.mjs coverage.txt`):

BLENDMAP

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

## Skipped, and why

- WebGPU backend (spec 4C, phase 5's other option): ~1,000 WebGL calls a frame after phase 1 is no longer
  where time goes, a second shader generator, partial Safari/Firefox support, and the SwiftShader proofs.
- libultraship adoption (4B) and frame interpolation: the spec says not to; nothing found here changes that.
- Noise as the RDP's animated speckle (gap 6), LOD/mipmaps (5), G_TEXTURE_GEN_LINEAR (7), texture-coordinate
  detail (8), 16-bit depth emulation (9), SIMD vertices (11), rectangles under a viewport (12): the spec ranks
  them cosmetic, unused by the game, or not a bottleneck, and nothing in this work contradicted that.

## Proofs

Perf (headless Chromium + SwiftShader, 900 frames between sim frames 600 and 1500, `hhcc`, one run each,
before = the branch base `3d049486d`, after = `d8e453bbf` with the default Smooth picture, after (aa=0) =
Sharp; GL calls counted in JS by wrapping every WebGL2 method, so both builds are measured the same way;
"interpreter" is `gfxrun - rbdraw` and the two builds split that line differently: before, `rbdraw` timed
every per-triangle `rb_draw` call; after, only the batch submission):

PERFTABLE

Hash logs: `proof/state-machines/hashlogs.sh` 57/57 identical to the base build after every phase (the base
itself 57/57 identical to `~/sb64/ssb64-spec`'s build, the baseline `proof/gfx` used). That script never
draws (the runner's draw flag is 0), so `hashlogs_draw.sh` runs the same 54 matches with every frame through
the interpreter: 54/54 equal to the undrawn base hashes after phases 1, 2, 3 and 5.

Suites: SUITES

Screenshots (`shots/`, 1000x750 canvas, the offline seeded match, frame held exactly by `shots.mjs`):
`before_s<stage>_f<frame>.png` and `after_...` for Meta Crystal (13: Metal Mario, Mario, DK, Samus; GO at 250
and 400), Final Destination (16: Metal Mario, two polygons, Samus; 250, 296, 400 for the GO intro), Dream
Land (6), Peach's Castle (0), Hyrule (4), Sector Z (1), Duel Zone (14: four polygons), each at 400 and 1200;
`n64_*` the N64-pixels picture, `plain_*` aa=0&filter=0 (phases 1-3 only). VERDICTS

## Tools

`build.sh` (page and node builds into this worktree's web/), `shots.mjs`/`shots_all.sh`/`diff_all.sh` (the 17
fixed frames, pixel counts by ImageMagick AE), `perf.mjs`/`table.mjs`, `coverage.mjs`/`blendmap.mjs`,
`hashlogs_draw.sh`/`hashcmp.sh`, `fallback.mjs`, `suites.sh`. Build `BUILDID`, data `DATAHASH`.
