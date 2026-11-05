# Phase 2: picture and sound. Works; human checks open

- All 9 stages render, with 8 fighters, at about 60 fps in headless Chrome: `proof/screenshots/all_stages_8_players.png`.
- Draw on and off identical (phase 1 log).
- Costs: see `proof/budget.log`. 8 fighters at full speed: sim 0.17 ms, draw 2.86 ms. At 4x slowdown: 0.64 ms and 8.74 ms. Draw is software rendering here, so it is an upper limit.
- Fixed late: garbled stock icons, black squares around the off-screen magnifier.

Not done, and needs a person or a reference:
- No emulator comparison. Screens were judged by eye against what the game should look like, not side by side.
- Not every fighter's attack, shield and KO visuals were checked one by one.
- Nobody has judged the sound by ear. Earlier levels measured some clipping.
- Keyboard is proven by test. `web/tests/gamepad_proof.mjs` proves the gamepad code with a simulated pad. A physical controller has never been tried.
- Rainbow streaks on the ground of Dream Land and some other surfaces are unexplained. They may or may not be in the original.
- The native SDL backend was dropped: only wasm ships.
