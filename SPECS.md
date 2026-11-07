# ssb64: specs, analyses and what was built from them

Last checked: 7 Nov 2026, against `git log` (branch `renderer` = `origin/main`, 2a8103974) and the live `web/build.js`.

## How to read this

- One section per spec. Each row is one finding, phase or idea of that spec.
- **Status**: **Implemented** (done), **Partial** (part done, the rest left out on purpose; the note says why),
  **Skipped** (decided not to do; the note says why), **Pending** (not started yet).
- **Live**: the build id that carries the change on https://ssb64.guanavibes.com and the n64 portal.
- The reasons in the notes come from the implementation notes linked under each section, not from this file.
- **Live today (7 Nov 2026): build `49c2190f7b082d3f`** (data `16ffa10ab963edb9`), from commit 2a8103974, on
  ssb64.guanavibes.com and the n64 portal. It contains everything on this page marked Implemented or Partial.
  Earlier builds named below (`9c0809ae`, `47c1ade4`) are superseded by it.

## Documents

All of them stay where they are; nothing was moved.

| Document | Date | Kind | What it is for |
|---|---|---|---|
| [`spec/state-machines.md`](spec/state-machines.md) | 2026-11-06 | spec | 12 findings (SSB-SM-xx) on hidden state machines in page, client, relay room and rollback core, in 5 phases |
| [`proof/state-machines/README.md`](proof/state-machines/README.md) | 2026-11-06 | proof note | What was fixed, skipped and proved for the state-machines spec; the owner's rules; relay numbers |
| [`proof/gfx/README.md`](proof/gfx/README.md) | 2026-11-06 | analysis | Why Meta Crystal and Metal Mario looked wrong (byte-swapped palettes) and why matches felt laggy (one GL draw per triangle) |
| [`spec/renderer.md`](spec/renderer.md) | 2026-11-06 | spec | Fable's analysis of the display-list to WebGL2 path, measured, with phases 0-5 |
| [`spec/fast3d-lessons.md`](spec/fast3d-lessons.md) | 2026-11-06 | research | Fable's research: which ideas from mk64's libultraship Fast3D renderer are worth copying (items 2.1-2.12) |
| [`proof/renderer/README.md`](proof/renderer/README.md) | 2026-11-06 | proof note | What was done and proved for renderer phases 1-5 and the Fast3D lessons; screenshots and numbers |
| [`proof/widescreen-spec.md`](proof/widescreen-spec.md) | 2026-11-05 | spec | Fable's widescreen spec (phases W0-W5) |
| [`proof/sweep/fable/streaks.md`](proof/sweep/fable/streaks.md) | 2026-11-05 | research | Fable's second opinion on the rainbow streaks on Dream Land (a port bug, fixed) |

