# Extras: three stages, three kinds of fighter, the item switch

Branch `extras` (worktree `~/sb64/ssb64-extras`), on top of `d8f447242`. Not deployed and not pushed.
Every proof ran on this machine against a private relay on a random port, serving the worktree's own `web/`.
The live site (`~/sb64/ssb64/web`, relay pid 1205581) was not touched.

## 1. Stages: Meta Crystal, Duel Zone, Final Destination

- **How a host picks one.** `web/lobby.js` `STAGES` lists stages by GRKind: the 9 VS stages, then
  `13` Meta Crystal, `14` Duel Zone and `16` Final Destination. Each is a tile in the Stage grid (4th row).
  A tile click sets `#stage_` and sends `{t:'config'}`, as every stage did before. `config().stage` is the
  GRKind, and `StartMatch` takes it unchanged.
- **Previews.** The game's stage-select screen has no picture for these three. `art.js` shows each stage's
  own wallpaper instead: files `0x62`, `0x61` and `0x60`, sprite `0x26c88`, the same wallpaper their map
  files name.
- **Final Destination music (engine fix).** Its map names the boss entrance jingle (`nSYAudioBGMBossEntry`).
  The 1P game swaps in the stage's theme when the fight starts. `scvsbattle.c` now does the same under `PORT`
  (both battle and sudden death). Proven: the sim reports BGM 25 (`nSYAudioBGMLast`), not 24.
- **Items on Final Destination.** None, as in the game: its map's `item_weights` is NULL. The lobby says so
  under the checklist, and the proof holds the port to it.

## 2. Fighters: Metal Mario, Giant DK, the Fighting Polygons (not Master Hand)

- **Lobby.** A pick now sends the FTKind. An "Extras" row under the 12 regular fighters holds Metal Mario
  (13), Giant DK (26) and the 12 polygons (14 to 25). The game has no select portrait for them, so each is
  drawn from its source fighter's portrait: Metal Mario in steel, the polygons in violet, Giant DK as DK
  enlarged. A roster card shows the picture and the name ("Metal Mario · ready", "Polygon Kirby · 40 ms").
- **Colours.** Metal Mario and the polygons have one look in the game (`dFTParamCostumeIDs`). Their colour
  buttons are disabled, and they send costume 0. Giant DK keeps DK's four.
- **Master Hand.** Not offered. `net.js` turns any pick other than 0-11 or 13-26 into the slot's default.
  Every page applies the same rule to the same picks, so the pages still agree. Proven with a pick of 12.
- **Announcer.** The lobby's `LobbyCue` now calls "Metal Mario", "Fighting Polygon Team" and "Giant Donkey
  Kong" with the 1P game's own voices (`nSYAudioVoiceAnnounceMMario/Zako/GDonkey`).
- **In the match (HUD).** The player tags (1P/2P/CP), damage panels and timer work as for anyone. Screenshot:
  `shots/match_host.png` shows Metal Mario and Polygon Kirby on Final Destination. Metal Mario and Giant DK
  have stock icons. **The polygons have none:** the game has no stock sprite for them, and the HUD skips
  them, as it would on the N64. The flying stock-steal icon (team stock) dereferenced that NULL sprite and
  would have crashed; it now skips polygons too (`ifcommon.c`, under `PORT`). Found by Fable's audit.

## 3. Item switch checklist

- **UI.** An "Item switch" panel holds the 15 VS switches, in the game's menu order: Beam Sword, Home-Run
  Bat, Hammer, Fan, Motion-Sensor Bomb, Bob-omb, Bumper, Shell, Poké Ball, Ray Gun, Fire Flower, Star Rod,
  Maxim Tomato, Heart, Star Man. It also has All on / All off buttons. Only the host can change it, and the
  appearance-rate dropdown is still "Items".
- **The mask** (`web/items.js`, shared by the lobby and the proofs) is computed the way
  `mnVSItemSwitchSetItemToggles` computes it:
  - Start from the game's default mask `~0`. Clear `1 << ITKind` for every switch that is off. Shell
    clears both Green (17) and Red (18).
  - While any switch is on, crate, barrel, capsule and egg (bits 0-3) are set.
  - All switches off gives `0`.
  - The ITKind numbers were checked by compiling the real enum, not by counting.
