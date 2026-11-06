# Changes

Browser port of Super Smash Bros. 64 with rollback netplay, from the matching decomp. Everything is guarded by `#ifdef PORT`, so the N64 build code path stays.

- `port/`: platform shim, stepped scene driver, F3DEX2 and WebGL2 renderer, audio, snapshot, rollback, harness.
- `relay/` (Rust): lobbies, clock, pad bundles, desync detection, serves the page.
- `tools/port/` (Rust): packs the game data from the ROM and the decomp's sources, verified word by word against the ROM.
- `web/`: lobby page, netcode, input, audio, browser tests.
- 8 players, past the original 4. See `proof/changes/8players.md`.
- `scripts/proof_all.sh` runs every proof. `scripts/stage.sh` prepares a deploy folder.

Status per phase is in `proof/changes/`. Design changes from the plan are in section 6 of `PLAN.md`.

## Second pass, 5 Nov 2026: owner's playtest notes

What the owner noticed, and what was found under it. Proofs in `proof/changes2/`, `proof/sweep/`, `proof/lobby/`, `proof/wide/`.

- **Mario's fireball looked see-through.** It was drawn edge-on. Billboards build their rows straight into the RSP's MVP (`gSPMvpRecalc` then `G_MW_MATRIX` movewords), which the interpreter ignored. Both are implemented (`port/gfx/f3dex2.c`). Every camera-facing effect uses this, not only the fireball.
- **Falcon Punch said only "PUNCH".** In netplay a re-simulated frame got back only the sounds its first run started. "Falcon" is on the first frame of the move, which a late input re-simulates, so it was lost. The journal (`port/net/audio_journal.c`) now starts a sound a re-run asks for that the first run did not (a few frames late) and stops one the re-run did not ask for (a wrong guess). Sound handles stay out of every hash, as before. Proof: `web/tests/sound_proof.mjs` (two clients, one 90 ms away with jitter, all 12 fighters in 6 pairs: both hear the same sounds; the old journal fails the same test on 10 sound ids).
- **Rainbow streaks on Dream Land's floor** (found by Fable): `gSPModifyVertex` was a no-op. The flower strips patch their texture coordinates with it. Implemented for `G_MWO_POINT_ST` and `G_MWO_POINT_RGBA`. See `proof/sweep/fable/streaks.md`.
- **A DC offset under all the game's sound, and clipping on loud hits.** The audio microcode's two-pole filter (`A_POLEF`, used by the reverb) used one tap of each coefficient row and the wrong scale, so it could hold a constant forever (-6759 after the music stopped). Rewritten to the RSP's 8-sample form (`port/audio/naspmain.c`). After: about ±20 at rest, a loud voice peaks at ±16k instead of clipping.
- **Four display lists typed as data** (found by Fable): the first 6 commands of Mario's and Luigi's up-B coin list and of Samus's bomb list were declared as 3 extra vertices, 22 commands before Link's boomerang list as 11 extra vertices, and 5 commands inside Kirby's model as a palette. The port converted them by their declared type, so those sprites ran with stale state. Retyped in `src/relocData/` (297, 321, 325, 328, and the pointers in 204, 218, 222). The pack still matches the ROM word for word (`packdata`: 0 bad words, 0 bad chains). The IDO toolchain is not on this machine, so the matching ROM build itself was not run.
- **Renderer bounds:** a vertex load past the 64 slots and more than 7 lights are counted and clamped instead of writing over the interpreter's state. Neither happened in the coverage run.
- **The lobby.** The game's own fighter portraits and stage previews (decoded in the page from the data image, `web/art.js`), its select-screen music, menu cues and the announcer calling a pick (`port/net/lobby_audio.c`, only the audio thread runs). The music goes under the voices when a mic is on in the portal. Sound and wide-screen buttons.
- **Landscape only.** A tall window shows "Turn your screen sideways" instead of a squeezed game.
- **Widescreen**, from Fable's spec (`proof/widescreen-spec.md`): the picture fills 4:3 to 21:9, showing more stage at the sides; the HUD stays in the 4:3 band; the backdrop and full-screen fills stretch. Render only: a 4:3 client and a 21:9 client keep the same hashes (`web/tests/lobby_look.mjs`). Off by the Wide screen button, per screen.
- **The portal hooks** (`window.N64`, from n64's former patch) are in the page itself and do nothing outside the portal.
- **Debug tools, render only and off by default:** `GfxTrace(n)`, `GfxCoverage` and `GfxCoverageDump` (every distinct draw state a match uses), `web/tests/probe.mjs`, `sweep.mjs`, `coverage.mjs`.
- **Build output:** `make -f Makefile.port web WEB=<dir>` builds the page elsewhere. `web/` is what the live site serves; a build there changes the live site.