Older plans, research and proofs are listed at the end ([Older plans and proofs](#older-plans-and-proofs)).

## State machines (`spec/state-machines.md`)

Implementation notes: [`proof/state-machines/README.md`](proof/state-machines/README.md).
Commits `9f2c5dc75` (phases 1 and 2), `b60eafd30` (phase 3), `f94a1886c` (phase 4), `101a89370` (proofs, docs).
First live in build `9c0809ae7ff8d638`; now in **`49c2190f`**.

**The owner's decisions (they override the spec):**
1. **No CPU takeover.** An away player's fighter stands on the zero pad. When they come back, their pads count again.
2. **The match end waits only for players who are present** (or 10 s after the first finish).
3. **A desync ends the match** (`desync` at once, `over` 2 s later, the room is idle again).

| ID | What (plain English) | Status | Commit(s) | Live | Note |
|---|---|---|---|---|---|
| SSB-SM-01 | Page screen and mode (frozen canvas, no way back) | Implemented | 9f2c5dc75 | 49c2190f | One writer of screens; every failure lands on a screen with a message. |
| SSB-SM-02 | Engine load and sound start (sound never resumed) | Implemented | 9f2c5dc75 | 49c2190f | Every press retries until sound runs; "Click for sound" while the browser waits. |
| SSB-SM-03 | Client session and rejoin (stalls for good) | Implemented | 9f2c5dc75 | 49c2190f | One reconnect timer; the relay keeps the race 30 min and resumes it; a reloaded page rejoins its match. |
| SSB-SM-04 | Who owns the music (item music restarts) | Implemented | b60eafd30 | 49c2190f | One music request; the lobby reads it. |
| SSB-SM-05 | Relay room and race life cycle (desync stuck "in progress") | Implemented | 9f2c5dc75 | 49c2190f | `relay/src/room.rs`: Idle / Starting / Running / Ending, one host rule, kick removed. |
| SSB-SM-06a | Sudden death's first frames run as re-simulations | Implemented | b60eafd30 | 49c2190f | Reset per scene. |
| SSB-SM-06b | A dropped page rolls back every bundle | Skipped | - | - | Moot: no drop to CPU any more. |
| SSB-SM-06c | Park leaves the rollback core on | Implemented | f94a1886c | 49c2190f | Park now turns the core off. |
| SSB-SM-06d | The held finish has no name | Partial | f94a1886c | 49c2190f | Comment only, describing where it lives; no code change (the spec found it correct). |
| SSB-SM-06e | Hash state not cleared at match start | Skipped | - | - | Safe by construction (spec); an assert adds nothing a player sees. |
| SSB-SM-07 | Scene driver: dead states, two end tails | Partial | f94a1886c | 49c2190f | Dead state and flags removed, one end tail, Park ejects. `PortMatchPhase()` not added: nothing reads it. |
| SSB-SM-08 | HUD threads as coroutines | Skipped | - | - | Risk without a player-visible gain: a one-instance check could crash a real match if a scene ever re-makes one. |
| SSB-SM-10 | Drop to CPU (permanent, invisible to the player) | Implemented | 9f2c5dc75 | 49c2190f | Done differently, per owner rule 1: the CPU drop is deleted and replaced by away/back. |
| SSB-SM-11 | Per-match resets (audio journal carries over) | Implemented | b60eafd30 | 49c2190f | One per-match reset. |
| SSB-SM-12 | Portal mode vs standalone (two retry timers, long names) | Implemented | 9f2c5dc75 | 49c2190f | One retry timer with back-off; "match in progress" gone: a newcomer waits in the room. |
| SSB-SM-15 | Stall banner (one message unreachable, three writers) | Implemented | 9f2c5dc75 | 49c2190f | One banner with ranks; "Your connection stopped." now shows. |
| Phase 5 | Optional protocol hardening | Partial | 9f2c5dc75 | 49c2190f | Engine state checked by the relay, kick removed. No `proto` field: the build id already pairs a page with its relay. |

Not proven (from the note): autoplay in a real Chrome after an auto-join; a match paused by a player who then
goes away stays paused (only they can unpause); no real internet match.

## Meta Crystal, Metal Mario and the laggy match (`proof/gfx/README.md`)

Analysis with two fixes. First live in build `47c1ade42215a22e` (portal commit `2494112`); now in **`49c2190f`**.

| ID | What (plain English) | Status | Commit(s) | Live | Note |
|---|---|---|---|---|---|
| GFX-1 | Meta Crystal and Metal Mario wrong: the data packer byte-swapped their palettes | Implemented | d0df0024f | 49c2190f | Renderer was cleared; the data image was wrong. Sim hashes unchanged (57/57). |
| GFX-2 | Matches felt laggy: one GL draw per triangle | Implemented | d6cb811fe | 49c2190f | Draws batched, a GL state cache: ~600-780 draws a frame down to ~130-180; pixels identical. This is renderer phase 0. |

## Renderer (`spec/renderer.md`)

Implementation notes: [`proof/renderer/README.md`](proof/renderer/README.md). Commits `5862b9ab9..2a8103974`.
**Live: build `49c2190f7b082d3f`** on ssb64.guanavibes.com and the n64 portal (portal commit `1bb21c8`).

| Phase | What (plain English) | Status | Commit(s) | Live | Note |
|---|---|---|---|---|---|
| 0 | Profile counters, batched draws, GL state cache | Implemented | d6cb811fe | 49c2190f | Done before the spec (see GFX-2). |
| 1 | Cheap wins: build draw state only when it changes; texture and shader caches with eviction | Implemented | 5862b9ab9 | 49c2190f | Interpreter 0.31 to 0.09 ms a drawn frame (node). |
| 1+ | Second data fix: palettes named by material-animation tables kept big-endian | Implemented | dfd5df3d0 | 49c2190f | Found when phase 2 showed green seams on Final Destination. |
| 2 | Decode the N64 blender (hit flashes, tints, fog) | Implemented | efed8dc6b | 49c2190f | Hit flashes and respawn tints now draw; Meta Crystal's girders right. |
| 3 | Near-plane clipping | Implemented | 1ff7da9eb | 49c2190f | No pixel changed at the fixed frames. |
| 4 | Picture: 3-point filter, 4x MSAA, N64-resolution mode, lobby Picture button | Implemented | 75c22e23e | 49c2190f | Defaults chosen without a real GPU; "Sharp" is one click if a laptop struggles. |
| 5 | One vertex upload per frame | Implemented | d8e453bbf | 49c2190f | The batching option of phase 5; WebGPU skipped (below). |
| - | WebGPU backend | Skipped | - | - | WebGL calls are no longer where time goes; a second shader generator; partial Safari/Firefox support. |
| - | Adopt libultraship; frame interpolation | Skipped | - | - | The spec says not to; nothing found changes that. |
| - | Smaller gaps: noise, mipmaps, texgen linear, texcoord detail, 16-bit depth, SIMD vertices, rectangles under a viewport | Skipped | - | - | The spec ranks them cosmetic, unused by the game, or not a bottleneck. |

Overall: a drawn frame is 25-55 % cheaper; sim hashes unchanged (57/57, 54/54 drawn).

## Fast3D lessons (`spec/fast3d-lessons.md`)

Implementation notes: the "spec/fast3d-lessons.md" section of [`proof/renderer/README.md`](proof/renderer/README.md).
No libultraship code was copied; ideas 2.2 and 2.3 were used as reference and written anew.

| Item | What (plain English) | Status | Commit(s) | Live | Note |
|---|---|---|---|---|---|
| 2.1 | Half-texel offset under bilinear filtering | Implemented | 599b26ea9 | 49c2190f | |
| 2.2 | 3-point filter | Implemented | 75c22e23e | 49c2190f | Used as reference; own code (phase 4). |
| 2.3 | MSAA and native-resolution picture | Implemented | 75c22e23e | 49c2190f | Used as reference; own code (phase 4). |
| 2.4 | Load only the textures the combiner reads | Implemented | 52ad96cee | 49c2190f | Texture lookups 260 to 178 a frame (FD). |
| 2.5 | Clamp to the tile in the shader | Implemented | 455553227 | 49c2190f | Fixed Mario's cap emblem and Hyrule's wall edges. |
| 2.6 | Mask 0 with wrap means clamp | Skipped | - | - | 0 draws in the game use it. |
| 2.7 | Copy-mode rectangles unfiltered | Implemented | 52ad96cee | 49c2190f | Yoshi's Island backdrop crisp. |
| 2.8 | Decal offset scaled to the picture height | Implemented | ebab64936 | 49c2190f | No decal state in matches: no visible change. |
| 2.9 | Fewer depth clears | Partial | ebab64936 | 49c2190f | The frame's first clear is skipped; later ones still clear everything (scissoring would miss the widescreen bands). |
| 2.10 | Drop triangles fully off screen early | Implemented | ebab64936 | 49c2190f | 41-221 triangles a frame dropped before state work. |
| 2.11 | Prim/env colours as vertex attributes | Skipped | - | - | Colour-only state changes are 0.1-3 a frame: nothing to gain. |
| 2.12 | Texture cache keyed by address | Skipped | - | - | Ranked last; hashing is cheap now, and the content hash catches data-image errors. |

## Widescreen (`proof/widescreen-spec.md`)

Built from this spec on 5 Nov (commit `d8f447242`, see `CHANGES.md` "Second pass"); in the live build. Its phases W0-W5
were not tracked one by one in an implementation note. Known gaps (the spec's section 5) are in `PENDING.md`,
"Known gaps in the game": no name tag in the side bands, the magnifying glass at the 4:3 edge, Link's boomerang
vanishing at the 4:3 edge.

## Older plans and proofs

Plans, research and proofs from before these specs, with each file's first heading. They stay where they are. Dates
are the file's last commit. Proof logs (`proof/*.log`, `proof/changes2/`, `proof/sweep/`) sit next to them.

#### Top level
- [`README.md`](README.md) (2026-11-06): ssb64
- [`PLAN.md`](PLAN.md) (2026-11-06): PLAN: Super Smash Bros. 64 online, in the browser
- [`CHANGES.md`](CHANGES.md) (2026-11-06): Changes
- [`PENDING.md`](PENDING.md) (2026-11-06): ssb64: pending items
- [`CLAUDE.md`](CLAUDE.md) (2026-06-12): Super Smash Bros. 64 Decompilation

#### Decompilation notes (from the decomp project, before the port)
- [`MUSIC_AND_SFX_DISCOVERIES.md`](MUSIC_AND_SFX_DISCOVERIES.md) (2026-04-30): Music & SFX Decomp — Discoveries
- [`PARTICLE_BANK_DISCOVERIES.md`](PARTICLE_BANK_DISCOVERIES.md) (2026-04-30): Particle Bank Decomp — Discoveries & Plan
- [`audio.md`](audio.md) (2026-04-30): Audio
- [`relocData.md`](relocData.md) (2026-06-09): relocData
- [`src/audio/fgm/README.md`](src/audio/fgm/README.md) (2026-04-29): `src/audio/fgm/` — FGM data overrides
- [`src/audio/instruments/README.md`](src/audio/instruments/README.md) (2026-04-29): `src/audio/instruments/` — instrument-bank waveform overrides
- [`src/audio/midis/README.md`](src/audio/midis/README.md) (2026-04-29): `src/audio/midis/` — music sequence overrides
- [`docs/index.html`](docs/index.html): the decomp's "SSB64 Decomp — Remaining Functions" page (HTML, not a plan)

#### Port phase history (`proof/changes/`, one file per PLAN.md phase)
- [`proof/changes/8players.md`](proof/changes/8players.md) (2026-11-04): 8 human players
- [`proof/changes/phase1.md`](proof/changes/phase1.md) (2026-11-04): Phase 1: headless port. Done, with two items dropped
- [`proof/changes/phase2.md`](proof/changes/phase2.md) (2026-11-04): Phase 2: picture and sound. Works; human checks open
- [`proof/changes/phase3.md`](proof/changes/phase3.md) (2026-11-04): Phase 3: harness. Done
- [`proof/changes/phase4.md`](proof/changes/phase4.md) (2026-11-04): Phase 4: relay, lobby, online play. Done except the real internet match
- [`proof/changes/phase5.md`](proof/changes/phase5.md) (2026-11-04): Phase 5: snapshot and re-simulation. Done, budget met
- [`proof/changes/phase6.md`](proof/changes/phase6.md) (2026-11-04): Phase 6: rollback online. Done except the real internet match
- [`proof/changes/phase7.md`](proof/changes/phase7.md) (2026-11-04): Phase 7: deploy and people. Live; no real players yet

#### Other proof notes
- [`proof/extras/README.md`](proof/extras/README.md) (2026-11-06): Extras: three stages, three kinds of fighter, the item switch
