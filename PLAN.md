# PLAN: Super Smash Bros. 64 online, in the browser

Written 4 Nov 2026 from a read of `ssb-decomp-re` (upstream commit 572a747, 20 Oct 2026) against
`/home/desktop/Documents/netplay-port-playbook.md`. This is the end-state technical spec and the phased to-do
list. It is written for an implementer that will break each phase into smaller sections. Short sentences on
purpose. Every claim about the code has a `file:line` next to it. Lines are from the upstream tree as cloned.

Ethos, from the playbook: a 1999 game raced on 1999 internet. A lightweight, simple, elegant solution. Fewer moving
parts beats cleverness. Reuse what exists. Fun with friends, nothing extravagant.

---

## 0. The shape of the whole thing

The decomp is a **matching N64 build** (IDO 7.1, byte-identical ROM). It has no PC or browser layer. So this project
has one layer the playbook's first game already had, and must build it first:

```
┌──────────────────────────────────────────────────────────────────────┐
│ web/        lobby page, netcode JS, WebGL2 canvas, AudioWorklet      │   layer 4: page
├──────────────────────────────────────────────────────────────────────┤
│ relay/      game-blind Rust relay: rooms, go, clock, frame bundles   │   layer 3: relay
├──────────────────────────────────────────────────────────────────────┤
│ port/net/   harness: StartMatch, Park, pad ring, NetState, hash,     │   layer 2: hooks
│             event ring, RNG trace, snapshot ring, RESIM gates        │
├──────────────────────────────────────────────────────────────────────┤
│ port/       platform layer: libultra shim, stepped main loop,        │   layer 1: port
│             data image, F3DEX2 → WebGL2, n_aspMain → PCM            │
├──────────────────────────────────────────────────────────────────────┤
│ src/        the game, unchanged except `#ifdef PORT` hooks           │   layer 0: decomp
└──────────────────────────────────────────────────────────────────────┘
```

Built in this order, each proven before the next: port (headless first), determinism proof, render and audio,
harness, relay and lobby, lockstep over relay bundles, snapshot budget, rollback with the relay as clock, deploy.

Everything stands on one fact: **the simulation is deterministic.** Same wasm, same pads, same seed, same frames on
every machine. Only pads cross the wire. No game state is ever serialized to the network.

**What is simpler than the first game.** SSB64 supports 4 humans natively (`GMCOMMON_PLAYERS_MAX 4`,
`src/gm/gmdef.h:5`; VS allocates 4 slots, `src/sc/sccommon/scvsbattle.c:162`). Every player sees one shared
camera. So playbook sections 6 (more players than native) and 7 (per-view render and audio) do not apply. There is
no "virtual N", no watch slot, no audio masking. The match is 60 Hz, so every number in playbook section 12 is
re-derived in section 3.12 below.

**What is harder than the first game.** There is no port. Building it is Phase 1 and Phase 2 and is the bulk of
the work. The design choices below keep that port as thin as the game allows.

---

## 1. Scope

In scope, v1:
- Desktop browser, landscape, keyboard or gamepad. One page per player.
- VS mode (`nSCKindVSBattle`, 22 US) with the full vanilla rule set: time or stock, items, team battle, damage
  ratio, handicap, CPU slots. Settings pinned by the lobby, applied by `StartMatch`.
- Up to 8 humans, past the cartridge's 4 (owner's priority, 4 Nov 2026). CPU fills any empty slot the lobby asks for. CPUs are simulated by every page from the seed.
- Relay-only transport. No P2P. Rollback with the relay as clock, after lockstep proves the stack.
- US ROM only. JP is out until US is done.

Out of scope, v1: in-game menus over the network, 1P modes, training, replays as a product, mobile, voice,
spectator grid, per-view anything. Couch two-players-on-one-page is cheap and listed as an extra at the end.

---

## 2. Findings table

Facts that decided the design. Lines refer to upstream `ssb-decomp-re`.

### 2.1 Frame loop and threads

| Fact | Where | Why it matters |
|---|---|---|
| One iteration = `task_update` (pad latch + `scene_update`) then optional `task_draw`. Update is `syTaskmanCommonTaskUpdate`, draw is `syTaskmanCommonTaskDraw`. | `src/sys/taskman.c:1015-1053, 1093-1120` | Natural tick boundary. Draw is already a separate call. |
| The frame wait is `osRecvMesg(&sSYTaskmanGameTicMesgQueue, BLOCK)`, fed by the scheduler every VI retrace. Extra tics are drained, not caught up. | `taskman.c:1020-1027, 1371`; `src/sys/scheduler.c:1038-1050, 1249` | The port replaces this wait with a host-driven step. |
| Draw is skipped when no gfx context is free. Update still runs. | `taskman.c:842-871, 1039` | The game already tolerates draw skips. |
| Update and frame intervals are fixed at 1. `syTaskmanSetIntervals` has no callers. | `taskman.c:1286, 1374` | One sim step per frame, fixed. |
| NTSC 60 Hz only. PAL hangs. `UPDATE_INTERVAL 60`. | `scheduler.c:1221-1249`; `include/macros.h:79` | Sim tick = 60 Hz. |
| Threads: game 5 (pri 50), scheduler 3 (120), audio 4 (110), controller 6 (115), idle 1, VI/PI managers, hang detector 8. Gameplay runs only on thread 5. | `src/sys/main.c:41-44, 180-235`; `src/sys/debug.c:906-984` | The port runs everything on one thread, stepped. |
| GObj "thread" processes are real OSThreads (pri 51) with their own stacks. Thread 5 starts one and blocks on `gGCMesgQueue`; it yields with `gcSleepCurrentGObjThread`. | `src/sys/objman.c:804-883, 2159-2163`; `src/sys/objhelper.c:133-145` | A wasm sim cannot hold suspended native stacks in a snapshot. These are rewritten (2.9). |
| In a VS match exactly five GObj threads exist, all HUD timing: announce GO, countdown, entry focus, entry all, sudden death. | `src/if/ifcommon.c:1955-2362` | Five small state machines replace them. |
| The scene manager is a `while(TRUE) switch(scene_curr)` that loads overlays and calls a `*StartScene`, which runs the frame loop and has code after it (BGM busy-wait, sudden death, results). | `src/sc/scmanager.c:816-1282`; `scvsbattle.c:513-561` | The port needs a stepped scene driver (3.4). |
| Scene switch = write `gSCManagerSceneData.scene_curr`, call `syTaskmanSetLoadScene()`. | `taskman.c:892-895, 905`; `scmanager.c:866` | The only API needed to enter or leave a scene. |
| Overlays are loaded by DMA to fixed VRAM, BSS zeroed, `.data` re-copied from ROM on every load. No relocation. 66 overlays plus scmanager. | `src/sys/dma.c:94-113`; `scmanager.c:63-131`; `scdef.h:103-115` | A static link must reproduce the fresh-scene semantics (3.3.5). |
| Overlay `.data` is mutated at runtime (`dSCVSBattleTaskmanSetup.arena_size`, `func_start`). | `scvsbattle.c:526-546` | Reset must restore `.data`, not only zero BSS. |

### 2.2 Inputs

| Fact | Where | Why it matters |
|---|---|---|
| Hardware pads land in `OSContPad sSYControllerData[4]`, read only inside controller.c. | `src/sys/controller.c:33, 113-164` | No other code touches hardware. |
| Game-visible pads are `SYController gSYControllerDevices[4]` (`button_hold`, `button_tap`, `button_update`, `button_release`, `stick_range`), plus mirror `gSYControllerMain`. | `src/sys/controller.h:19-26, 119-120`; `controller.c:45`; `src/sys/maindevice.c:4` | Injection target. |
| One latch per update: `sSYTaskmanFuncController()` (= `syControllerFuncRead` for VS) runs before `scene_update`. It blocks until controller thread 6 copies descs to devices. | `taskman.c:1075-1076, 1095-1096, 1205`; `controller.c:252-263, 273, 368-378`; `scvsbattle.c:43` | Single injection point. |
| Tap and release are OR-accumulated across VI polls, and a repeat timer ticks per VI. | `controller.c:131-160` | Not deterministic under lag. The port computes them per sim frame from the injected pad. |
| Fighters read only `button_hold` and `stick_range` through `fp->input.controller = &gSYControllerDevices[player]`, and derive tap/release themselves. | `src/ft/ftmain.c:1246-1297`; `src/ft/ftmanager.c:820`; `scvsbattle.c:192-197` | Slot index = port = fighter player. |
| CPU and scripted fighters write `fp->input.cp`, not the pad globals. The CPU uses the shared RNG heavily (64 calls). | `src/ft/ftcomputer.c:3399-3511, 3685+`; `src/ft/ftkey.c:13` | CPUs need no fake pad; they must be simulated by every page. |
| START on any port pauses, including CPU and empty slots. A+B+R+Z while paused quits the match. | `ifcommon.c:2900-2970, 3039-3056` | Pause is an input. All 4 pads are always sent (zero for empty). |
| `gSYControllerMain` indexes `gSYControllerDeviceStatuses[0]`, which is -1 with no pads. | `controller.c:188-195` | Fake four connected pads at boot. |
| Rumble is output-only via a motor event queue on thread 6. | `src/gm/gmrumble.c`; `controller.c:312-350, 414-447` | Shim to the Gamepad vibration API or no-op. |
| Persistent input state inside fighters: `input.pl.*`, `stick_prev`, `tap_stick_*`, `hold_stick_*`, `tics_since_last_z`. | `src/ft/fttypes.h:835-842, 1133-1136`; `ftmain.c:1320-1380` | Inside `FTStruct`, so inside the snapshot. |

### 2.3 Randomness and time

| Fact | Where | Why it matters |
|---|---|---|
| One LCG (`seed*214013+2531011`), seed `sSYUtilsRandomSeed = 1`, through pointer `sSYUtilsRandomSeedPtr`. 296 gameplay call sites (ft 91, it 52, ef 50, gr 33, lb 26, sc 22, wp 3, if 3). | `src/sys/utils.c:14-15, 143-183` | One generator. Easy to trace. |
| The seed is never set. `syUtilsSetRandomSeed` and `syUtilsRandSeed` have zero callers. | `utils.c:143-150` | `StartMatch` seeds it. |
| The LCG step is a signed multiply. Signed overflow is UB in C. | `utils.c:165, 174` | Do the step in `u32` under PORT. |
| HAL's How-to-Play demo swaps in a fixed update seed and a separate draw seed. | `src/sc/sccommon/scexplain.c:46-49, 812-837` | Their own deterministic-replay pattern. Copy it. |
| Time-based randoms (`syUtilsRandTimeUCharRange`, `osGetTime`) are called only in menus and movies. Four picks set up a match (random fighter, random stage). | `utils.c:185-198`; `src/mn/mnplayers/mnplayersvs.c:3479, 4504`; `src/mn/mnmaps/mnmaps.c:1383` | Not in the sim. The lobby decides those. |
| Every other `osGetTime`/`osGetCount` outside libultra is profiling, debug meter or debug menu. | `taskman.c:987-1046`; `scheduler.c:182-1066`; `src/sys/audio.c:1016-1063`; `debug.c:526-536`; ovl8 | Wall clock is safe to stub. |
| **The match timer advances by VI retrace deltas** (`sySchedulerGetTicCount`), not by sim frames. | `ifcommon.c:2472-2531, 3173-3179`; `scheduler.c:1042` | Under PORT the tic count is the sim frame count. |
| Effects, particles and the HUD draw from the shared RNG at update time, gated on pool space. Spawn results feed fighter state. | `src/ef/efmanager.c:2128-2169`; `objman.c:402-413`; `src/lb/lbparticle.c:263, 2158, 641, 930+`; `ifcommon.c:710`; `src/ft/ftness/ftnessspecialhi.c:318` | Effects are sim. They run in every frame, including re-simulation. |
| No RNG call in any display function. | grep of `*Display*`/`*Draw*` in ef if gr ft it wp gm lb sys | Draw can be skipped freely. |
| Audio has a private LCG (`randFloat1/2`), output-only. | `src/libultra/n_audio/n_env.c:2380-2381, 4570-4580` | Nothing to sync. |

### 2.4 Draw code that feeds the sim

| Fact | Where | Why it matters |
|---|---|---|
| **`gGMCameraMatrix` is written in the display pass** (`gmCameraLookAtFuncMatrix` via `gcPrepCameraMatrix`) and **read by Link's boomerang sim** every 9 frames. | `src/gm/gmcamera.c:985-1018`; `src/sys/objdisplay.c:2910`; `src/wp/wplink/wplinkboomerang.c:104, 354` | Sim depends on whether a frame was drawn. Fixed by computing the matrix at the end of the camera update (2.9). |
| `fp->is_magnify_show`, `magnify_pos`, `gIFCommonPlayerInterface.magnify_mode/arrows_flags` are written in draw and read only by draw. | `src/ft/ftdisplaymain.c:1089-1151`; `gmcamera.c:1049-1050, 1245, 1365` | Render-only. Never hashed. |
| `FTParts` matrix caches (`vec_scale`, `mtx_translate`, flag `unk_dobjtrans_0x5`) are filled in display and the same fields are read by collision. Whether draw-filled values ever reach the sim is **unverified**. | `src/lb/lbcommon.c:1369-1456`; `src/gm/gmcollision.c:87, 332-488` | Phase 1 proof compares a no-draw run against a draw run on every stage. |

### 2.5 Starting a match without menus

| Fact | Where | Why it matters |
|---|---|---|
| VS reads `gSCManagerTransferBattleState` (menus write it) and the stage from `gSCManagerSceneData.gkind`. The battle struct's `gkind` is overwritten. | `scvsbattle.c:515-519`; `scmanager.c:24-45` | `StartMatch` writes exactly these. |
| `SCBattleState` and `SCPlayerData` field lists. Slot index = port. No port field. | `src/sc/sctypes.h:314-376` | Section 3.6 lists what `StartMatch` pins. |
| The retail CSS commit `mnPlayersVSSetSceneData` is the pattern to copy. The debug battle and the attract demo are weaker templates. | `mnplayersvs.c:4379-4440, 4488-4507`; `src/db/dbbattle.c:205-225`; `scautodemo.c:546-578` | |
| Setup order in `scVSBattleStartBattle`: files, anti-piracy check, camera, effects, `ftParamInitGame`, stage, items, ground, fighters, HUD, entry thread, BGM, timer, GO. | `scvsbattle.c:125-226` | The seed hook goes right before this function. |
| Match end: last team standing or time up. Status End → Set → `syTaskmanSetLoadScene`. Then optional sudden death, then `scene_curr = VSResults`, which writes SRAM. | `ifcommon.c:2683-2753, 3144-3153, 3315-3346`; `scvsbattle.c:538-560`; `src/mn/mnvsmode/mnvsresults.c:199-263` | The scene driver parks instead of showing results. |
| `handicap` indexes a table as `h-1`; 0 is out of bounds. | `src/ft/ftparam.c:1457` | Pin 1..9, default 9. |
| `NoController` is a static scene with no run function and no exit. | `src/mn/mncommon/mnnocontroller.c:120-136` | The parked state. |
| Title idle launches a demo chain after 650 tics. Every menu returns to Title after 5 minutes. | `src/mn/mncommon/mntitle.c:712-724`; `mnplayersvs.c:4471-4484` | Never park on Title or a menu. |

### 2.6 Gameplay-affecting settings and anti-tamper

| Fact | Where | Why it matters |
|---|---|---|
| Battle fields that alter physics or AI: `damage_ratio`, per-player `handicap`, `is_team_attack`+`is_team_battle`, `item_toggles`, `item_appearance_rate`, `level`. | `ftparam.c:1457-1463`; `ftmain.c:3029`; `src/it/itmanager.c:489-680` | All pinned by the lobby. |
| Save data read by the sim: `error_flags` (RANDOMKNOCKBACK makes knockback `rand*200`, HALFSTICKRANGE halves sticks, VSBATTLECASTLE forces a stage), `unlock_mask` NEWCOMERS gates the Mew roll, `is_allow_screenflash`. | `src/lb/lbdef.h:168-171`; `ftparam.c:1469, 1496`; `ftmain.c:1315`; `scvsbattle.c:519`; `src/it/itmain.c:650-657` | `StartMatch` installs one canonical `LBBackupData` on every page. |
| Anti-piracy code is executed from asset files (`llSYKseg1ValidateFileID`, `llSYSignValidateFileID`) and from SP IMEM/DMEM probes (`gSYMainImemOK/DmemOK`). Failing them poisons the save. | `scvsbattle.c:139-151`; `src/sc/sc1pmode/sc1pgame.c:2043-2056`; `main.c:134-150`; `src/mn/mnvsmode/mnvsmode.c:1622`; `src/mn/mn1pmode/mn1pmode.c:803` | Stubbed to pass under PORT. |
| Saves are SRAM over PI, written only in menus, results and bonus-stage end. | `dma.c:126-157`; `src/lb/lbbackup.c:36-63` | No save I/O inside a match. The page never writes a save in v1. |

### 2.7 Memory and allocation

| Fact | Where | Why it matters |
|---|---|---|
| The only allocator is a bump arena with no free (`SYMallocRegion`). `syTaskmanMalloc` is the only "malloc". The general heap is re-initialised per scene. | `src/sys/malloc.c:7-41`; `taskman.c:258-264, 1231` | Snapshot = arena bytes up to `ptr`, plus `ptr`. |
| VS arena runs from `&ovl4_BSS_END` to `gSYFramebufferSets`, about 2 MB (exact size needs the map). | `scvsbattle.c:31, 526`; `src/sys/framebuffer.c:4` | The port gives it a fixed buffer. |
| Object pools (GObj, DObj, AObj, MObj, XObj, SObj, CObj, process, thread, stack) are typed free lists that **grow lazily from the arena when empty**. VS sets every pool count to 0. | `objman.c:119-753, 2242-2441`; `scvsbattle.c:46-63` | The arena pointer can move mid-match. Fixed by preallocating (2.9). |
| Game pools are fixed at scene start: fighters 4, parts 4×37, items 16, weapons 32, effects 38, particles 112/24/80. | `ftmanager.c:144-154`; `itmanager.c:138`; `src/wp/wpmanager.c:35`; `efmanager.c:1740`; `src/ef/efparticle.c:30-33` | Bounded. |
| Every fighter status change DMAs and relocates an animation file into `fp->figatree_heap`. | `ftmain.c:4621-4624`; `ftmanager.c:364-369` | A mid-match "ROM read" into the arena. The port memcpys from the data image. |
| Graphics heaps and DL buffers are carved from the general heap and reset every draw. | `taskman.c:270-278, 1082, 1107, 1176-1196` | Moved to a port-owned region so snapshots skip them. |
| 4 MB RDRAM, no Expansion Pak. Framebuffers `u16 [3][230][320]` at a fixed address; zbuffer at a fixed address. | `src/sys/video.h:29-36`; `framebuffer.c:3-4`; `src/sys/zbuffer.c:4`; `scmanager.c:849-856` | The HLE renderer ignores framebuffer memory. The port declares the arrays and never reads them. |
| Audio heap `gSYAudioHeapBuffer[0x56000]` (US). | `audio.c:141` | Presentation. Never snapshotted. |

### 2.8 Data, loading, endianness

| Fact | Where | Why it matters |
|---|---|---|
| 2132 asset files live in the relocData ROM segment, addressed by file id through a 12-byte table with vpk0 compression and intern/extern pointer chains. The loader is `lbRelocLoadAndRelocFile`. 84 loader call sites outside `src/lb`. | `src/lb/lbreloc.c:124-273, 302-389`; `src/lb/lbtypes.h:15-47`; `dma.c:160-416` | |
| **All 2132 files are committed as typed C source** (`Vtx`, `Gfx`, `FTAttributes`, `DObjDesc`, `AObjEvent32` scripts, …) and build byte-identical. Chain pointers are typed pointer expressions. | `relocData.md` (status table); `src/relocData/*.c` | The port compiles the data natively. No vpk0, no chains, no byteswap of assets. Bitfield order is a non-issue because the same compiler lays out writer and reader. |
| Game code reaches into files with `lbRelocGetFileData(type, file, &llSym)`, where `llSym` is a linker constant offset. About 3900 `ll*` symbols, 888 use sites, generated by `tools/genRelocSymbols.py` from the typed sources. | `src/lb/library.h:7-8`; `tools/genRelocSymbols.py` | Under PORT the same generator emits `llSym` as the native address delta of the named block, and `llXxxFileID` as a file index. |
| Particle banks are compiled C too (`src/particles`, 9.4k lines) but the runtime does raw offset-to-pointer fixups. | `efparticle.c:102-108`; `lbparticle.c:182-231` | Compiled natively with real pointers under PORT. |
| Audio banks, sequences and FGM data are raw big-endian ROM blobs, patched in place by `audio.c`. ADPCM books and samples are big-endian. | `audio.c:312-420, 784-867`; `src/audio/*/README.md` | The only raw data left. Byteswapped once at load. |
| The particle VM reads floats big-endian ("Big Endian only!"). | `lbparticle.c:584-596` | Native read under PORT. |
| Mtx fixed-point packing in `matrix.c` must agree with the renderer. | `src/sys/matrix.c:5-41` | The HLE renderer decodes `Mtx` the F3DEX2 way. |
| About 85 distinct libultra functions and 4 data symbols are used outside `src/libultra` (threads/messages 12, timers 3, VI 8, SP/DP 4, PI 4, SI 5, motor 3, AI 2, n_audio 13, gu 8, cache 5, sprite 5, libc 3). No EEPROM, no PFS. | memory survey, section 5 | Sizes the shim at roughly 1,500 lines. |
| `n_audio` synth and SSB's FGM player are pure C. Only `n_aspMain` (audio RSP) and `gspF3DEX2_fifo` (gfx RSP) are binary microcode. | `n_env.c:2114-5458`; `taskman.c:59-72, 459-540`; `audio.c:1040-1080` | Two HLE interpreters: one for F3DEX2 display lists, one for the n_audio command list. |
| Sim trig uses libultra's polynomial `__sinf`/`__cosf`. | `src/libultra/gu/sinf.c, cosf.c` | Compile them. Never libm. No fast-math. |
| UB and garbage-argument sites exist in gameplay (Samus/Kirby escape `itemthrow_buffer_tics`, Link bomb `stat_flags`, Link Up-B wrong gobj, Mario fireball id). `AVOID_UB` is off by default. | `src/ft/ftsamus/ftsamusspecialn.c:170`; `src/it/itlinkbomb.c:11, 402`; `src/ft/ftlink/ftlinkspecialhi.c:67-100`; `Makefile:106-108` | PORT builds with `AVOID_UB` and pins explicit values (decision D11). |

### 2.9 Audio

| Fact | Where | Why it matters |
|---|---|---|
| Sound effects have no request queue. Gameplay calls `func_800269C0_275C0(id)`, which allocates a slot and links it into the live list at once. Both entry points pass through `func_80026844_27444`. 24 slots. NULL when full. | `n_env.c:4987-5068, 5023`; `audio.c:115` | The RESIM gate for sounds is `func_80026844_27444`. |
| Gameplay stores sound handles (`p_sfx/sfx_id`, `p_voice`, `p_loop_sfx`) and polls `handle->sfx_id == stored` to decide stops. Handle serials are bumped on the audio thread too. | `fttypes.h:1258-1263`; `ftparam.c:296-336`; `ftmain.c:347-349, 2109`; `n_env.c:5056, 4762` | Audio-only decisions. Excluded from the hash. Handles replayed from a journal in RESIM. |
| Crowd chant and announcer queue wait on sound completion; audio-only effect. | `src/ft/ftpublic.c:340-395` | Excluded from the hash. |
| BGM is a one-slot mailbox drained by the audio thread. The sim starts item music (hammer, star) and the hurry music, and posts a volume event every frame in the last 5 s. | `audio.c:1099-1142, 1283-1303`; `ftparam.c:110-157`; `ifcommon.c:2506, 2525` | Gated in RESIM, reconciled after. |
| The audio thread reads no gameplay state. Pan is pushed from gameplay. | `src/lb/lbcommon.c:725-748` | Audio can run once per frame after the last sim step. |
| Scene transitions busy-wait on `syAudioCheckBGMPlaying`. | `scvsbattle.c:532, 551`; `scmanager.c:842-848` | The stepped scene driver turns these into states. |
| Audio tick is 184 samples (5.75 ms), about 2.9 per frame; 552 or 368 samples per frame at 32 kHz; 3 output buffers of 920 stereo frames. | `n_env.c:5340`; `audio.c:835-837, 914-916, 1022-1035` | The port runs `n_alAudioFrame` once per host frame and pushes PCM to an AudioWorklet ring. |

---

## 3. End-state architecture

### 3.1 Decisions that shape everything

D1. **Source port of the decomp to wasm32 with our own thin platform layer.** Not an emulator (re-simulating 12 frames of N64 emulation in one 16 ms frame is not possible in a browser), not static recompilation (no web renderer, no determinism control). One `.wasm` on every client gives identical IEEE results.

D2. **One thread, stepped from the page.** No pthreads, no SharedArrayBuffer, no Asyncify. `port_step_frame()` runs one update and optionally one draw, then returns. The N64's five threads collapse: the scheduler is gone (display list handed to the renderer directly), the controller thread is gone (pads come from the ring), the audio thread becomes a call after the last sim step of the tick. The two blocking shapes in the game (the frame wait and the GObj threads) are removed by D3 and D4.

D3. **A stepped scene driver replaces `scManagerRunLoop` under PORT.** It knows three scenes: boot, parked (`NoController`), VS battle (including sudden death). The `*StartScene` tails become states.

D4. **The five HUD GObj threads become tick-counted state machines.** Each is 10 to 50 lines in `ifcommon.c`. After this, no code suspends mid-function in a VS match, and the snapshot holds no stacks. An assert refuses `nGCProcessKindThread` during a netplay match.

D5. **Asset data is compiled from the typed C sources, not loaded from ROM.** The relocData segment, particle banks and the `ll*` offsets become ordinary linked data. vpk0, pointer chains and asset byteswaps disappear. "Loading" a file is a table lookup (and for the figatree heap, a memcpy that keeps the game's copy semantics). The data lives in a separate `.data` file served by the relay, checked by hash.

D6. **Fresh-match semantics by image restore, not by overlay reload.** After boot the harness takes one snapshot of all game globals (the "boot image", same table as the rollback snapshot). `StartMatch` restores it, then writes the match settings. Overlay BSS zeroing and `.data` re-copy become unnecessary. Race-after-race determinism follows by construction.

D7. **The relay implements the clock-and-bundle protocol from day one.** Lockstep v1 is "simulate frame f only when bundle f has arrived", with input delay D. Rollback v2 adds prediction and the window. Nothing built for lockstep is deleted later. This follows the playbook's own regret about lockstep machinery.

D8. **Rendering is an F3DEX2 display-list interpreter in C, drawing with WebGL2.** Port an existing C interpreter (sm64-port's `gfx_pc` family is the reference shape: ucode table, combiner shaders, texture cache) rather than a C++ framework. Target ~3,500 lines. Render once per host frame, newest sim frame only.

D9. **Audio is an n_aspMain command-list interpreter in C** (ADPCM decode, resample, envelope mix, interleave; the `n_abi.h` command set), feeding an AudioWorklet ring. Audio is presentation. It lands in Phase 2, after determinism is proven in Phase 1 with audio stubbed.

D10. **Hash and snapshot tables are generated from the linker map at build time**, as the playbook prescribes (`-Wl,--print-map` plus `llvm-nm` on bitcode for LTO). Game objects in; `port/`, renderer, audio and netplay bookkeeping out.

D11. **Build with `AVOID_UB`, and pin the garbage-argument sites to explicit values.** Deviation from N64 behaviour is accepted and logged (section 6).

D12. **Desktop, landscape, keyboard or gamepad.** Nothing else until the owner asks.

### 3.2 Repository and build

The decomp repo is the project. The matching IDO build must keep working untouched; every port change in `src/` is under `#ifdef PORT` and the N64 path stays byte-identical (`make` still produces the ROM). New code lives outside `src/`.

