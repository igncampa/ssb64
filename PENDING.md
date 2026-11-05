# ssb64: pending items

State when work stopped, 4 Nov 2026. The game is live at https://ssb64.guanavibes.com. Every automated proof passes. No group of people has played it yet.

## Decisions for the owner

1. **Host secret.** The relay runs with none, so anyone with the address can make lobbies. Recommended: set `RELAY_SECRET`. Needs a relay restart, which drops matches in progress.
2. **Lobby codes in public.** `/api/stat` lists active codes, and a code is all it takes to join. Recommended: remove them from that page. Needs a relay rebuild and restart.
3. **Game data in public.** The 22 MB `ssb64.data` is built from the owner's ROM, and anyone with the address can download it. Decide whether that is acceptable, or put a login in front of the site.
4. **Where the code lives.** See "Not pushed" below.
5. **Phase 8 extras.** Build any of them? See below.
6. **Defaults assumed in the plan, section 7.** CPU players allowed, items on medium, Start pauses everyone. Confirm or change.

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
- **Sound by ear.** Nobody had judged it when this was written. Earlier measurements showed some clipping.
- **Feel of the controls and of rollback** in a real match.
- **A physical gamepad.** Only a simulated one has been tested.

## Checks not done

- **Comparison with the original game.** No emulator on this machine. Plan phase 2 asks for it. Screens were judged by eye only.
- **Every fighter's attack, shield and KO visuals** were not checked one by one.
- **Rainbow streaks** on some stage floors, such as Dream Land, are unexplained. They may or may not be in the original.
- **Native Linux build** and its comparison with wasm: dropped, because only wasm ships.

## Known gaps in the game

- Rumble is not implemented. The plan assumed it would be on where available.
- Fighters 5 to 8 reuse the KO explosion particles of fighters 1 to 4, so those colours repeat.
- The announcer has no lines for players 5 to 8, so it stays silent for them.
- The stock icon that flies during a steal stays full size in the small 8-player row. A stock count above 6 also uses full-size icons there.
- Debug page flags are off inside a real lobby, but the wasm test functions are not locked.

## Phase 8 extras, not built, waiting for the owner's go

- Couch play: two people sharing one page. The plan calls this the cheap one.
- Spectators: join and watch without playing.
- Offline play with the game's own menus.
- The Japanese version.

## Parking lot

More room for 8 players by shrinking fighters or stretching stages. Assessment in `PLAN.md`.

## Not pushed

The work is on the local `main` branch of this folder. It could not be pushed, because `origin` is the upstream decomp repository (`VetriTheRetri/ssb-decomp-re`) and this machine has no access to push there. To publish it, create a repository of your own, keep it private if you prefer, and run:

```
git remote add mine <your repository address>
git push mine main
```

The commits contain game screenshots in `proof/`. They do not contain the ROM or the data built from it.

`tools/splat` shows as modified. That is line endings only, and it is safe to ignore.
