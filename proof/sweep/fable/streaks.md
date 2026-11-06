# Rainbow streaks on Dream Land's flower beds

Second opinion, 5 Nov 2026. Verdict: a port bug, fixed in `port/gfx/f3dex2.c`. The original game does not
draw the streaks.

## Cause

The interpreter ignored `G_MODIFYVTX` (`gSPModifyVertex`): `case OP_CULLDL: case OP_MODIFYVTX: break;`.

Dream Land's flower strips (`src/relocData/152_StagePupupuFile3.c`, joint trees `DObjDesc_0x2A80` and
`DObjDesc_0x31F8`) are built from two joints that share RSP vertex slots:

- The lower joint's list `DL_0x2880` turns texturing off (`gsSPTexture(0,0,0,…,G_OFF)`), loads three vertices
  with s = t = 0 into slots 0–2 (`Vtx_0x2790`: (495,15,0), (-495,15,0), (-495,14,0)) and draws a 1-unit-tall
  degenerate triangle. Those slots are transformed by the lower joint's matrix, which does not sway.
- The upper joint's list `DL_0x28B0` loads the texture, then
  `gsSPModifyVertex(0, G_MWO_POINT_ST, 0x00000000)`, `gsSPModifyVertex(1, G_MWO_POINT_ST, 0x0FFF0000)`,
  loads only the two top vertices into slots 2–3 (`Vtx_0x27C0`, tc (4096,1024) and (0,1024)) and draws
  `gsSP2Triangles(0,3,1, 3,2,1)`. The animation (`AnimJoint_0x2BFC` and siblings, `AOBJ_FLAG_ROTX`) rotates
  the upper joint, so the tops of the flowers sway while the bases stay put.

`G_MWO_POINT_ST` writes the vertex's s/t in s10.5 texels, exactly what the RDP receives (the older GBI spells
it out: `gsMoveWd(G_MW_POINTS, vtx*40 + where, val)`, a raw store into the RSP's 40-byte post-transform
vertex; `include/PR/gbi.h:2337-2340`). Slot 1 should get s = 0x0FFF = 127.97 texels, matching the top
vertex's 4096/32 = 128. Without the patch both bottom corners keep s = 0, so the triangle (0,3,1) has s = 0
at all three corners and column 0 of the 64x32 flower texture is stretched across it, one texture row per
screen row: the rainbow streak. The same happens in `DL_0x29A8` (s = 0x17FF = 192 texels) and the three
front-row strips.

## Evidence

- Trace before the fix (`GfxTrace`, Dream Land, sim frame 516; raw vertex data added to the trace as `GTV`
  lines): `GTV 495,15,0/0,0@0.000 495,65,0/0,1024@1.000 -495,15,0/0,0@0.000` followed by
  `GTT … line=4 masks=6 maskt=5 lrs=508 … uv=0.0..0.0,0.0..32.0` and `GT tri cc=127e24:fffff3f9 … ab=1 …
  box=-1.24,-0.22,-0.58,-0.02` (N64 rows 122–144, the flower row). Two corners were loaded with texture
  scale 0 by `DL_0x2880`, one with scale 1 by `DL_0x28B0`: slots shared across lists, as the data says.
- After the fix the same triangles read `uv=0.0..128.0`, `0.0..192.0`, `0.0..256.0` and no triangle in the
  frame has a collapsed s range on a wide tile (before: 6).
- `st6_flowers_before_after.png`: the flower row at frame 516 before and after.
- `all_stages_516.png`, `all_stages_900.png`, `before_st*_f0_*.png`, `after_st*_f0_*.png`: all nine stages
  with Mario, before and after. Dream Land is the only stage whose rendering changes; stages 4, 5, 7, 8 differ
  by under 250 pixels (the frame counter), stages 0–3 differ only by animated backgrounds and camera because
  the probe's screenshot lands a sim frame apart (Sector Z's Arwing and blast, Jungle's scrolling, Zebes'
  lava). No other stage shows streaks before or after.

The two 16x16 cases the handoff also flagged (`uv=0.0..0.0,0.0..16.0`, boxes top-left) are not this bug: they
are the end faces of the floating platforms (`104_StagePupupuFile2.c`, vertices at x = ±952 with s = 0 by
design, column 0 of the platform texture is the edge colour) and the real RDP draws them the same way. Likewise
`uv=0.0..133.3` on the platform tops is real data (`tc = {4266, …}` on a clamped 16-wide tile: an end cap plus
a clamped flat middle), which `GL_CLAMP_TO_EDGE` reproduces.

## Fix

`port/gfx/f3dex2.c`, `OP_MODIFYVTX`: apply `G_MWO_POINT_ST` (s/t, s10.5, no texture scale) and
`G_MWO_POINT_RGBA` to the addressed slot. Screen-space variants are left alone; the game never uses them.
The data corpus has 210 `gsSPModifyVertex` commands, all `G_MWO_POINT_ST`: Dream Land (5 lists), Mushroom
Kingdom, Final Destination, item objects, and the Pikachu, Samus, Kirby, Link, Mario and Jigglypuff models,
so the fix also affects those (Mario's and Mushroom Kingdom's set shared slots to s = t = 0 for untextured
draws, a no-op either way; Pikachu, Samus and Kirby patch real coordinates).

