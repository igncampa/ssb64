# Widescreen for the browser port: spec

Written 5 Nov 2026 by Fable from a read of this repo (`src/`, `port/`, `web/`), the n64 portal
(`/home/desktop/n64/ARCHITECTURE.md`) and mk64's libultraship as the reference technique
(`/home/desktop/mk64/SpaghettiKart/libultraship/src/fast/interpreter.cpp`). Every claim about code has a
`file:line` next to it. Lines are from the trees as they stand today. Nothing was changed; no server was touched.

The owner asked for widescreen "just like mk64 has, not restricted to the N64 resolution", landscape only,
in a canvas that can be anything from 4:3 to about 21:9.

---

## 0. Verdict

**Possible, and safe for netplay, at low cost.** The simulation reads the screen size in exactly one place that
matters (Link's boomerang, section 3), and that place keeps the N64 constants. Everything else that knows about
320×240 is draw code: it builds display lists that the port's own interpreter turns into pixels. So the aspect
ratio can live entirely in `port/gfx/` plus one render-only hook, and clients at different aspect ratios keep
identical hashes.

What you get: the 3D stage and fighters fill the canvas at the correct proportions, showing more of the stage to the
left and right (33% more at 16:9, 75% more at 21:9). The camera still frames the fighters as if the screen were
4:3, so the extra width is stage, not a zoomed-in view. The HUD (damage, stocks, timer, tags, "GO!", "GAME SET")
stays centred in the 4:3 band, exactly as it does on mk64 for anything not explicitly anchored. The stage's painted
backdrop (the "wallpaper" every stage has, section 1.3) is stretched to fill the width, the way mk64 stretches its
skybox. Full-screen fades and flashes cover the whole canvas.

What is honest to say up front:

- The stages were authored for 4:3. Beyond the 4:3 edge you see geometry the artists never expected anyone to see.
  On most stages that is more castle, more jungle, more sky. On some it may be a visible edge of a model or the
  backdrop clamping. This needs eyes on every stage (phase W1 takes the screenshots); it cannot be judged from code.
- The off-screen magnifying glass and the off-screen arrows are decided with the 4:3 frame. Left alone, in
  widescreen you would see a fighter in the side band *and* a bubble for it. The spec handles this with one
  render-only hook (section 2.6). The bubble, when it does appear, sits at the 4:3 edge rather than the true edge;
  moving it out is an optional later phase.
- Link's boomerang disappears when it leaves the 4:3 frame plus a margin. In widescreen you may see it vanish
  while still visible in the side band. This is the one place where the sim's 4:3 shows through. It is identical on
  every client, so it is a cosmetic quirk, not a desync. Changing it would mean changing the sim (section 5, Q3).

Recommended scope: phases W0 to W4 below. Expected size: about 150 lines in `port/gfx/`, 40 in `web/`, 10 in
`src/` under `#ifdef PORT`, plus two proof scripts.

---

## 1. How the picture is made today (facts)

### 1.1 The game's screen

| Fact | Where |
|---|---|
| Framebuffer is 320×240, 16-bit. `gSYVideoResWidth/Height` are 320/240. | `include/config.h:10-11`; `src/sys/video.h:38-51, 74-75` |
| Every VS battle sets the camera's picture area to `(10, 10, 310, 230)`: a 300×220 picture with a 10-px black border. The border was overscan on a TV; the port shows it. | `src/sc/sccommon/scvsbattle.c:170, 442`; `src/gm/gmcamera.c:1575-1583` |
| `gGMCameraStruct.viewport_{ulx,uly,lrx,lry,center_x,center_y,width,height}` are set once there and never change during a match. | `src/gm/gmcamera.c:1575-1583` (only writer) |
| The main camera's perspective aspect is `(310-10)/(230-10) = 1.3636`, a float computed from those fields. Its fovy is 38°. | `src/gm/gmcamera.c:1159-1161, 1199`; `src/sys/objtypes.h:257` |
| The default scissor margins are 10 px on each side. | `src/sys/objdisplay.c:79-85` |
| Every camera's display proc emits `gSPViewport` plus a `gDPSetScissor` clamped to those margins, then a Z-buffer clear as a fill rectangle aimed at the depth image. | `src/sys/objdisplay.c:2620-2690` (`func_8001663C`), `2570-2615` (`func_80016338`) |
| The frame-start reset list sets a 320×240 viewport and identity matrices. | `src/sys/rdp.c:27-35`; `syRdpSetViewport` at `rdp.c:68-79` |

### 1.2 Cameras alive in a VS battle, in draw order

| Camera | Made at | Projection | Draws |
|---|---|---|---|
| default (black clear, Z-buffer) | `scvsbattle.c:166` via `objhelper.c:564` | none | clears |
| wallpaper camera | `scvsbattle.c:171`, `gmcamera.c:1257-1286` | none (sprites) | the stage backdrop sprite through `lbCommonDrawSprite` |
| battle camera | `scvsbattle.c:173`, `gmcamera.c:1114-1247` | perspective, aspect 1.3636, `gmCameraLookAtFuncMatrix` `gmcamera.c:985-1018` | stage, fighters, items, effects |
| player magnify camera | `gmcamera.c:1425-1452` | ortho ±150×±110 (`gmCameraOrthoLookAtFuncMatrix` `gmcamera.c:1383-1399`) and the fighter-in-bubble matrix `gmcamera.c:1345-1380` | magnifying-glass ring and the small fighter inside it |
| player arrows camera | `gmcamera.c:1471-1501` | ortho | off-screen arrows |
| interface camera | `gmcamera.c:1504-1530` | none (sprites) | HUD sprites: damage, stocks, timer, tags, announcements |
| screen flash camera | `gmcamera.c:1289-1337` | none | `ifScreenFlashProcDisplay`: a fill rectangle `(10,10,310,230)` `src/if/ifscreenflash.c:44` |
| effect camera | `gmcamera.c:1540-1570` | aspect from viewport | particles |

Two viewports exist: the camera picture `(10,10,310,230)` for every camera above, and the magnifier's small square,
`18 × magnify_scale` pixels (at most 54), built per fighter per frame in `src/if/ifcommon.c:1697`.

### 1.3 2D content

- **Sprites** (HUD, tags, backdrop) go through `lbCommonDrawSObjAttr`, which emits one `gSPTextureRectangle` per
  texture strip, each strip clipped in software to the sprite scissor (`src/lb/lbcommon.c:2286-2300, 2491`). The
  sprite scissor is the camera picture, 10..310 (`lbcommon.c:2972-2995`).
- **The backdrop** ("wallpaper") exists on every VS stage: Yoshi's Island gets a static one, Sector Z a scrolling
  one, every other stage the parallax one (`src/gr/grwallpaper.c:267-300`). The parallax one is a 300×220 image
  scaled 1.004 to 2.0 by camera distance and shifted by camera angle, then clamped so it always covers 10..310
  (`grwallpaper.c:45-124`). After the software clip, each strip is exactly 300 px wide.
- **Full-screen fills**: screen flash (`ifscreenflash.c:44`), fades (`src/lb/lbfade.c:72`), the wallpaper's black
  (`grwallpaper.c:25-35`), each `(10,10,310,230)`.
- **Billboards and particles** build their own matrices from `gGCMatrixPerspF` (`src/sys/objdisplay.c:795-930`;
  `src/lb/lbparticle.c:1497-1530, 1596-1625`). Draw only.

### 1.4 The port's renderer

| Fact | Where |
|---|---|
| The interpreter keeps the viewport and scissor in N64 pixels (`S.vp`, `S.sc`), defaults 320×240. | `port/gfx/f3dex2.c:41, 471-481` |
| `G_MOVEMEM` viewport → `S.vp = {x, y, w, h}` from vscale/vtrans. | `f3dex2.c:375-380` |
| Vertices are transformed to clip space in `do_vtx`; `d->x..w`. | `f3dex2.c:91-148`, esp. 121-124 |
| Triangles: backface test on `x/w`, then `rb_draw` with clip-space positions. | `f3dex2.c:262-293` |
| Rectangles: viewport forced to 320×240; NDC `x/160-1`, `1-y/120`; `rb_draw` with `rect=1`. | `f3dex2.c:295-328`, esp. 305, 310 |
| Fill rectangles aimed at the depth image clear depth; texture rectangles aimed anywhere but the colour image are dropped (the magnifier's mask). | `f3dex2.c:406, 412-413` |
| The backend maps N64 pixels to the canvas with `sx = W/320`, `sy = H/240` for both viewport and scissor. | `port/gfx/rb_webgl2.c:136, 156-157` |
| Canvas size is read once at context creation; `port_set_canvas_size` exists but nothing calls it and it is not exported. | `rb_webgl2.c:13, 104-106, 162`; `Makefile.port:31` |
| The canvas is a fixed 960×720 element, CSS `height:100%; max-width:100%; aspect-ratio:4/3`. | `web/index.html:9, 28` |
| Display lists live in the port's own arena, outside the snapshot. | `port/loop/sched.c:141-163`; snapshot ranges `port/net/snapshot.c:40-48` |
| Draw happens only on the last sim step of a browser tick, never in re-simulation. | `web/engine.js:85`; `web/lobby.js:114`; PLAN 3.4 |
| The null backend used by the node proofs discards everything. | `port/gfx/rb_null.c` |

### 1.5 What the portal does

The ssb64 page runs in an iframe that fills `#tv` (`/home/desktop/n64/www/index.html:68`). The page's `#stage` is
`position:fixed; inset:0` (`web/index.html:8`), so the canvas already follows the iframe's size. The portal needs
no change for this spec. The portal's `games/ssb64/embed.patch` touches `web/index.html` (end of file) and
`web/engine.js:11`; the edits below must keep that patch applying, or the patch is regenerated (section 5, R8).

---

## 2. Design

### 2.1 One number: k

Let the canvas be `W × H` pixels and `A = W / H`. Define

```
k = (4/3) / A          (k = 1 at 4:3, 0.75 at 16:9, 0.571 at 21:9)
```

clamped to `[4/7, 1]`. If the canvas is taller than 4:3 the page letterboxes to 4:3 instead (landscape only),
so `k` is never above 1.

Two ways to map an N64 x coordinate (0..320) to canvas pixels:

```
stretch(x) = x · W / 320                           covers the whole canvas width
centred(x) = (W − H·4/3) / 2 + x · H / 240         the 4:3 band in the middle; equals stretch(x) when k = 1
```

Note `H/240 = k · W/320`, so `centred(x) = W/2 + (x − 160) · k · W/320`.

### 2.2 The rule, in one paragraph

Everything is drawn into the 4:3 band at its original size and position, except that **the picture is allowed to
extend past the band's edges to the canvas edges**. Concretely: 3D geometry under the camera's full-size viewport
has its clip-space x multiplied by `k` and the viewport stretched to the canvas width, which is pixel-for-pixel the
same picture in the band and more world at the sides. Anything the game sized to the whole picture (the 300-wide
viewport, scissor, fills, backdrop strips) is stretched to the canvas; anything smaller (HUD sprites, the magnifier's
viewport and scissor) is placed in the band. This is libultraship's rule: `AdjXForAspectRatio` on rectangles
(`interpreter.cpp:1529-1537, 2802-2810`), viewports and scissors stretched by `RATIO_X`
(`interpreter.cpp:2268-2312`), and the game passing the real aspect to `guPerspective`
(`SpaghettiKart/src/engine/cameras/GameCamera.cpp:91`). The only difference is that here the game cannot be given
the real aspect (section 3), so the interpreter applies `k` to clip space instead. The two are mathematically the
same for a perspective or orthographic projection: scaling column 0 of the projection by `k` and scaling clip x by
`k` produce the same `x/w`.

Why clip space and not the projection matrix: the game also writes projection elements directly with
`gMoveWd(G_MW_MATRIX, …)` for billboards (`src/sys/objdisplay.c:760-790`) and loads concatenated matrices. Scaling
the final clip x catches every path; scaling `S.proj` in `OP_MTX` would miss some.

### 2.3 Classification: "wide" means spans the picture

```
wide(x0, x1)  :=  (x1 − x0) >= 300        in N64 pixels
```

- Viewport (`S.vp[2] >= 300`): the camera picture (300) and the frame-start 320. Not the magnifier (≤ 54).
- Scissor (`S.sc[2] − S.sc[0] >= 300`): the camera scissor 10..310 and the reset 0..320. Not the magnifier's.
- Fill rectangle: the Z clear, screen flash, fade, wallpaper black, all 300 wide. Nothing smaller in a match.
- Texture rectangle: the backdrop strips are exactly 300 wide after the game's own clip (section 1.3). HUD
  sprites are far smaller: digits, emblems, stock icons and tags are tens of pixels; the announcement banners
  ("GO!", "GAME SET", "TIME") are the widest. Their exact widths were not measured; phase W1 Check F2 logs them.

The threshold should have a comfortable gap: nothing in a match is expected between 55 and 299 pixels wide except
HUD sprites, and those well under 300. If the log in W1 finds a rectangle near the threshold, or a future stage or
mode breaks the rule, the fallback is an explicit hint (section 2.8); it is not expected to be needed for v1.

### 2.4 Interpreter changes (`port/gfx/f3dex2.c`)

1. **Own the canvas.** Add `static int sW = 960, sH = 720; static float sK = 1;` and
   `void port_gfx_set_canvas(int w, int h)` which stores them and computes `k` per 2.1. Export it as
   `_PortSetCanvas` (add to `EXPORTS` in `Makefile.port:31`). `rb_webgl2.c:162` `port_set_canvas_size` goes away;
   `rb_webgl2.c:106` keeps reading the initial size only to seed the same value.

2. **Map viewport and scissor here, not in the backend.** `RbState.viewport` and `RbState.scissor`
   (`port/gfx/gfx.h:25-26`) change meaning to **canvas pixels, GL convention (y up)**. `fill_state`
   (`f3dex2.c:233-251`) computes them:
   - viewport: `x0 = wide ? stretch(vp.x) : centred(vp.x)`, `w = wide ? vp.w·W/320 : vp.w·H/240`,
     `y0 = (240 − vp.y − vp.h) · H/240`, `h = vp.h · H/240`.
   - scissor: same two formulas on `sc[0]..sc[2]`, y as the backend does today (`rb_webgl2.c:157`).
   - also record `st->kx = wide(viewport) ? k : 1` (new field).
   `rb_webgl2.c:136, 156-157` then use the rectangles as given, no `sx/sy`.

3. **3D geometry.** In `emit_tri` (`f3dex2.c:279-291`) set `o->x = v->x * st.kx`. `y, z, w` unchanged. The
   backface test at 269-276 uses `x/w` and is unaffected by a positive uniform scale; the fog term at 141-145 uses
   `z/w`, unaffected.

4. **Rectangles.** In `do_rect` (`f3dex2.c:295-328`): the forced 320×240 viewport at line 305 is wide, so the
   backend will stretch it; compensate per rectangle:
   ```
   rk  = wide(x0, x1) ? 1.0f : k;              after the "+1" adjustment at line 306
   fx0 = (x0 / 160 − 1) · rk;   fx1 = (x1 / 160 − 1) · rk;      (line 310)
   ```
   `fy0, fy1` unchanged. Texture coordinates unchanged (they depend on `x1 − x0` in N64 pixels, which is the
   same). A 300-wide rectangle therefore covers the canvas width; a HUD sprite lands in the band.

5. **Nothing else.** `do_vtx`, matrices, textures, combiner, depth: untouched. At `k = 1` every formula reduces to
   today's, so the 4:3 picture must be identical (phase W0 checks this).

### 2.5 Backend changes (`port/gfx/rb_webgl2.c`)

- Delete `sx, sy` (line 136). `glViewport(st->viewport[0..3])`, `glScissor(st->scissor[0..3])` as given.
- `rb_begin_frame` (118-126) already clears the whole canvas black: the border and any letterbox come out black.
- The vertex shader (19-21) is unchanged; `k` is already in the positions.

`port/gfx/rb_null.c` is unaffected: it ignores everything.

### 2.6 One render-only hook in the game: the magnifying glass

The decision "is this fighter off screen" is made in a display proc, `ftDisplayMainProcDisplay`
(`src/ft/ftdisplaymain.c:1069`, the test at `1129-1151`): project the fighter with `gGMCameraMatrix` (written in
the display pass, `gmcamera.c:985-1018`), test with `gmCameraCheckTargetInBounds` (`gmcamera.c:1587-1596`, half
the 300×220 picture), then set `fp->is_magnify_show`, `fp->magnify_pos`, `gIFCommonPlayerInterface.magnify_mode`
and call `ifCommonPlayerArrowsUpdateFlags`. PLAN 2.4 already records these as render-only, written in draw and read
only by draw; the bubble itself is drawn by `ifCommonPlayerMagnifyUpdateViewport` (`src/if/ifcommon.c:1649-1726`)
and `ifCommonPlayerMagnifyProcDisplay` (`ifcommon.c:1729-1760`).

Change (under `#ifdef PORT`, at `ftdisplaymain.c:1131`):

```c
#ifdef PORT
    if (port_gfx_in_wide_frame(cam_pos_x, cam_pos_y) == FALSE)
#else
    if (gmCameraCheckTargetInBounds(cam_pos_x, cam_pos_y) == FALSE)
#endif
```

with, in `port/gfx/f3dex2.c`:

```c
int port_gfx_in_wide_frame(float x, float y)
{
    float hw = (gGMCameraStruct.viewport_width / 2) / sK;   /* 150 / k: the band edge pushed to the canvas edge */
    float hh =  gGMCameraStruct.viewport_height / 2;
    return x >= -hw && x <= hw && y >= -hh && y <= hh;
}
```

Effect: the bubble and arrows appear only when the fighter is outside the *canvas*. When they do appear, the
bubble is placed by `ifCommonPlayerMagnifyGetPosition` (`ifcommon.c:1466-1540`) at the 4:3 band edge, inset from
the canvas edge by `(1/k − 1)·150` N64 px (50 at 16:9). The magnifier's viewport and scissor are narrow, so they
are placed in the band, in register with the ring, which is drawn under the camera's wide viewport with `k`
applied: both land at the same canvas x. Moving the bubble to the true edge is phase W5.

Not changed in v1, on purpose: the player tags (`ifCommonPlayerTagProcDisplay`, `ifcommon.c:1975-2000`) and item
arrows (`ifCommonItemArrowProcDisplay`, `ifcommon.c:2095-2116`)
use the same bounds test, but their sprites are software-clipped to 10..310 before the display list exists
(`lbcommon.c:2286-2300`, scissor from `lbcommon.c:2972-2995`), and `gSPTextureRectangle` cannot encode x < 0. So a
fighter in the side band is drawn without its tag. Phase W5 covers this if the owner wants it.

### 2.7 The page (`web/`)

- `web/index.html:9`: drop `aspect-ratio:4/3` and the fixed `height:100%`; the canvas is sized by script.
- `web/engine.js`: on load and in a `ResizeObserver` on `#stage`:
  ```
  aw, ah  = stage size in CSS px
  A       = clamp(aw/ah, 4/3, 21/9)            portrait or narrower than 4:3 → A = 4/3, letterboxed
  cssH    = min(ah, aw / A);  cssW = cssH · A
  dpr     = min(devicePixelRatio, cap so that backing height ≤ 1440)
  canvas.style.width/height = cssW/cssH;  canvas.width/height = round(cssW·dpr)/round(cssH·dpr)
  m._PortSetCanvas(canvas.width, canvas.height)
  ```
  Changing `canvas.width` clears the drawing buffer; the next tick redraws. `preserveDrawingBuffer` stays.
- `web/tests/shots.mjs` gains an `ASPECT` env (`4:3`, `16:9`, `21:9`) that picks the Playwright viewport
  (1000×760, 1280×720, 1680×720) so the stage box has that shape.

### 2.8 Fallback hint (only if 2.3 ever misfires)

`G_NOOP` (opcode 0x00) with a magic `w1` is ignored by the RSP and by `f3dex2.c:466`. A display proc under
`#ifdef PORT` could emit `gDPNoOpTag(dl++, PORT_TAG_WIDE_ON/OFF)` around its draws, and `run()` would set a flag
that overrides the width rule for the following rectangles and triangles. Display lists are outside the snapshot
(section 1.4), so this is render-only. Not needed for any content found in a VS match; written down so Opus has
the tool if a stage surprises us.

---

## 3. The determinism argument

Two clients, one at 4:3 and one at 21:9, must produce identical `port_hash_sim()` (`port/net/harness.c:143-196`)
every frame and must never diverge in anything that later reaches those fields.

**Claim: the simulation never sees `k`.**

1. **Where `k` lives.** `sK` is a static in `port/gfx/f3dex2.c`, set only by `_PortSetCanvas` from the page. It
   is read by `fill_state`, `emit_tri`, `do_rect` (all reached only from `port_gfx_run`, called only by
   `port_sched_drain` for a gfx task, `port/loop/sched.c:181`) and by `port_gfx_in_wide_frame`.
2. **When the renderer runs.** `port_gfx_run` runs only when a frame was stepped with `draw = 1`
   (`web/engine.js:85`, `web/lobby.js:114`). Re-simulation never draws. It reads display lists in the port's own
   `sGfxArena` (`sched.c:141`) and game memory; it writes only its own statics and GL state. It writes nothing in
   `gGameDataBegin..gGameBssEnd` or the arena (`snapshot.c:40-48`).
3. **The one game-side reader of `k`** is the hook in `ftdisplaymain.c:1131`, inside a display proc. The values
   it influences (`fp->is_magnify_show`, `fp->magnify_pos`, `gIFCommonPlayerInterface.magnify_mode`,
   `arrows_flags`, `sIFCommonPlayerMagnifyInterface[].pos/viewport`, `magnify_scale`) are already written only in
   display procs (`ftdisplaymain.c:1089-1151`; `ifcommon.c:1466-1760`; `gmcamera.c:1057-1058, 1369`) and read only
   in display procs. The existing proof "draw on and off identical" on every stage with 4 and 8 players
   (`scripts/proof_phase1.sh:34-38, 57`) shows that no display-written field reaches the hash: if one did, a
   no-draw run would differ from a draw run. Clients already differ in these fields today, because they draw on
   different ticks and never during rollback. Widescreen changes *what* draw writes there, not *whether* draw
   writes there.
4. **Every reader of the screen size in `src/`,** found by `grep` for `aspect`, `viewport_`, `gGMCameraMatrix`,
   `gGCMatrixPerspF`, `GS_SCREEN_WIDTH_DEFAULT`, `gSYVideoRes`, classified:

| Site | Reads | Sim or draw | Under this spec |
|---|---|---|---|
| `src/gm/gmcamera.c:478-497` `gmCameraGetClampDimensionsMax` | `viewport_width/height` ratio to pick the zoom that fits all fighters | **sim** (camera update) | unchanged: 300/220. The camera frames fighters for 4:3; wide shows more stage. Same on all clients. |
| `src/gm/gmcamera.c:1204-1243` `gmCameraPortUpdateSimMatrix` (PORT) | `cobj->projection.persp.aspect` | **sim** | unchanged: aspect stays 1.3636 (`gmcamera.c:1160`). |
| `src/wp/wplink/wplinkboomerang.c:104-121` | that matrix + `viewport_width/height` + 40 | **sim**: boomerang dies when off the 4:3 frame | unchanged; identical on all clients. Visible quirk in the side bands (section 0). |
| `src/gm/gmcamera.c:1587-1596` `gmCameraCheckTargetInBounds` | `viewport_width/height` | helper; callers: `ftdisplaymain.c:1131` (draw), `ifcommon.c:1995, 2113` (draw) | the first caller takes the hook; the others unchanged. |
| `src/gm/gmcamera.c:985-1018` `gmCameraLookAtFuncMatrix`, `objdisplay.c:2795-2830` | aspect → `gGMCameraMatrix`, `gGCMatrixPerspF` | draw | unchanged; `k` applied later in clip space. |
| `src/sys/objdisplay.c:795-930` billboards, `src/lb/lbparticle.c:1497-1625` particles | `gGCMatrixPerspF`, aspect | draw | unchanged; caught by clip-space `k`. |
| `src/ft/ftdisplaymain.c:1129-1151` magnifier decision | `gGMCameraMatrix`, bounds | draw | hook, section 2.6 |
| `src/if/ifcommon.c:1466-1760` magnifier placement and draw; `1593-1607` render clamps | `viewport_*` | draw | unchanged; narrow viewport/scissor placed in the band |
| `src/if/ifcommon.c:1975-2000` tags, `2095-2116` item arrows | `viewport_center_*`, bounds | draw (`SObj.pos` written in the display proc) | unchanged in v1 |
| `src/if/ifcommon.c:1002-1030, 70-71` damage positions (4-player 55..265; 8-player compact 29..291) | constants | **sim** (set at match start) | unchanged; centred in the band by the rectangle rule |
| `src/gr/grwallpaper.c:45-124` backdrop parallax | camera eye/at, constants 320/240 | **sim** (process func, every frame) | unchanged; strips are 300 wide → stretched by the rule |
| `src/sys/objdisplay.c:2570-2690, 3362-3395`, `src/lb/lbcommon.c:2972-2995`, `src/lb/lbfade.c:72`, `src/if/ifscreenflash.c:44`, `src/sys/rdp.c:27-35` | `gSYVideoRes*`, 320/240 constants | draw | unchanged; wide → stretched |
| `src/sys/debug.c:208-314`, `src/ovl8/ovl8_22.c:291-350`, `src/mv/*`, `src/sc/sc1p*`, `scexplain.c`, `scstaffroll.c`, `lbtransition.c:145` | aspects for menus, movies, 1P, debug | unreachable in the port (PLAN D3: menus unreachable in v1) | nothing |

   The camera's zoom math (first row) deserves a sentence: it uses the 4:3 ratio to decide how far out the camera
   must be for everyone to fit horizontally. In widescreen the camera is therefore never closer than it would be at
   4:3, and there is always more stage visible. Making it use the real aspect would be a sim change and would require
   every client to agree on one aspect (section 5, Q3). Not done.

5. **Game-visible pad input, RNG, timers, audio** are untouched by anything here.

**Headless proof** (no browser): `port/tools/run_match.c` gains `CANVAS=WxH` → `port_gfx_set_canvas(W, H)` before
`StartMatch`. With `draw = 1` and the null backend, the interpreter runs the full `k` path. The `[frame, hash]` log
must equal the no-draw log and the 4:3 log on every stage. This goes into `scripts/proof_phase1.sh` next to the
existing draw-on/off check.

---

## 4. Phases and acceptance checks

All checks run headless on this machine with the existing harness: `web/tests/lib.mjs` launches Chromium with
SwiftShader (`lib.mjs:15-17`), `web/tests/shots.mjs` takes stage screenshots from a seeded scripted match
(`?netN=1&stage=S&fighters=…&modes=hhhh`, deterministic frame by frame), `web/tests/lobby_proof.mjs` drives real
pages through the lobby and relay, `web/tests/net_client.mjs` is a node client with the null backend. Nothing in a
proof may touch ports 8080, 8774 or 8900; the proofs already pick their own ports.

"Band" below means the centre `H·4/3` columns of a wide screenshot. "Band equivalence" means: crop the wide shot to
the band, scale the 4:3 shot of the same seed and frame to the same size, mean absolute difference < 2/255 and
fewer than 0.5% of pixels differ by more than 16/255. SwiftShader is deterministic and the frames are identical
sims, so this is a strict test of the mapping: the band must contain the same picture.

### W0: plumbing, no visible change

- `port_gfx_set_canvas`, `_PortSetCanvas` export, viewport/scissor mapping moved into `f3dex2.c`, `rb_webgl2.c`
  simplified, page sizing in `engine.js`, `index.html:9` CSS.
- **Check A:** `shots.mjs` at the old viewport (1000×760, canvas lands at 4:3) for stages 0, 4, 6, frames 300 and
  900, before and after: PNGs identical. If not identical, each differing region is explained in `proof/changes/`.
- **Check B:** `node ssb64_run.js … 1 1` with `CANVAS=960x720` and with `CANVAS=1280x720`: hash logs identical to
  each other and to the no-draw log, all 9 stages, 4 scripted and 8-fighter runs (extend `proof_phase1.sh`).

### W1: the wide picture

- Clip-space `k` in `emit_tri`, rectangle rule in `do_rect`, wide/narrow classification.
- **Check C (band equivalence):** stages 0 (Peach's Castle), 1 (Sector Z, scrolling backdrop), 4 (Hyrule), 5
  (Yoshi's Island, static backdrop), 6 (Dream Land), frames 300, 900, 1800, `ASPECT=16:9` vs `4:3`. Also 21:9 for
  stages 0 and 6.
- **Check D (sides are picture, not black):** in each 16:9 shot, on rows at 30%, 50%, 70% of H, the pixels between
  the border (`x < 10·W/320`) and the band edge are less than 50% pure black. Every stage has a backdrop, so the
  side bands must show it.
- **Check E (border):** columns `x < 10·W/320 − 1` and `x > 310·W/320 + 1` are pure black on every shot.
- **Check F (eyes):** the shots go into `proof/screenshots/wide/`. A person looks at each stage's side bands for
  missing geometry or a visibly clamped backdrop and lists what they see in `proof/changes/widescreen.md`. This
  is the one check code cannot do.
- **Check F2 (rectangle widths):** with a debug histogram of texture- and fill-rectangle widths in `do_rect`
  (port static, exported like `port_gfx_histogram`, `f3dex2.c:52-53`), run a full match on every stage with 8
  fighters, items on. Assert: no texture rectangle has a width in `[200, 299]`; every rectangle `>= 300` wide is
  either a fill or one of the backdrop strips (equal to the wallpaper camera's picture width, 300). This pins the
  classification of 2.3 against the real content rather than against a reading of the code.

### W2: the magnifier

- Hook at `ftdisplaymain.c:1131` and `port_gfx_in_wide_frame`.
- Add a debug counter in `port/gfx/f3dex2.c` (port static, not game memory): `port_gfx_magnify_mask()` returns a
  bitmask of fighters for which the magnifier's narrow viewport was set this frame (a `G_MOVEMEM` viewport with
  `w < 300` while `cimg_main`). Export as `_PortMagnifyMask`.
- **Check G (headless):** `run_match` on Dream Land, 4 CPUs, 5400 frames, `draw = 1`, at `CANVAS=960x720` and
  `CANVAS=1280x720`, logging the mask every frame. At 16:9 the mask is a subset of the 4:3 mask on every frame, and
  strictly smaller on at least 1% of the frames where the 4:3 mask is non-empty. Hash logs identical (Check B
  again).
- **Check H (eyes):** a 16:9 screenshot at one such frame, saved beside the 4:3 one: a fighter in the side band
  with no bubble over it.

### W3: netplay at mixed aspect ratios

- New `web/tests/wide_proof.mjs`, built from `lobby_proof.mjs`: own relay on a random port, two Playwright
  contexts with viewports 1000×780 (4:3) and 1600×720 (16:9), joined through the lobby UI, plus two
  `net_client.mjs` node clients, 1800 frames, scripted pads with 40 ms lag each way so rollback happens.
- **Check I:** both pages report `canvas.width/height` with aspect 4:3 and 16:9 respectively; all four clients'
  `hashCsv` are equal for the common prefix; no `desync` event; at least one rollback happened; no page errors.
- **Check J:** screenshots of both pages at the end, in `proof/shots/`.

### W4: HUD with 8 players, and resizing

- **Check K:** `shots.mjs` with `MODES=hhhhhhhh`, 8 fighters, stage 6, frames 300 and 1500, at 16:9 and 21:9:
  band equivalence against the 4:3 shot (this covers the compact damage row at y=216, the stocks, the timer and the
  tags). All 8 readouts are inside the band by construction; the check is that they are present and identical.
- **Check L (resize mid-match):** in a running offline match, `page.setViewportSize` to 1280×720, then 1000×780,
  then 1680×720, 30 frames each; after each, `canvas.width/height` match the expected shape, a screenshot has a
  non-black picture, and the console has no WebGL errors.
- **Check M (portal):** load the page inside an iframe of 1600×720 in a local static server (as the portal does,
  without the portal); the canvas is 16:9. No portal code changes; this only proves the fixed-inset stage follows
  the frame.

### W5: optional polish, each needs the owner's go

1. **Bubble at the true edge.** Multiply `ifmag->pos.x` by `1/k` in `ifCommonPlayerMagnifyUpdateViewport`
   (draw-only field) and widen the scissor clamps at `ifcommon.c:1593-1607, 1706-1721` to the wide bounds. The
   ring follows because it uses the same `pos`. Needs texture-rectangle coordinates beyond 320, which the 12-bit
   field allows up to 1023; the left side needs a port-side offset for x < 0 (not representable). About 20 lines.
2. **Tags and item arrows in the side bands.** Same bounds change at `ifcommon.c:1995, 2113`, plus widening the
   interface camera's sprite scissor (`lbcommon.c:2972-2995`) under PORT, plus the x < 0 problem above.
3. **Backdrop without stretching.** The parallax backdrop is scaled 1.004..2.0 (`grwallpaper.c:81-92`). When
   `scale ≥ 1/k` the image already has enough width to fill a wide canvas unstretched. Under PORT, widen the
   wallpaper camera's sprite scissor and use the hint of 2.8 so the strips are drawn centred rather than stretched.
   When the scale is too small, fall back to stretching. Sector Z's scrolling backdrop and Yoshi's static one need
   their own look.
4. **HUD anchored to the canvas edges**, mk64 style (`SpaghettiKart/src/port/Engine.cpp:782-799`, 44 call sites
   in SpaghettiKart). The damage readouts' positions are set at match start into `SObj.pos` (sim memory,
   `ifcommon.c:1055`), so anchoring must be a render-side translation keyed by a hint (2.8), not a data change.
   Recommend not doing this: the centred HUD is what mk64 shows for its unanchored elements and it keeps every
   readout at a fixed distance from the centre of the action.
5. **Crop the 10-px overscan border** so the picture reaches the canvas edge, as a TV did. Pure render:
   `stretch`/`centred` would map 10..310 to 0..W. Owner's taste.

---

## 5. Risks and open questions for the owner

**Risks**

- **R1. Stages were drawn for 4:3.** Side bands may show model edges, missing walls, or the backdrop's clamp on
  some stages. Only phase W1's screenshots tell. If a stage looks bad, the options are: accept it, fall back to a
  per-stage narrower `k` (render-only, trivial), or the hint of 2.8 to hide a specific draw.
- **R2. The backdrop is stretched** by `1/k` (1.33× at 16:9, 1.75× at 21:9). On sky and space it is invisible;
  on Yoshi's Island's painted backdrop it may be noticeable. W5.3 removes it when the owner cares.
- **R3. The magnifier sits inset** from the canvas edge when it appears (W2), until W5.1.
- **R4. Fighters in the side bands have no tag** until W5.2. With 8 players this is where people will look for
  their name.
- **R5. The boomerang vanishes at the 4:3 edge plus 40 px.** Sim behaviour; identical on all clients; left alone.
- **R6. Pixel cost.** A 16:9 canvas at 1080p is 2.07 Mpx against 0.69 Mpx for today's 960×720. Draw is already
  the heavy half (`proof/budget.log`: 2.86 ms of draw per frame at 8 fighters, software). The `dpr` cap in 2.7
  (backing height ≤ 1440) bounds it; W4 Check L should also log frame time at 1680×720.
- **R7. `k` is per client.** A spectator or a friend on a 4:3 monitor sees less stage than one at 21:9. This is
  the nature of the feature, same as mk64.
- **R8. The portal's `embed.patch`** touches `web/index.html` and `web/engine.js`. The edits here are in different
  lines, but the patch must be re-applied and re-tested in `/home/desktop/n64` after this lands.
- **R9. Resizing clears the drawing buffer** for one tick. Harmless; noted so nobody chases it.

**Open questions**

- **Q1. Which aspect range?** Spec assumes 4:3 to 21:9 inclusive, letterbox beyond. Confirm 21:9 as the ceiling
  (ultrawide monitors are 21:9; 32:9 exists). The clamp is one constant.
- **Q2. Border.** Keep the N64's 10-px black frame (today's behaviour, kept in this spec) or crop it (W5.5)?
- **Q3. Should the camera use the wide frame?** It would zoom in closer when fighters spread out, because it
  would know it has more width. That is a sim change: the aspect would have to become a match setting chosen by
  the host and sent to everyone, and a 4:3 player would then see a cropped picture. Recommendation: no. The
  render-only design is the safe one and is what the owner sees on mk64.
- **Q4. W5 items.** Which of the five, if any, and in what order? Recommendation: W5.1 (bubble at the true edge)
  and W5.2 (tags in the side bands) together, because with 8 players they matter; skip W5.4.
- **Q5. Per-stage override.** If W1's eyes find one bad stage, is a narrower `k` for that stage acceptable, or must
  every stage be fully wide?

---

## 6. Reference: how mk64 does it, and what carries over

- libultraship keeps the game at its native 320×240 coordinates. For rectangles it applies
  `AdjXForAspectRatio(x) = x · (4/3) / aspect` on the NDC x (`interpreter.cpp:1529-1537`, used at `2809-2810`):
  that is `centred()` here. Viewports and scissors are scaled by `RATIO_X = width/320` (`interpreter.cpp:2276-2279,
  2300-2312`): that is `stretch()` here. 3D is widened because the game passes the real aspect to `guPerspective`
  (`GameCamera.cpp:91`, `gScreenAspect` from `skybox_and_splitscreen.c:488-497`). SSB64 cannot do that without
  touching the sim, hence clip-space `k` (2.2), which is the same transform.
- SpaghettiKart's HUD anchoring (`OTRGetDimensionFromLeftEdge/RightEdge`, `Engine.cpp:786-799`, used at 44 sites)
  is per element, hand-placed by its authors. Nothing here depends on it; it is the model for W5.4 if ever wanted.
- SpaghettiKart's skybox is drawn wide ("Widescreen skybox", `src/engine/sky/Sky.cpp:185-203`) and its menu
  fills use `gDPFillWideRectangle` (`menu_items.c:3465-3470`). The rectangle rule of 2.3 gives the same results
  without new opcodes, because every full-width thing in SSB64 is exactly 300 wide.

## 7. Noticed on the way, not part of this spec

- `port/gfx/f3dex2.c:365-374` handles `G_MOVEWORD` indices 2, 6, 8, 10 only. The game writes projection matrix
  elements directly with `G_MW_MATRIX` (index 0) for some billboard kinds (`src/sys/objdisplay.c:760-790`). If
  those draws look wrong today, this is why; it is independent of widescreen, and the clip-space `k` would cover
  them once implemented.
- `src/gr/grwallpaper.c:25-35` `dGRWallpaperDisplayList` fills black over the picture; its user was not traced.
  It is 300 wide, so the rule handles it either way.
