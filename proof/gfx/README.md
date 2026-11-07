# Meta Crystal, Metal Mario, and the "slightly laggy" match

Branch `gfx-metal-crystal` (worktree `~/sb64/ssb64-gfx`) on top of `101a89370`. Not deployed, not pushed.
Everything here ran against a private static server on a random port with the worktree's own
`build/us/port/devweb`; the live site was not touched.

## 1. Meta Crystal and Metal Mario: the data image, not the renderer

Report: crystals render as flat, saturated, vertex-coloured facets; Metal Mario is white with coloured
outlines. First suspicion was the texgen path in `port/gfx/f3dex2.c` `do_vtx`.

Checked and cleared:
- Texgen formula. The port computes `U = (dot + 1) * 512 * 32 * scale/65536` in s10.5, i.e.
  `(dot + 1)/4 * scale` (`port/gfx/f3dex2.c:218`). libultraship's Fast3D (`SpaghettiKart/libultraship/src/fast/interpreter.cpp:1658-1687`)
  computes `(dot + 1)/4 * texture_scaling_factor` and GLideN64 `(dot + 1) * 512 * scale/65536`: the same.
  The game's scales confirm it: Metal Mario's `gsSPTexture(0x0BC0, 0x0A40)` on a 48x42 map gives U 0..47,
  V 0..41; the crystals' `(0x0400, 0x0200)` on 16x8 gives 0..16, 0..8. Both span their map exactly once.
- Lookat transform. Both sides transform the lookat vectors into model space by the transpose of the
  modelview 3x3 and normalise (`f3dex2.c:189-195` vs `interpreter.cpp CalculateNormalDir`).
- Lookat delivery. `gSPLookAtX/Y` (`src/sys/objdisplay.c:2946`) arrive as `G_MOVEMEM` idx 10, offsets 0 and 24;
  a one-frame trace (`GFXTRACE=400`) shows `lookat0=127,0,4 lookat1=0,127,-12`, i.e. the camera's right and
  up axes as `syMatrixLookAtReflectF` writes them (`src/sys/matrix.c:337-342`).
- Lighting. `numlights=1`, light and ambient both white for Metal Mario (`DB0A0000/DB0A0018 FFFFFF00` in
  `MMarioModel/Joint_*.dl.inc.c`), so SHADE is white and his colour is the chrome map alone; the
  combiner `FC127E05 FF17F3FF` is `T0*SHADE` then `COMBINED*ENV`, ENV white. Nothing to fix there.

Root cause: the texels and palettes were byte-swapped in the data image. The trace printed the loaded
bytes: crystal map `f995 d9de ...` where the ROM has `95F9 DED9 ...`, Metal Mario's LUT `0100 0100 0108`
where the ROM has `0001 0001 0801`. The interpreter reads texel memory big-endian as the RDP does
(`f3dex2.c decode()`), so every CI texel went through the wrong palette entry: the crystals picked the
map's few saturated entries, Metal Mario's entries came out with alpha 0 (holes) or white.

Why: `tools/port/src/bin/packdata.rs rdp_block_names` decides which blocks to keep in ROM byte order. It
recognised `gsDPSetTextureImage(` macros, `(int*)` casts, `(u32)d*_lut` and pointer tables, but not
display lists expanded as raw words, `{ { 0xFDxxxxxx, (u32)dSym } }`, which is how `117_StageMetalFile2`
and `300_MMarioModel` (and 32 other files) name their images. Blocks typed `u16` in those files
(`dStageMetalFile2_palette_*`, `dMMarioModel_Lut256_0x2288`) were therefore halfword-swapped like any
other u16 array. The fix adds that pattern. 300 more blocks are kept big-endian, 161 of them with bytes
that differ (7958 bytes of the 21.9 MB image). `u8` textures were never affected.

Note: `packdata.rs` was never in git: `.gitignore` line 12 is the bare pattern `bin`, which matches
`tools/port/src/bin/`. The branch force-adds it.

Screenshots (headless Chromium, SwiftShader, offline seeded match `netN=1 modes=hhhh`):

| | before | after |
|---|---|---|
| Meta Crystal, frame 1200, Metal Mario + Fox + DK + Samus | `meta_crystal_before.png` | `meta_crystal_after.png` |
| Meta Crystal at GO, frame 400 | `meta_crystal_go_before.png` | `meta_crystal_go_after.png` |
| Final Destination, frame 1200, Metal Mario at left | `metal_mario_fd_before.png` | `metal_mario_fd_after.png` |

Reference pictures (not copied here): SmashWiki `MetaCrystal.png` (pale grey-white crystals, dark red
girders) and `Metal_Mario_SSB.png` (white-silver chrome with red/blue/yellow rim reflections). After the
fix the crystals are pale with smooth gradients and the girders dark; Metal Mario is silver chrome.

Simulation unchanged: the 57 phase-1 hash logs (`proof/state-machines/hashlogs.sh`, 9 stages x 6 modes x
12000 frames, 3 matches in one process, `RB=15` rollback byte-compare with 0 mismatches, sound count) are
identical between the baseline build (`~/sb64/ssb64-spec`, same commit, old data) and this branch (new
data, new wasm): 57/57 same.

## 2. "Felt slightly laggy": the renderer's per-triangle draw path

