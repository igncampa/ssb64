# Changes

Browser port of Super Smash Bros. 64 with rollback netplay, from the matching decomp. Everything is guarded by `#ifdef PORT`, so the N64 build code path stays.

- `port/`: platform shim, stepped scene driver, F3DEX2 and WebGL2 renderer, audio, snapshot, rollback, harness.
- `relay/` (Rust): lobbies, clock, pad bundles, desync detection, serves the page.
- `tools/port/` (Rust): packs the game data from the ROM and the decomp's sources, verified word by word against the ROM.
- `web/`: lobby page, netcode, input, audio, browser tests.
- 8 players, past the original 4. See `proof/changes/8players.md`.
- `scripts/proof_all.sh` runs every proof. `scripts/stage.sh` prepares a deploy folder.

Status per phase is in `proof/changes/`. Design changes from the plan are in section 6 of `PLAN.md`.