```
ssb-decomp-re/
  src/                 game; PORT guards only where this plan says
  port/                platform layer (C, wasm32 and native)
    shim/              libultra replacement: os*, gu glue, cont, motor, ai, vi, dma, sram
    loop/              port_main.c (stepped loop), scene_driver.c, pools.c
    data/              data image: file table, ll offset header, audio bank loader
    gfx/               F3DEX2 interpreter + WebGL2 backend (+ null backend)
    audio/             n_aspMain interpreter + PCM ring (+ null backend)
    net/               harness: netstate.c, padring.c, start_match.c, hash.c, events.c,
                       rngtrace.c, snapshot.c, resim.c, audio_journal.c
    gen/               build-generated: snapshot_table.inc, audio_table.inc, reloc_fileids.h
  web/                 index.html, lobby.js, net.js, gfx.js (canvas glue), audio-worklet.js, debug flags
  relay/               Rust: axum + tokio, one binary serving site, data and /api
  tools/port/          gen_snapshot_table.py, gen_port_reloc_symbols.py, build id stamp
  proof/changes/       one file per phase
  PLAN.md  CHANGES.md  DECISIONS live in PLAN.md section 6
```

Build:
- `make` is the IDO ROM build, unchanged.
- `make -f Makefile.port NATIVE=1` builds a Linux executable with the null gfx/audio backends plus an SDL2 window backend for local play and debugging (sanitizers, gdb). Same C, same `PORT` define.
- `make -f Makefile.port WASM=1` builds `ssb64.wasm` + `ssb64.data` with clang/emscripten, `-O2`, no fast-math, LTO optional. Stamps `BUILD_ID = sha256(wasm)[:16]` into `web/build.js`.
- Compile set under PORT: everything in `src/` except `src/libultra/{os,io,libc,sp,vimodes}` (sp is kept; it is C), `src/ovl8`, `src/db`, `src/entry.s`. `src/relocData/*.c` and `src/particles/*.c` are compiled as data objects. `src/audio/*` stays as raw banks extracted at build time.
- The 4 nonmatching stubs in `src/ovl8` are irrelevant (not compiled). Anything still `GLOBAL_ASM` in compiled files: none (grep confirmed).