Also added, render-only: `GTV` lines in the trace (raw object coords, texcoords and the texture scale each
vertex was loaded under, before every traced triangle) and a `GTOP` line for the first unknown opcodes during
trace or coverage runs, with the four commands before them.

## The stray opcodes 0x10, 0x1E, 0x7E (281 each in the coverage run)

Not related to the streaks, but found with the same trace. They are a display list laid out as vertices.
`src/relocData/297_MarioSpecial3.c` declares `Vtx dMarioSpecial3_JointVerts_Vtx[7]` at 0x168 and
`Gfx dMarioSpecial3_SuperJumpPunchDL_DisplayList[23]` at 0x1D8; `321_SamusSpecial3.c` does the same with
`dSamusSpecial3_JointVerts_Vtx[7]` at 0x230 and `BombDL` at 0x2A0. Only the first four "vertices" are
vertices (a 300x300 or 30x30 quad). The last three rows decode as
`E7000000 00000000, E3001001 00008000, E2001E01 00000001, E200001C 00553048, FC127E24 FFFFF3F9, F9000000 …`:
pipe sync, `gsDPSetTextureLUT(G_TT_RGBA16)`, `gsDPSetAlphaCompare(G_AC_THRESHOLD)`,
`gsDPSetRenderMode(G_RM_AA_ZB_TEX_EDGE, G_RM_AA_ZB_TEX_EDGE2)`, `gsDPSetCombineMode(G_CC_MODULATEIDECALA,
G_CC_MODULATEIDECALA)`, `gsDPSetBlendColor`: the real head of the list, which starts at 0x1A8, not 0x1D8
(the declared list begins with a `gsDPTileSync`; only 3 of the corpus's 2087 lists start that way, and these
two are among them). packdata converts
the block by its declared type, so those 48 bytes get the `Vtx` byte order (s16 fields swapped, the `u8`
colour word intact) and the interpreter sees `0000E700, 1001E300, 1E01E200, 001CE200, 7E24FC12, 0000F900`:
three NOOPs plus opcodes 0x10, 0x1E and 0x7E, exactly the 3:1:1:1 ratio in `coverage.txt`. The list runs
once per Super Jump Punch coin or Samus bomb, which is why every stage shows it. In the port those sprites
are drawn with whatever LUT mode, alpha compare, render mode, combiner and blend colour the previous list
left behind.

Proof: the opcode hunt (`GTOP` lines, stage 0, 1500 frames with the coverage recipe) printed
`op=10 w0=1001e300 w1=00800000 depth=1 prev=… 02000000:00ffffff 0000e700:00000000`, where the words before
it are the correct little-endian vertex (0,150,-150) tc (0,512) of the same block.

Fix (not made here, the files are outside this task's scope): in `297_MarioSpecial3.c` and
`321_SamusSpecial3.c`, shrink `JointVerts_Vtx` to 4 and give the 6 commands at 0x1A8 (0x270 for Samus) a
`Gfx` declaration, keeping the same symbol order so the relocation chain is unchanged; `make extract` then
regenerates the includes and packdata converts them as words. `tools/port/src/bin/packdata.rs` could also
flag the case (a `Vtx` block whose rows decode to RDP opcodes), but the typing is the real error.
Link's boomerang has the same mis-split: `Vtx dLinkSpecial3_Vtx_0x02A0_Vtx[14]` (in the LinkSpecial3 source)
ends with the same three bogus rows ahead of `BoomerangDL`, the third of the three `gsDPTileSync`-headed
lists. `328_KirbyModel.c` has a smaller instance of the same thing: `u16 dKirbyModel_gap_0x31CC_sub_0x6CFC[20]`
("palette") is the 5-command tail of `gap_0x31CC_sub_0x6AEC` (load sync, `gsDPLoadTLUTCmd`, pipe sync,
`gsSPTexture`, `gsDPSetTileSize`); as halfwords those become five NOOPs, so that Kirby part is drawn without
its palette load and texture scale.

## Not verified

- No emulator or console on this machine, so "the original draws it right" rests on the data and the GBI,
  not on a reference frame.
- Fighters other than Mario were not screenshotted after the fix; Pikachu's, Samus's and Kirby's
  `G_MODIFYVTX` lists now take effect and were not inspected by eye.
- The last full build attempted here failed in `src/ft/ftdisplaymain.c` (another session's edit in progress);
  `port/gfx/f3dex2.c` compiles on its own and the build that produced the "after" shots (20:57) carries the
  fix and the sync-filtered `GTOP` line, but not the final trace-gating of `GTOP`.

## Files

- Changed: `port/gfx/f3dex2.c` (OP_MODIFYVTX; trace additions `GTV`, `GTOP`; `Vert` keeps raw source data
  for the trace).
- This folder: `before_*.png`, `after_*.png` (nine stages, Mario, sim frames 516 and 900),
  `all_stages_516.png`, `all_stages_900.png`, `st6_flowers_before_after.png`, this note.
