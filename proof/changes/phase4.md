# Phase 4: relay, lobby, online play. Done except the real internet match

- `web/tests/lobby_proof.mjs`: 13 checks, 0 fails. Wrong secret and unknown lobby give plain messages, real keys move fighters, match ends on the same frame, back to the lobby, rematch stays in sync.
- `proof/soak4.log`: 30 4-page matches on 3 stages with 30 ms lag each way: 0 desyncs. Most ran to their end; up to 3 were still going at the test's 5,000-frame limit, which sudden death can pass.
- `net_proof.mjs` lockstep scenario passes. Rollback replaced it as the default, as planned.
- `scripts/stage.sh` checks that wasm, data, page and relay belong together.

Not done, needs people:
- One real match across the internet.
- The separate scratch stack on other ports: not built, since nothing is deployed yet.