Determinism rules for the build: `-ffp-contract=off`, no `-ffast-math`, no libm for `sinf/cosf/sqrtf` (use `src/libultra/gu`), `-fno-strict-aliasing` as the IDO code assumes.

### 3.3 Platform layer (`port/`)

#### 3.3.1 Stepped loop

```
port_boot()                      run syMainLoop's thread-5 body once: init shim, audio init,
                                 scene_driver_init(); fake 4 connected pads; take boot image
port_step_frame(draw: bool)      1. pads: harness writes gSYControllerDevices[0..3] (3.6)
                                 2. scene_driver_step(): one task_update (+task_draw if draw)
                                 3. if not RESIM: audio_frame(): n_alAudioFrame → PCM ring
port_present()                   renderer draws the last submitted display list to the canvas
```

`syTaskmanRunTask` (`taskman.c:946-1053`) is split under PORT into `syTaskmanStepTask()` which does one iteration without the `osRecvMesg` wait, and returns whether `syTaskmanCheckBreakLoop()` fired. `syTaskmanCommonTaskUpdate` keeps its shape: `sSYTaskmanFuncController()` then `scene_update`. Under PORT `syControllerFuncRead` does not post to a thread; it copies the harness pad for this frame into the device structs and derives tap/release against the previous frame (3.6).