Measured with the new frame profile (`port/loop/prof.c`, `PortProf(i)`; `PROF=1` in the node runner,
`perf.mjs` in the browser). All figures are CPU time inside the wasm per frame; GL time under SwiftShader
is CPU time too, so browser figures are an upper bound for a real GPU's driver cost, and Chrome's
`performance.now` is 100 us-grained, so the per-phase browser splits are approximate while the JS
wall time per `StepFrame(1)` (thousands of us) is solid.

Node, no GL (`rb_null`), 1500 frames, `hhcc`, per frame, before this branch's renderer changes
(`prof_node.txt`):

| stage | fighters | sim update | DL build | interpreter | tris | draws | texture hashes |
|---|---|---|---|---|---|---|---|
| Meta Crystal | Metal Mario,Mario,DK,Samus | 0.019 ms | 0.011 | **0.554** | 892 | 575 | 926 |
| Meta Crystal | Mario,Fox,DK,Samus | 0.019 | 0.011 | 0.335 | 800 | 535 | 840 |
| Final Destination | with Metal Mario | 0.021 | 0.015 | 0.742 | 1060 | 681 | 1034 |
| Final Destination | without | 0.020 | 0.015 | 0.501 | 954 | 641 | 942 |
| Dream Land | with Metal Mario | 0.018 | 0.012 | 0.752 | 917 | 581 | 933 |
| Dream Land | without | 0.017 | 0.012 | 0.517 | 830 | 541 | 845 |

The simulation is ~20 us a frame; a 12-frame rollback (restore + 12 resims) is 0.12-0.16 ms
(`prof_rb.txt`, `RB=15 RBK=12 RBTIME=1`). Rollbacks are not where time goes. The interpreter was 20-40x
the sim, and Metal Mario added 0.22-0.27 ms on every stage: `import_tile` hashed the full texel block and
palette for every triangle, and his 48x42 CI8 chrome map (2016 + 512 bytes) is 5x a normal fighter's
32x32 CI4 texture.

Browser (headless Chromium + SwiftShader, 900 frames between sim frames 600 and 1500), JS wall time of one
drawn `StepFrame(1)` and the interpreter/GL split (`perf_browser_before.txt`, `perf_browser_after.txt`):

| stage, fighters | before: step / interpreter / GL (ms) | draws | after: step / interpreter / GL (ms) | draws |
|---|---|---|---|---|
| Meta Crystal, Metal Mario | 3.87 / 3.30 / 2.11 | 641 | 2.05 / 1.47 / 0.61 | 133 |
| Meta Crystal, Mario | 3.30 / 2.73 / 1.87 | 610 | 1.85 / 1.35 / 0.61 | 159 |
| Dream Land, Metal Mario | 3.90 / 3.45 / 1.97 | 675 | 1.52 / 1.17 / 0.44 | 151 |
| Dream Land, Mario | 3.65 / 3.17 / 2.00 | 615 | 2.12 / 1.57 / 0.67 | 172 |
| Final Destination, Metal Mario | 4.46 / 3.87 / 2.37 | 778 | 2.05 / 1.56 / 0.64 | 178 |
| Peach's Castle, Mario | 2.95 / 2.56 / 1.74 | 583 | 1.89 / 1.42 / 0.65 | 169 |

Before: one GL draw per triangle with the full state re-sent each time (program, 9 uniforms, two texture
binds with 4 sampler parameters each, blend, depth, polygon offset, viewport, scissor, buffer upload,
draw: ~27 GL calls, ~17,000-21,000 WebGL calls a frame). Every WebGL call crosses from wasm to the GPU
process, so on a real GPU this is main-thread time of the same order as under SwiftShader. Meta Crystal is
not special: Final Destination and Dream Land cost more. Metal Mario adds 7-15% draws and, before the
memo, 0.2-0.3 ms of hashing, so "Meta Crystal with Metal Mario" is the ordinary cost of this renderer plus
Metal Mario's premium, on a machine close to its budget. The frame maxima of 14-17 ms (and one 37 ms) are
SwiftShader's shader compiles and GC, not the steady state.

After (this branch): a per-frame memo of texture keys, draws batched while the state is byte-identical
and flushed in order, a GL state cache. Draw calls 580-780 -> 130-180, texture binds ~1000 -> ~150, GL
calls ~20,000 -> ~1,500 a frame; a drawn frame's CPU time halves. Pixels at the same frame are identical
(`compare -metric AE` on the game canvas: 0 differing pixels; the only differences are the status strip's
frame counter).

Honest limits: this is one machine with a software GL. A frame is now ~2 ms of CPU under SwiftShader and
would be well under 1 ms with a GPU; whether the owner's machine was over budget before cannot be known
from here, only that the renderer was doing 10x the GL work it needed to and that Metal Mario made that
worse.

## Files

- `meta_crystal_*.png`, `metal_mario_fd_*.png`: screenshots above.
- `prof_node.txt`, `prof_rb.txt`: node profiles (before the renderer changes).
- `perf_browser_before.txt`, `perf_browser_after.txt`: browser profiles, one JSON line per run.
- `perf.mjs`: the browser profiler (`node perf.mjs <webdir> <stage> <fighters> [modes] [from] [to]`).
- `shots2.mjs`: screenshots from any web folder (`node shots2.mjs <webdir> <outdir> <stage> <fighters> <frames...>`).