- **Default.** Everything on and the rate at Medium. That gives `0xFFFFFFFF` at rate 3, the same values the
  lobby sent before this change, so ordinary play is unchanged.
- **Every peer gets the same mask.** `config().itemToggles` rides in the host's existing `{t:'config'}` and
  `{t:'start', config}` messages. The relay stores it and rebroadcasts it unchanged in `roster` and `go`.
  Each page passes `go.config.itemToggles` to `StartMatch` (`net.js`). Friends' checkboxes follow the
  roster's config.
- **Engine fix found on the way.** `itMainSearchRandomWeight` drops the results of its recursive calls. On
  the N64 the answer is still in `v0`; under clang it is undefined. In the port, every random item and every
  container drop came out as one kind (Peach's Castle at Very high: only Star Rods). It now returns them
  under `AVOID_UB`, and items are picked by weight again. **This changes how matches play compared with the
  live site: items vary, as in the game.**

## Proofs run (all on the final build `70046cd01b8fd1c6`)

| Command (in `~/sb64/ssb64-extras`) | Result |
|---|---|
| `make -f Makefile.port -j32 objs`, then `web netnode` | exit 0 |
| `node web/tests/extras_proof.mjs` → `net.log` | **23/23 ok, exit 0** (15 min) |
| `node web/tests/extras_lobby_proof.mjs proof/extras/shots` → `lobby.log` | **16/16 ok, exit 0** |
| `node web/tests/net_proof.mjs` (existing) → `regress/net_proof.log` | **16/16 ok, exit 0** (9 min) |
| `node web/tests/lobby_proof.mjs` (existing) → `regress/lobby_proof.log` | **13/13 ok, exit 0** after a test fix (below) |
| IDO matching build check: preprocess the 3 changed `src/` files without `PORT`/`AVOID_UB` and compare with HEAD | identical text for all 3 |

- **`extras_proof.mjs`.** Real relay, one engine process per page, 30 ms lag each way, rollbacks on. Every
  race checks that the pages agree hash for hash, and checks what the sim was handed
  (`PortMatchInfo`: stage, mask, each slot's FTKind and player kind, music). The races:
  - Each of the 3 stages with Metal Mario, Giant DK and polygons, as people and as CPUs.
  - 8 different extra fighters in one match. This covers the audit's concern about the 100-node file
    loader.
  - All 14 extras as human picks.
  - Master Hand refused.
  - Item races of 90 s each:
    - All on at Medium gave items 0, 6, 12, 13 and the castle bumper.
    - Only Hammer gave Hammer and containers.
    - Only Beam Sword gave Beam Sword and containers.
    - Only Maxim Tomato gave Maxim Tomato and containers.
    - All off gave no switchable item.
    - Final Destination gave none.
- **`extras_lobby_proof.mjs`.** Two real Chromium pages through the lobby UI.
  - The 3 stage tiles have pictures. There are 14 extras and no Master Hand.
  - The checklist defaults to all 15 on, at Medium. The friend cannot change it.
  - The host clicks Final Destination, Metal Mario, All off, then Hammer and Maxim Tomato. The friend
    clicks Polygon Kirby, and the friend's checklist mirrors the host's.
  - After START, both sims report stage 16, mask `0xfff0201f`, and slots `13` and `22`. A real key press
    moves Metal Mario, the pages agree on every hash, and there are no page errors.
- **Existing `lobby_proof.mjs`** already failed at HEAD `d8f447242`; I reproduced that in a throwaway
  worktree. The proof uses `selectOption` on selects that the new lobby hides. The fix adds `{ force: true }`,
  and nothing else changed.

## Not proven / needs a person

- **In-browser on the real site.** That needs a deploy. It is left to Ignacio.
  - Merge `extras`, build the page into `web/`, then run `scripts/stage.sh`.
  - The relay needs no change: it passes `config` and `pick` through as they are.
- **By eye only, no emulator.** Metal Mario's chrome looks noisy under SwiftShader. Compare it on a real GPU.
- **Final Destination's moving background** paces itself off player 1's damage. Its driver expects a Master
  Hand. This is cosmetic, not changed, and the same as VS-on-FD cheats on the N64.
- **Polygons show no stock count in Stock matches** (no sprite in the game). A fallback to the 1P polygon
  stock icon (`FTStocksZako`) would mean loading another file into the match heap. Not done.
