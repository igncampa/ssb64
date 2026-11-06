# ssb64: pending items

State when work stopped, 4 Nov 2026. The game is live at https://ssb64.guanavibes.com. Every automated proof passes. No group of people has played it yet.

## Decisions for the owner

1. **Host secret.** The relay runs with none, so anyone with the address can make lobbies. Recommended: set `RELAY_SECRET`. Needs a relay restart, which drops matches in progress.
2. **Lobby codes in public.** `/api/stat` lists active codes, and a code is all it takes to join. Recommended: remove them from that page. Needs a relay rebuild and restart.
3. **Game data in public.** The 22 MB `ssb64.data` is built from the owner's ROM, and anyone with the address can download it. Decide whether that is acceptable, or put a login in front of the site.
4. **Phase 8 extras.** Build any of them? See below.
5. **Defaults assumed in the plan, section 7.** CPU players allowed, items on medium, Start pauses everyone. Confirm or change.

## Operations

- **The relay does not survive a reboot.** It was started by hand on the owner's desktop, listening on port 8080. Make it a service that starts by itself. Until then, restart it from the project folder:
  `PORT=8080 SITE=web BUILD_ID=<id from web/build.js> relay/target/release/ssb64-relay`
- The running process still has the old name `sb64-relay`. It works as is. The next restart should use `ssb64-relay`.
- The Cloudflare tunnel and DNS were set up by the owner's side. They forward to port 8080. Do not change them or the port without asking the owner.
- **The data file reaches browsers uncompressed.** About 10 seconds to a lobby on the desktop's connection, longer on slow ones. Turn on compression in front of the relay.
- The relay serves everything under `web/`, including `web/tests/`. Nothing in there is secret, but `scripts/stage.sh` makes a cleaner folder.
- No separate test server exists. Testing a new build means replacing the live one.

## Proofs that need people

- **A real internet match, from players' own locations.** Required by phases 4, 6 and 7. All lag so far was simulated, or both pages were on one desktop.
- **A playtest log** of what people found (phase 7).
- **Sound by ear.** Nobody had judged it when this was written. The clipping found earlier came from a DC offset in the reverb filter, fixed on 5 Nov.
- **Feel of the controls and of rollback** in a real match.
- **A physical gamepad.** Only a simulated one has been tested.

## Checks not done

- **Comparison with the original game.** No emulator on this machine. Plan phase 2 asks for it. Screens were judged by eye only.
- **Every fighter's attack, shield and KO visuals**: a sweep of every fighter's specials, smashes and taunt is in `proof/sweep/after/`, judged by eye from contact sheets. No emulator to compare with.
- **Native Linux build** and its comparison with wasm: dropped, because only wasm ships.

## Known gaps in the game

- **Widescreen quirks** (proof/widescreen-spec.md, section 5): a fighter in the side bands has no name tag; the magnifying glass, when it appears, sits at the 4:3 edge; Link's boomerang vanishes at the 4:3 edge plus a margin (the sim's own test, the same on every client). The camera still frames for 4:3.

- Rumble is not implemented. The plan assumed it would be on where available.
- Fighters 5 to 8 reuse the KO explosion particles of fighters 1 to 4, so those colours repeat.
- The announcer has no lines for players 5 to 8, so it stays silent for them.
- The stock icon that flies during a steal stays full size in the small 8-player row. A stock count above 6 also uses full-size icons there.
- Debug page flags are off inside a real lobby, but the wasm test functions are not locked.

## VS extras, built on branch `extras`, not deployed (6 Nov)

Meta Crystal, Duel Zone, Final Destination; Metal Mario, Giant DK, the Fighting Polygons; the item switch as a
checklist. Also fixes random items always coming out as one kind. Proofs and what is left: `proof/extras/README.md`.
Deploying means merging the branch, building the page into `web/` and staging it.

## State machines, built on branch `state-machines`, not deployed (6 Nov)

The owner's rules from `spec/state-machines.md`: no CPU takes an away player's fighter (it stands on the zero pad and
plays again when they are back), away pages never hold up the end, a desync ends the race. Proofs and what is left:
`proof/state-machines/README.md`. Deploying means building the page into `web/`, staging page and relay together
(the build id now covers the page code) and restarting the relay, which drops matches in progress. The n64 portal
carries a copy of the relay (`~/n64/crates/ssb64-lobby`) and pins the page: it needs the same relay changes.

- **A player who pauses and then goes away** leaves the match paused: only the pausing player can unpause, and
  their pad is now the zero pad. The same was true with the old CPU takeover (a CPU never presses Start).
- **Autoplay in a real Chrome** after an auto-join: proven with an emulated press-first policy only.

## Phase 8 extras, not built, waiting for the owner's go

- Couch play: two people sharing one page. The plan calls this the cheap one.
- Spectators: join and watch without playing.
- Offline play with the game's own menus.
- The Japanese version.

## Parking lot

More room for 8 players by shrinking fighters or stretching stages. Assessment in `PLAN.md`.

## Repository

The code is at https://github.com/igncampa/ssb64 as `origin`. `upstream` is the decomp it came from (`VetriTheRetri/ssb-decomp-re`). The commits contain game screenshots in `proof/`. They do not contain the ROM or the data built from it.

`tools/splat` shows as modified. That is line endings only, and it is safe to ignore.