Draw: `syTaskmanCommonTaskDraw` builds the display list as today. `func_800053CC` (`taskman.c:632-751`) hands the finished list to `port_gfx_submit(Gfx*)` instead of building an `OSTask`. Gfx contexts: the port always reports one free context; the game's own skip rule (`taskman.c:1039`) is replaced by the caller's `draw` flag.

#### 3.3.2 libultra shim

| Group | Behaviour under PORT |
|---|---|
| Threads, message queues (12) | `osCreateThread/StartThread/StopThread` for threads 1,3,4,6 become no-ops with a log; queues are plain ring buffers; `osRecvMesg(BLOCK)` on an empty queue is a fatal assert (the stepped design must never block). |
| Timers (3) | `osGetCount`/`osGetTime` return `frame*constant` (profiling only); `sySchedulerGetTicCount()` returns the sim frame counter (the match-timer fix). |
| VI (8) | Record mode and framebuffer pointer; nothing else. |
| SP/DP (4) | `osSpTaskStart` for gfx → `port_gfx_submit`; for audio → `port_audio_run_acmd`. Yield: no-op. |
| PI/DMA (4) | `osEPiStartDma`/`syDmaReadRom(rom, dst, n)` → memcpy from the sparse ROM image for the few raw regions still read that way (audio banks, RSP boot code never needed). Everything else goes through the data image (3.3.4). |
| SI (5) | `osContInit` reports 4 pads present; `osContStartReadData/GetReadData` fill from the harness pad ring. |
| Motor (3) | Gamepad vibration via an `EM_JS` callback, or no-op. |
| AI (2) | `osAiSetFrequency` returns 32000; `osAiSetNextBuffer` pushes to the PCM ring. `IO_READ(AI_LEN_REG)` returns the ring's queued bytes so the 552/368 sample logic still breathes. |
| n_audio (13) | Compiled as is. |
| gu (8) | Compiled as is (`src/libultra/gu`). |
| Cache ops (5) | No-ops. |
| SP IMEM/DMEM probes | Return the expected values so `gSYMainImemOK/DmemOK` are TRUE (`main.c:134-150`). |
| SRAM | Read returns the canonical `LBBackupData`; write is a no-op in v1. |
| libc | `bzero`, `memcpy`, `_Printf` → libc. |

About 1,500 lines. Every entry logs its first call so the surface can be audited against the ~85 names.

#### 3.3.3 Memory model

- `u8 gPortArena[PORT_ARENA_SIZE]` (start at 3 MB; measured in Phase 1) replaces `&ovl4_BSS_END .. gSYFramebufferSets`. `syTaskmanInitGeneralHeap` receives it under PORT.
- `u8 gPortGfxArena[]` holds the DL buffers, graphics heaps and the RDP FIFO buffer that `syTaskmanStartTask` carves (`taskman.c:1176-1203`). Render-only, outside snapshots.
- Framebuffers and zbuffer are declared with their N64 sizes so pointer arithmetic in `video.h` compiles, and are never read by the renderer.
- `SYVIDEO_DEFINE_FRAMEBUFFER_ADDR`'s `0x80400000` arithmetic is not used under PORT; `gSYFramebufferSets` is the array.
- **Pools are preallocated** (D-R1): `dSCVSBattleTaskmanSetup` pool counts (`scvsbattle.c:46-63`) are set under PORT to the measured high-water mark plus 25 % for GObj, DObj, AObj, MObj, XObj, SObj, CObj and processes; thread and stack pools are 0 and asserted unused. `syTaskmanMalloc` keeps a counter that the harness reads; it must read 0 inside the sim scope over a full match on every stage. The figatree load is a memcpy into a preallocated per-fighter buffer, not an allocation.

#### 3.3.4 Data image and loader

