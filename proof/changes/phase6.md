# Phase 6: rollback online. Done except the real internet match

`web/tests/net_proof.mjs`: 16 scenarios, 0 fails, 0 desyncs (`proof/net.log`). Wrong predictions, lopsided lag, jitter, a really slow page, a page that stops, a page that leaves, whole matches, and the 8-page cases.

Corrections across the 30 matches of `proof/soak4.log`, frames rewound and how often:

| Frames | 3 | 5 | 6 | 7 | 12 | 13 | 14 |
|---|---|---|---|---|---|---|---|
| Times | 16 | 30 | 1,689 | 56,251 | 38 | 97 | 1 |

All lag here is simulated on one machine.

Not done, needs people: one real match over the internet.
