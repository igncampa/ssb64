# 8 human players

Added after the plan was written, at the owner's priority. Only the port build changes; the N64 build keeps 4.

## What ran
- `scripts/proof_all.sh`: logs in `proof/`.
- Determinism, rollback and network proofs below all include 8-fighter cases.
- `scripts/proof_bounds.sh`: 15 full matches with every fixed-size array access checked. 0 unexpected.
- `proof/soak8.log`: 9 8-page matches on 3 stages with 30 ms lag each way: 0 desyncs. 8 ran to their end; 1 was still going at the test's 5,000-frame limit.
- `web/tests/lobby8_proof.mjs`: 8 real browser pages through the lobby UI with real key presses. All agree on every hash, a ninth is told the lobby is full.
- Every stage screenshotted with 8 fighters: `proof/screenshots/all_stages_8_players.png`.

## Bugs that only appeared past 4, all fixed
| Bug | What would have happened |
|---|---|
| An attack remembered 4 targets | A wide hit strikes someone twice |
| Placement count array of 5, indexed by player | Stack overwrite at match end |
| Shield colours, emblem colours, respawn halo offsets, camera zoom table | Read past their tables |
| Intro camera tour ran twice as long | Fighters playing in an extreme close-up after GO |
| Sound journal count kept across a re-run of the same frame | Wrong handles after one-frame rollbacks. Also affected 4 players |
| Crowd sound serials in the compared state | False mismatches. Audio bookkeeping, not gameplay |
| Game memory past 16 MB | Display list addresses misread, black screen. Now checked at boot |
| Damage detect arrays widened by mistake | My error: they are per hitbox, not per player. Reverted |

## Design
- Slots 5 to 8 spawn on floor between the authored spawn points. All landed safely on all 9 stages.
- With more than 4 fighters, or anyone in slots 5 to 8, all readouts share one smaller row along the bottom. Otherwise the original layout.
- New tags and pause numbers for 5P to 8P, drawn from the 4P art. New colours: orange, cyan, pink, white.

## Open
- No human has played 8 players.
- Slots 5 to 8 reuse the KO explosion particles of slots 1 to 4, so those colours repeat.
- The announcer has no "Player 5" to "Player 8" lines, so it stays silent for them.
- The icon that flies during a stock steal stays full size in the small row.