- `src/relocData/*.c` and `src/particles/*.c` compile to wasm data. `tools/port/gen_port_reloc_symbols.py` extends `genRelocSymbols.py` to emit `port/gen/reloc_fileids.h`: for every file, `llXxxFileID` is its index, `gPortFileTable[index] = {base_symbol, size}`; for every block, `llXxxBlock` is `(uintptr_t)&dBlock - (uintptr_t)&dFileBase` so `lbRelocGetFileData` (`library.h:7-8`) works unchanged.
- `lbRelocGetExternHeapFile(id, heap)` and friends (`lbreloc.c:302-389`) under PORT: return `gPortFileTable[id].base` without copying when `heap == NULL`-style callers only read; for the figatree path (`ftmain.c:4621`) memcpy the image into `fp->figatree_heap` to keep the game's semantics. Rule: static data is read-only. The Phase 1 proof hashes the whole data image before and after a match and fails on any change.
- Audio banks (`B1_*ctl`, `*tbl`, `S1_music_sbk`, `fgm.*`) are raw segments in `ssb64.data`, byteswapped by a port loader where `audio.c:312-420` patches them.
- `ssb64.data` is produced at build time from the ROM the owner holds, named by its hash, served by the relay. The page refuses to start on a hash mismatch and says why. Nobody ever brings a ROM to the page.

#### 3.3.5 Scene driver (`port/loop/scene_driver.c`)

A state machine stepped once per frame:

```
BOOT        shim init → audio init → harness takes boot image → PARKED
PARKED      NoController scene active (one sprite, no run function); netplay off;
            waits for StartMatch or a debug URL flag
MATCH_LOAD  restore boot image (keep-list: pad presence, harness state, canonical backup data)
            → write SCBattleState/SceneData from StartMatch args → syUtilsSetRandomSeed(seed)
            → scVSBattleStartScene body up to syTaskmanStartTask (func_start = scVSBattleStartBattle)
MATCH       syTaskmanStepTask() each frame until the break status
MATCH_END   the tail of scVSBattleStartScene as states: stop BGM; wait until
            !syAudioCheckBGMPlaying (max 120 frames); if tie and !is_reset → SUDDEN_DEATH_LOAD
            else → emit match result event → PARKED
SUDDEN_DEATH as MATCH with scVSBattleStartSuddenDeath on gSCManagerVSBattleState
```

The driver never sets `scene_curr = VSResults`, so no SRAM write and no menu. `scManagerRunLoop` is compiled out under PORT. The menus and 1P scenes are compiled but unreachable in v1.

### 3.4 Renderer (`port/gfx/`)

- Input: the finished `Gfx*` list per frame, segment table from `gSYTaskmanSegment*` (`taskman.c:238-242`), the Mtx packing from `matrix.c`.
- Interpreter: F3DEX2 opcodes as used by the game (G_VTX, G_TRI1/2, G_QUAD, G_MTX, G_POPMTX, G_DL, G_BRANCH_Z, G_TEXTURE, G_SETOTHERMODE_*, G_SETCOMBINE, G_SETTIMG/TILE/TILESIZE/LOADBLOCK/LOADTILE/LOADTLUT, G_FILLRECT, G_TEXRECT, G_SETPRIMDEPTH, G_SETFOGCOLOR, G_SETENVCOLOR, G_SETPRIMCOLOR, G_SETBLENDCOLOR, G_SETSCISSOR, G_SETZIMG, G_SETCIMG, G_RDPFULLSYNC, G_SETGEOMETRYMODE/CLEAR, G_MOVEWORD, G_MOVEMEM lights). Known SSB64 particulars: `G_SETPRIMDEPTH` for 2D layering, IA4/IA8/I4 textures, 2-cycle combiners for the fighter shading, texture rectangles for sprites. Build the opcode histogram from a recorded attract run in Phase 2 and implement exactly that set.
- Backend: WebGL2 (`EM_JS`/`emscripten_webgl`), combiner-derived shaders cached by combine mode, texture cache by (address, fmt, size, tlut hash), 320×240 logical resolution scaled to the canvas. A null backend for headless proofs; an SDL2+GL backend for the native build.
- RESIM: no draw (the loop's `draw=false`). Draw once per host tick, newest frame.

### 3.5 Audio (`port/audio/`)

- `n_alAudioFrame` (C) produces the Acmd list each frame as today (`audio.c:1038`). `port_audio_run_acmd(list)` interprets the `n_abi.h` command set into a stereo s16 buffer: A_CLEARBUFF, A_LOADBUF, A_SAVEBUF, A_ADPCM (with the big-endian book), A_RESAMPLE, A_ENVMIXER, A_MIXER, A_INTERLEAVE, A_SETVOL, A_SETBUFF, A_DMEMMOVE, A_LOADADPCM, A_SETLOOP, and the n_ variants. Reference behaviour is checked against emulator output on a recorded match.
- Output: a SharedArrayBuffer-free ring posted to an AudioWorklet via `postMessage` per frame (≈920 stereo frames), latency 2 to 3 frames. Mono downmix (`audio.c:1070-1078`) honoured.
- The audio heap, FGM lists and LFO pools are presentation. Never snapshotted, never hashed.

### 3.6 Harness (`port/net/`), the hooks layer

Exports (`EMSCRIPTEN_KEEPALIVE`, typed, no strings):

```
int32* NetState()                 frame, now, confirmed, top, stalled, delay, window,
                                  per-slot confirmed frame[4], last hash, hash frame,
                                  desync frame, flags, rollback count, max correction
int32  PadRingLayout()            slots=4, ring=64, stride; int16* PadRing(); int32* Confirmed()
void   StartMatch(localMask, slotKinds[4], fkinds[4], costumes[4], levels[4], teams[4],
                  stage, rules, timeLimit, stocks, damageRatio, teamAttack, itemRate,
                  itemToggles, seed, delay, window)
void   Park()
int    StepFrame(draw)            runs the netcode tick (3.9) and returns frames simulated
int    CanAdvance()               lockstep gate (v1)
uint32 HashAt(frame)              from the per-frame hash store
int32* EventRing(); int32* RngTrace()
void   SetScriptedSlot(slot, mode) off | scripted-pattern-A | scripted-human-like | bot(CPU)
void   SetFlags(f)                no-draw, heap-poison, byte-compare, rollback-test
```

**StartMatch pins** (from the findings, 2.5 and 2.6):
- `gSCManagerTransferBattleState`: `game_rules` (TIME=1 | STOCK=2), `time_limit` (minutes, 100 = infinite), `stocks` (lives−1), `is_team_battle`, `is_team_attack`, `damage_ratio` (100 vanilla), `handicap` (0), `item_toggles`, `item_appearance_rate` (0..5), `is_show_score` = TRUE, `is_not_teamshadows`, `pl_count`/`cp_count` recounted from `pkind`.
- Per `players[i]`: `pkind` (Man/Com/Not), `fkind` (0..11), `costume` via `ftParamGetCostumeCommonID` or `ftParamGetCostumeTeamID`, `shade`, `player` (= i, or team), `team`, `color` (i for humans, 4 for CPUs, team colour in teams), `tag`, `is_single_stockicon` (= time rule), `level` (1..9), `handicap` (1..9, default 9), `is_spgame_enemy` = FALSE.
- `gSCManagerSceneData.gkind` (stage 0..8), `is_reset` = FALSE, `scene_prev`, `scene_curr = nSCKindVSBattle`.
- `gSCManagerBackupData` = the canonical image: `error_flags = 0`, `boot` small, all unlocks set, `is_allow_screenflash` as the lobby says, `sound_mono_or_stereo` local-only.
- Pad presence: `gSYControllerDeviceStatuses = {0,1,2,3}`, `gSYControllerConnectedNum = 4`.
- `syUtilsSetRandomSeed(seed)` immediately before `scVSBattleStartBattle` runs; a separate draw seed pointer during draw, copying `scexplain.c:812-837`.
- Unique fighters per team are a lobby rule, not a game rule.

**Pad injection.** `syControllerFuncRead` under PORT: for each slot, take the pad for frame `f` from the ring (`int16 ring[4][64][2]` = buttons, stick packed; `confirmed[4]`), write `button_hold` and `stick_range`, derive `button_tap` and `button_release` against the previous frame's hold, clear `button_update`. Mirror slot 0 into `gSYControllerMain`. Local slots: capture the hardware pad for frame `f + delay` (lockstep) or `f` (rollback), write it to the ring, call `EM_JS send_pad(slot, frame, bytes)`.

**Hash** every 15 frames over, per fighter: `pos`, `physics.vel_air`, `vel_ground`, `vel_damage_ground`, `coll_data.pos_prev`, `percent_damage`, `stock_count`, `status_id`, `motion_id`, `lr`, `ga`, `shield`, hitlag, `input.pl`; per item and weapon: kind, pos, vel, owner, hitbox state; per stage: `gMPCollisionUpdateTic`, moving platform poses; battle: `time_remain`, `game_status`, scores; `syUtilsRandSeed()`. Object identity by slot and by a per-match spawn serial stamped at creation (never an address). Excluded: every field in 2.4 and 2.9 marked render-only or audio-only. Enough fields to localize the first divergence; the RNG trace does the rest.

**Event ring** (frame-stamped, not hashed): match_start, first_frame, go, pause, unpause, ko(slot), finish, match_end(result), stall, drop, park, quit, desync.

**RNG call-site tracing**: `syUtilsRandUShort/RandFloat/RandIntRange` become macros under PORT that record `(frame, __FILE__ id, __LINE__, seed_before)` into a 4096-entry ring. On desync the page dumps it. The first differing site is the bug.

**Debug URL flags** on the page: `?netN=SEED` (seeded offline N-slot match, all slots scripted), `?bots`, `?stage=`, `?lagms=`, `?lagdown=`, `?jitter=`, `?rbtest`, `?nodraw`, `?stats`. Guarded so a real lobby refuses them.

### 3.7 Determinism contract

Rules every change must keep. Each has an owner site.

1. One sim step per `StepFrame` unit. No delta time anywhere (`taskman.c:1374` fixed intervals; verified in 2.3).
2. The match timer counts sim frames: `sySchedulerGetTicCount()` returns the frame counter under PORT (`ifcommon.c:2472-2531`).
3. `gGMCameraMatrix` is computed at the end of the camera update process (`gmCameraRunFuncCamera`, `gmcamera.c:1113-1199`) under PORT, from the same camera state the draw would use. The draw keeps using it (`gmcamera.c:985-1018`). The boomerang (`wplinkboomerang.c:104, 354`) then reads a value that never depends on drawing.
4. No RNG in draw. Draw seed swapped in during draw as a tripwire (`scexplain.c` pattern).
5. The seed is set at `StartMatch`. The LCG step is `u32`.
6. No general allocation inside the sim scope during a match. Pools preallocated. Counter must read 0.
7. Static data is read-only. Image hash before and after a match must match.
8. No suspended stacks in the sim: the five HUD threads are state machines; thread processes asserted absent.
9. Audio never feeds the sim: handles, crowd and announcer state are out of the hash; sound allocation is gated in RESIM and replayed from the journal.
10. Save data is the canonical image on every page. Anti-piracy probes pass.
11. UB sites pinned (D11).
12. Garbage-free: build with heap poison (fill fresh arena allocations with 0xA5) in proof runs; a read of unwritten memory shows up as a hash difference between poison patterns.

### 3.8 Snapshot and rollback (`port/net/snapshot.c`, `resim.c`)

**What goes in a snapshot** (`gen_snapshot_table.py` from the linker map):
1. Every `.data` and `.bss` symbol from game objects (`src/` except the excluded dirs), excluding by prefix: the audio heap and `n_audio` globals, framebuffers and zbuffer, `gPortGfxArena`, taskman DL/graphics bookkeeping that points into it, debug (`src/sys/debug.c` bss), and everything in `port/`.
2. `gPortArena[0 .. high-water]` plus `gSYTaskmanGeneralHeap.ptr`.
3. Never: audio state, event and RNG rings, hash store, pad ring, the snapshots.

Estimated S ≈ 2.5 MB (arena about 2 MB measured in Phase 5, game bss/data about 0.5 MB). Save and restore are two memcpys. Ring of W+2.

Snapshots and the boot image (D6) are the same mechanism. Phase 1 already uses the table for the boot image, so by Phase 5 the table is proven.

**Counters** in wasm: `now`, `C` (confirmed), `top`. Rollback to `r` = restore `r−1`, re-simulate `r..now` in RESIM, continue. Misprediction detection by the applied-pad ring (what pad the sim used per slot per frame) compared against arriving bundles; earliest difference is `r`. Frame 0 is neutral on every page.

**RESIM mode**: `draw=false`; `func_80026844_27444` returns the journaled handle for `(frame, call_index, id)` or NULL; `syAudioPlayBGM/StopBGM/SetBGMVolume` are no-ops; events, toasts and SFX counters fire only when `frame > top`. After a rollback completes: reconcile BGM (`gMPCollisionBGMCurrent` vs `sSYAudioBGMPlayingIDs[0]`), run the audio frame once.

**Final-frame rule**: anything the relay hears or the page acts on (match end, KO toasts, pause state) is read from the newest final frame row. A row per frame, rewritten by re-simulation.

**Budget that gates Phase 6** (60 Hz): S ≤ 4 MB, save+restore ≤ 1 ms, RESIM step ≤ 0.8 ms, so a worst-case W=12 rollback (~10 ms) fits inside one 16.7 ms frame on a desktop, measured at a 4× CPU throttle too. If the step is slower, W shrinks before the design changes.

### 3.9 Relay and netcode

**Relay** (`relay/`, Rust, axum + tokio, one binary, one port, `TCP_NODELAY`): serves `web/`, `ssb64.wasm`, `ssb64.data`, `/api`. Game-blind. Rooms with 5-character codes from an unambiguous alphabet, rate-limited creates, host secret pasted once per device, 30-minute idle expiry (empty rooms sooner). JSON control frames: `hello{name, build_id}`, `ready`, `state{booting|loading|ready}`, `config` (host), `start` (host), `ping/pong`, `go{seed, config, slots}`, `left`, `cpu`, `error`, `desync`, `leave`. Binary hot path, `u8 type` first byte:

- Input, page → relay: `type=1, u8 slot, u32 frame, u8 count, count × {u16 buttons, i8 x, i8 y}`, last 8 pads, 60/s ≈ 2.3 kB/s up.
- Hash, page → relay: `type=2, u8 slot, u32 frame, u32 hash`, every 15 frames.
- Frame bundle, relay → page: `type=4, u32 frame, u8 n, u8 replacedMask, n × {u16, i8, i8}` in slot order, 60/s ≈ 1.4 kB/s down at 4 humans. The bundle is the only truth.

**Clock**: starts when every page has simulated past frame 12 (first picture). Frame `f` closes when every slot delivered its pad for `f`, never before `t0 + f/60 s`, and at `t0 + (f+G)/60 s` regardless, G = 6 frames (100 ms). A missing pad is the slot's last pad, marked replaced. A late pad becomes that slot's last pad. A slot silent for 3 s (180 frames) goes CPU by a `cpu{frame}` message ordered with the bundles; `left` likewise. A page that simulated past that frame rolls back.

**Netcode v1, lockstep over bundles**: local pad sampled at `f` applies at `f + D`, `D = clamp(ceil((RTT/2 + 16 ms)/16.7 ms), 2, 10)` from RTT at `go`. The page simulates `f` only when bundle `f` is held. Stall overlay after 24 frames with a reason. Hash compare every 15 frames; mismatch = banner, `desync`, RNG dump, park.

**Netcode v2, rollback with the relay as clock**: local delay 0, missing remote pad predicted as the slot's last pad, window W = 12 frames (200 ms) to start, raised to 18 if the budget allows. Pacing lead over the last bundle frame of `ceil(RTT/16.7 ms) + 2`, G tolerance only; behind → two sim frames this tick, draw newest; ahead → skip one; never past `C + W`. RTT = lowest of recent pings. No visual smoothing in v1; correction sizes logged per rollback.

Deleted by design (never built): adaptive delay, false-raise guards, peer drop agreement, peer balancing, P2P pads.

### 3.10 Page (`web/`)

Plain HTML lobby: room code, roster with ping and engine state, stage, rules, items, character and team pick with unique-character enforcement, share link, one screen at a time. The WebSocket lives for the session with auto-rejoin on drop. Error paths: connection lost, host left, room full, build mismatch, data hash mismatch, unreachable. On `go`: park pads, `StartMatch`, collapse the lobby to a stats strip. The canvas is 320×240 scaled to fit, landscape. Keyboard mapping and Gamepad API with an N64-style layout; one page owns one slot in v1 (two for couch, as an extra).

### 3.11 Deploy

Relay between the players (US east coast for Costa Rica–Germany per the playbook), one binary, one port, TLS at the edge. A stage ships only if wasm, data file, page JS and relay carry one build id from one clean commit. Scratch stack on other ports for proofs; the Playwright helper refuses the live URL unless allowed.

### 3.12 Numbers to start from (60 Hz)

| Item | Value | Notes |
|---|---|---|
| Sim tick | 60 Hz, one step | `taskman.c:1374` |
| Hash cadence | every 15 frames (250 ms) | frame-stamped |
| Pad ring | 4 slots × 64 frames | layout exposed by a call |
| Input redundancy | last 8 pads per message | 60 msgs/s, ~2.3 kB/s up |
| Lockstep delay D (v1) | 2..10 frames from RTT | replaced by rollback |
| Rollback window W | 12 frames (200 ms), target 18 | pads hold to about RTT 150..250 ms |
| Snapshot ring | W + 2 | about 35 MB at S = 2.5 MB |
| Relay close grace G | 6 frames (100 ms) | floor at `t0 + f/60` |
| Clock start | every page past frame 12 | past first picture |
| Pacing lead | `ceil(RTT/16.7) + 2` | lowest recent RTT |
| Auto-CPU after silence | 180 frames (3 s) | relay-timed |
| Stall banner | 24 frames (400 ms) | with a reason |
| Lobby code | 5 chars | rate-limited creates |
| Room idle expiry | 30 min | empty rooms sooner |
| Build id | sha256(wasm) | relay refuses a mismatch |
| Rollback budget | S ≤ 4 MB, save+restore ≤ 1 ms, RESIM ≤ 0.8 ms | gate before network work |
| Bundle | ~23 bytes, 60/s | ~1.4 kB/s down |

---

## 4. Phases and to-do

Status and proof results live in `proof/changes/`. The 8-player work below came in after the plan was written and went first.

### Phase 8P. Eight human players

Goal: 8 humans in one match, online, on every stage. The original N64 build keeps its 4.

Done. `GMCOMMON_PLAYERS_MAX` and `MAXCONTROLLERS` are 8 under `PORT`; `GMCOMMON_PLAYERS_NATIVE` stays 4 for what the cartridge authored for 4. See `proof/changes/8players.md`.

Every phase ends with `proof/changes/<phase>.md`: what ran, numbers, what failed, what is open. Nothing is done
without one. Each phase lists files, functions and expected line counts so it can be cut into slices.

### Phase 1. Headless port: the sim runs a VS match natively and in wasm

**Status: done in wasm; native build dropped.** Details in `proof/changes/`.

Goal: `port_boot(); StartMatch(...)` with four scripted slots runs to match end, headless, in a Linux binary and in
`node ssb64.js`. Logs `[frame, hash]` every 15 frames.

To do:
- [ ] `Makefile.port` with the compile set of 3.2, `-DPORT -DAVOID_UB -DNON_MATCHING`, native and wasm targets. Expect 150 lines.
- [ ] `tools/port/gen_port_reloc_symbols.py`: `port/gen/reloc_fileids.h` and `gPortFileTable`. Expect 200 lines, reusing `genRelocSymbols.py`.
- [ ] Data objects: compile `src/relocData/*.c` and `src/particles/*.c` for wasm. Fix the handful of files that need `#ifdef PORT` for pointer-typed slots (expect few; the sources are already typed). Measure data size.
- [ ] `port/shim/*`: the table in 3.3.2. Expect 1,500 lines. Assert on any blocking `osRecvMesg`.
- [ ] `port/data/loader.c`: `lbRelocGet*File` under PORT, figatree memcpy, audio-bank byteswap loader (stubbed audio still needs banks to init). Expect 400 lines.
- [ ] `port/loop/port_main.c`, `scene_driver.c`: D2 and D3. `syTaskmanStepTask` split in `taskman.c` under PORT. Expect 600 lines plus 60 lines of guards in `taskman.c`.
- [ ] `src/if/ifcommon.c`: the five thread processes as state machines under PORT (`ifCommonAnnounceThread`, `ifCommonCountdownThread`, `ifCommonEntryFocusThread`, `ifCommonEntryAllThread`, `ifCommonSuddenDeathThread`). Expect 250 lines. Assert no `nGCProcessKindThread` in `gcAddGObjProcess` under netplay.
- [ ] `src/sys/controller.c`: PORT `syControllerFuncRead` from a per-frame pad source (a stub driver in this phase: neutral or scripted). Expect 80 lines.
- [ ] Determinism fixes in `src/`: tic count = frame count (`scheduler.c` under PORT), camera matrix at camera update end (`gmcamera.c`), `u32` LCG step (`utils.c`), seed setter call, anti-piracy stubs (`scvsbattle.c:139-151`, `sc1pgame.c:2043-2056`, `main.c:134-150`), UB pins (D11). Expect 150 lines total.
- [ ] Pools preallocated in `scvsbattle.c` under PORT; `syTaskmanMalloc` counter. Expect 40 lines.
- [ ] `tools/port/gen_snapshot_table.py` from the wasm linker map (and `nm` for the native build): first use is the boot image (D6). Expect 250 lines.
- [ ] `port/net/start_match.c` with the pin list of 3.6 and `Park()`. Expect 300 lines.
- [ ] `port/net/hash.c` (field list of 3.6), `rngtrace.c`, `events.c`. Expect 500 lines.
- [ ] Scripted pad drivers: pattern A (new extreme stick every 7 frames, buttons on a schedule) and human-like (held stick changed about once a second). Expect 120 lines.
- [ ] Headless runner: `port/tools/run_match.c` and `web/tools/run_match_node.js` taking stage, seed, slots, frames.

Proof (`proof/changes/phase1.md`):
- Same seed twice → identical `[frame, hash]` logs to the last frame, on all 9 stages, items on, 4 scripted slots, and 2 scripted + 2 CPU.
- Different seed → differs from frame 0.
- Race after race on one process: three matches in a row, each identical to its standalone run (D6 test).
- Native vs wasm hashes compared (informational; only wasm is shipped).
- Heap-poison pattern A vs pattern B identical hashes.
- `syTaskmanMalloc` counter 0 inside the sim scope; data-image hash unchanged.
- Draw vs no-draw: not possible yet (no renderer); done in Phase 2.

Exit: all green, arena high-water mark and pool high-water marks recorded, data file size recorded.

### Phase 2. Picture and sound in the browser

**Status: works; judged-by-ear, emulator comparison and gamepad open.** Details in `proof/changes/`.

Goal: an offline VS match plays in a browser tab with keyboard or gamepad, looks right on all 9 stages with all 12 fighters, sounds right.

To do:
- [ ] `port/gfx/f3dex2.c`: opcode histogram first (record the headless run's display lists to a file), then the interpreter for exactly that set. Expect 2,500 lines.
- [ ] `port/gfx/webgl2.c` + `web/gfx.js`: shaders from combiner modes, texture cache, scissor, depth, fog, blending modes used. Expect 1,200 lines. `port/gfx/sdl_gl.c` for native, 300 lines.
- [ ] `port/audio/naspmain.c`: command interpreter per `n_abi.h`. Expect 1,200 lines. `web/audio-worklet.js` ring, 150 lines.
- [ ] `web/index.html`, `web/game.js`: canvas, rAF loop calling `StepFrame(1)` once per 16.7 ms with a fixed-step accumulator, keyboard and Gamepad mapping, `?netN=SEED` flag path. Expect 600 lines.
- [ ] `?nodraw` flag and the draw-vs-no-draw proof.

Proof:
- Screenshots of all 9 stages and every fighter's idle, attack, shield and KO visuals against an emulator reference; notable mismatches listed.
- Draw vs no-draw: hashes identical on every stage with 4 scripted slots (covers 2.4, including the `FTParts` question; if it fails, the fix is to compute the cache in the update pass under PORT and the proof note says so).
- Audio: a 60 s recording compared by ear and by SFX counter totals against an emulator run of the same inputs.
- Frame time at 320×240 on the dev desktop and at 4× CPU throttle: update, draw, audio each measured.

Exit: playable offline, all proofs green, measured costs in the note.

### Phase 3. Harness complete, offline

**Status: done.** Details in `proof/changes/`.

Goal: everything the page will need from the wasm exists and is exercised offline.

To do:
- [ ] `port/net/padring.c`, `netstate.c`: ring, confirmed, NetState layout, `EM_JS` callbacks `send_pad`, `send_hash`, `event`. Expect 400 lines.
- [ ] Full URL flag set of 3.6; harness exports refused when a lobby is active.
- [ ] Build id stamping into `web/build.js`; `ssb64.data` hash check in the page.
- [ ] Playwright helper library (`web/tests/lib.js`): launch, env, flags, wait on events, hash compares, summaries. Expect 500 lines.

Proof: seeded offline 4-slot match driven through the pad ring (page writes scripted pads into the ring exactly as a relay would), twice identical; a flipped button at frame 900 reported as a divergence at frame 900 by the hash; RNG trace dump names the site.

### Phase 4. Relay, lobby, lockstep over the internet

**Status: done except one real internet match.** Details in `proof/changes/`.

Goal: four friends on four machines play a full match with lockstep. Short phase; it is scaffolding, but it is the first time people play.

To do:
- [ ] `relay/`: rooms, codes, host secret, hello/build gate, config/start/go with seed and slot list, ping/pong, clock and bundles of 3.9, cpu/left on silence, hash fan-in and desync broadcast, static file serving. Expect 2,500 lines of Rust.
- [ ] `web/lobby.js`, `web/net.js`: the page of 3.10, lockstep gate (`CanAdvance`), delay from RTT, stall overlay, desync banner, rejoin. Expect 1,500 lines.
- [ ] `?lagms`, `?lagdown`, `?jitter` in the page; add lag to the RTT the page reads.
- [ ] Scratch stack (other ports) and the deploy script that checks the build id across artifacts.

Proof: 4 headless pages on one machine with simulated lag both ways, full match, zero desyncs across 10 seeds × 3 stages; one real match across the internet with the owner and friends; stall rate and delay recorded.

### Phase 5. Snapshot and re-simulation, offline, with a budget

**Status: done, budget met.** Details in `proof/changes/`.

Goal: go/no-go numbers for rollback.

To do:
- [ ] `gen_snapshot_table.py` exclusions finalized; the generator stops on an excluded symbol inside a run.
- [ ] `port/net/snapshot.c`: save/restore, ring of W+2, byte-compare mode. Expect 400 lines.
- [ ] `port/net/resim.c`: RESIM flag (per the single thread, one flag), draw/audio/event gating, applied-pad ring, journaled sound handles (`audio_journal.c`, 150 lines), BGM reconcile. Expect 500 lines.
- [ ] `?rbtest=K`: every K frames restore `now − k` and re-simulate; compare hashes and a digest of the whole snapshot table against a straight run; count allocations and frees; negative control that breaks the restore and must fail.

Proof: identical on every stage, items on, scripted and CPU slots; S, save, restore and RESIM step times on the desktop and at 4× throttle, against the budget of 3.8; audio trigger counters equal to a straight run.

Exit: budget met or W reduced with the reasoning written down.

### Phase 6. Rollback with the relay as the clock

**Status: done except one real internet match.** Details in `proof/changes/`.

Goal: the Counter-Strike feel. The laggy player alone pays for lag.

To do:
- [ ] Page netcode v2 of 3.9: prediction, window, pacing lead, run-two/skip-one, confirmed-only hashing, final-frame rows, correction logging. Expect 800 lines in `web/net.js` and 300 in `port/net/`.
- [ ] Remove the lockstep gate from the page path (keep it behind a flag for proofs).
- [ ] Rollback stats strip; event toasts from final frames only.

Proof: wrong predictions with per-slot scripted pads (pattern A and human-like), lag both ways, a page that is really slow (busy-loop its main thread; print its frame rate and fail if it is not slow), a page that stops sending (goes CPU on the relay timer while others keep playing), a page that leaves. Zero desyncs. Correction size distribution logged. One real match over the internet.

### Phase 7. Deploy and people

**Status: waiting on the owner: a server and players. Stage script ready.** Details in `proof/changes/`.

To do:
- [ ] Relay placed between the players; TLS; one binary serving everything; host page with one NEW LOBBY button.
- [ ] Deploy scripts that check and stage; a human runs the restart.
- [ ] Playtests with people early, on any host, before the relay moves.

Proof: proofs re-run from each player's country; a playtest log with what people found.

### Phase 8. Extras (after the owner's go)

**Status: waiting on the owner's go.** Details in `proof/changes/`.

Couch two players on one page (two local slots, one socket); spectator join (a page with no slot rendering the shared view); offline local play with the game's own menus (compile the menu scenes into the driver); JP build.

### Parking lot (owner's ideas, not scheduled)

More room for 8 players. Owner parked it on 4 Nov 2026; nothing built. Assessment from reading the code, untested: stretching stages means hand work on hazards, and jumps don't scale with them. Shrinking fighters to about 80% gives the same result with stages untouched. The hard parts are the 94 fixed move speed and distance values, movement built into animations, and confirming whether hit areas follow the fighter size value. Cheapest experiment: all fighters at 80% on Dream Land.

### First coding slice (Phase 1, slice 1)

Files: `Makefile.port`, `port/shim/os_mesg.c`, `port/shim/os_thread.c`, `port/shim/os_misc.c`, `port/loop/port_main.c`. Goal: `syMainLoop`'s thread-5 body runs to the first `osRecvMesg` without an assert, with `scManagerRunLoop` compiled out and a stub scene driver that logs. Native build only. Expected 700 lines. Proof: a log line per shim entry point and a clean exit.

---

## 5. Risks

| Risk | Mitigation |
|---|---|
| The renderer is the largest unknown chunk (SSB64 uses prim depth, IA textures, 2-cycle combiners). | Opcode histogram first; implement exactly the set used; emulator reference screenshots; it is not on the determinism path, so Phase 1 does not wait for it. |
| Compiled data layout differs from the N64 in a place where game code depends on byte offsets (not symbol offsets). | `lbRelocGetFileData` goes through generated `ll*` deltas, so symbol-relative offsets are correct by construction. Any raw-offset arithmetic shows up as a crash or a hash difference in Phase 1 and is fixed by typing the block. |
| Mid-match arena growth we did not see (pool growth, stack pool). | Preallocation plus the malloc counter in every proof. |
| A hidden draw-to-sim coupling beyond the camera matrix (`FTParts` caches). | Draw vs no-draw proof on every stage in Phase 2. |
| Garbage-value sites pinned to values that differ from the N64's register leftovers. | Accepted deviation, logged. Same on every page, so no desync. |
| Rollback step cost too high at 60 Hz for W = 12. | Measured in Phase 5 before any network rollback work; W shrinks first. |
| Data distribution and legal posture of the served data file. | Owner's decision, as with the first game. The page never asks for a ROM. |

---

## 6. Decisions log (one line each; add, never rewrite)

- 2026-11-04 D1 source port to wasm, own thin platform layer; no emulator, no recomp.
- 2026-11-04 D2 single thread, stepped loop; no pthreads, no Asyncify.
- 2026-11-04 D3 stepped scene driver with three scenes; menus unreachable in v1.
- 2026-11-04 D4 five HUD GObj threads rewritten as state machines; thread processes asserted absent in matches.
- 2026-11-04 D5 asset data compiled from the typed C sources; no vpk0, no chains, no asset byteswap.
- 2026-11-04 D6 fresh match by boot-image restore; overlay reload semantics not reproduced.
- 2026-11-04 D7 relay has the clock and bundle protocol from day one; lockstep v1 runs over bundles.
- 2026-11-04 D8 F3DEX2 interpreter in C with WebGL2; sm64-port's gfx_pc as the reference shape.
- 2026-11-04 D9 n_aspMain interpreter in C; audio after determinism.
- 2026-11-04 D10 snapshot and hash tables generated from the linker map.
- 2026-11-04 D11 AVOID_UB on; garbage-argument sites pinned: `itemthrow_buffer_tics = 0`, Link bomb `stat_flags = 0, stat_count = 0`, Link Up-B passes the weapon gobj, Mario fireball id falls back to Mario's.
- 2026-11-04 D12 desktop landscape only.
- 2026-11-04 match timer counts sim frames under PORT (`sySchedulerGetTicCount` = frame counter).
- 2026-11-04 `gGMCameraMatrix` computed at camera update end under PORT.
- 2026-11-04 sound gate is `func_80026844_27444`; RESIM replays journaled handles.
- 2026-11-04 60 Hz numbers of 3.12 are starting points, not conclusions.
- 2026-11-04 8 players under PORT: stage files keep their 4 emblem colours, spawn points, voices and particle scripts; slots 5 to 8 get derived spawns, new colours and drawn tags.
- 2026-11-04 `gFTMainIsDamageDetect` and `gFTMainIsAttackDetect` are per hitbox, not per player, despite upstream's constant. They stay 4.
- 2026-11-04 Attack records 12 under PORT, or a wide hit on more than 4 targets strikes one twice.
- 2026-11-04 Everything a display list points at stays under 16 MB, because the top address byte means a segment. Arena 8 MB, checked at boot.
- 2026-11-04 Pools not preallocated; the heap pointer is in the snapshot. Replaces the preallocation idea in section 3.
- 2026-11-04 Data compiled with gcc -m32 per-symbol sections, verified word by word against the ROM; palettes and textures keep ROM bytes.
- 2026-11-04 Crowd sound handles and serials are excluded from the snapshot digest, like fighter and weapon ones.
- 2026-11-04 With more than 4 fighters, or anyone in slots 5 to 8, all damage readouts share one smaller row along the bottom (owner's request). 4 or fewer keep the original layout.

---

## 7. Open questions for the owner (answered when they block, not before)

1. Data file: the relay serves `ssb64.data` built from the owner's ROM, as the first game did. Confirm. (Blocks Phase 4 deploy, not Phase 1.)
2. CPU slots in online matches: allowed by default in the lobby? Assumed yes.
3. Items default in the lobby: vanilla on, "medium". Assumed.
4. Pause online: START pauses for everyone, as in the game; A+B+R+Z ends the match for everyone. Keep, or filter START from remote slots? Assumed keep.
5. Which parts of the first game's page and relay code can be reused here? The relay is game-blind by design, so it should carry over nearly whole. Not inspected; nothing in this plan depends on it.
6. Rumble: on, through the Gamepad vibration API, or off? Assumed on where available.

---

## 8. References

- `/home/desktop/Documents/netplay-port-playbook.md`: the method this plan follows. Section numbers in this document map to it where they overlap.
- Upstream decomp: `https://github.com/vetritheretri/ssb-decomp-re` (US ROM SHA1 `e2929e10fccc0aa84e5776227e798abc07cedabf`).
- Prior art, not used as a base: a libultraship-based PC port of this decomp exists (JRickey/BattleShip, MIT). Noted only so nobody rediscovers it; this plan derives nothing from it.
