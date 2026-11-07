# State machines: what changed, what was proven

Branch `state-machines` (worktree `~/sb64/ssb64-spec`), on top of the spec commit `4fd4b80ee`. Deployed 6 Nov 2026 as build 9c0809ae7ff8d638 (superseded by 47c1ade4, then 49c2190f). Not
pushed. Every proof ran on this machine against private relays on ports 18100-18199, serving the worktree's own
`web/`. The live site and its relay were not touched. Final build id `9c0809ae7ff8d638`, data `940006dec6a536a7`.

## The owner's decisions

1. **No CPU takes over.** An away player's fighter gets the zero pad from the relay for every frame and stands.
   "Away" is: the page said it went to the background (`{t:'away'}` on `visibilitychange`), its socket is gone,
   it sent no pad for 3 s, or its match failed to start. It is back when the relay receives a pad of theirs for a
   frame it has not closed yet; from that frame on their pads count. Everything a page simulates comes from the
   relay's bundles, so this is deterministic by construction. `cpu`, `NetCpu`, `sCpuAt`, `apply_cpu` and the
   CPU banners are deleted.
2. **Away pages do not hold up the end.** `over` goes out when every present racer (online, not away, not
   failed) has finished, or 10 s after the first finish. A desync ends the race: `desync` at once, `over` with
   `why: desync` 2 s later, and the room is idle again.

## Per finding

| Finding | Done | How |
|---|---|---|
| SM-01 page mode | yes | `setMode` in `lobby.js` is the only writer of screens, lobby, strip, loop and lobby sound. A refusal while racing, a relay that is gone, a failed load and a failed start all land on a screen with a message. Old sockets are ignored (`client === net`). |
| SM-02 audio | yes | `startAudio` is idempotent and resumes every time; every press retries until running; the Sound button says "Click for sound" while the browser waits. `Engine.state`. |
| SM-03 client | yes | `NetClient.phase`, one reconnect timer (0.5 to 4 s, 30 s then `gone: lost`), rejoin by race id, `begin` failure → `{t:'failed'}`, `hashes` per match. The >900-frame resume gap is gone: the relay keeps the race (30 min), resumes in batches; the page keeps frames its 512-frame ring cannot take (hidden tabs used to desync after 8.5 s) and catches up flat out. A reloaded page rejoins its match from frame 0. |
| SM-04 music | yes | `gPortBGMRequested`; reconcile against the request; lobby reads it. |
| SM-05 relay room | yes | `relay/src/room.rs`: Idle / Starting / Running / Ending, one host rule, start waits for engines, `clock` message, kick removed. |
| SM-06a sudden death resim | yes | `sTop` resets per scene, journal forgets the old scene, capture from `sCapTop`. |
| SM-06b dropped page rollbacks | moot | No drop to CPU any more. |
| SM-06c Park leaves the core on | yes | `Park` → `NetOff`. |
| SM-06d held finish | comment | Described where it lives; no code change (the spec found it correct). |
| SM-06e hash assert | skipped | Safe by construction (spec); an assert adds nothing a player sees. |
| SM-07 scene driver | yes, part | Dead state and flags removed, one end tail, `Park` ejects. `PortMatchPhase()` not added: nothing reads it. |
| SM-08 HUD coroutines | skipped | Every HUD coroutine is in every hash; a one-instance `port_fatal` could crash a real match if a scene ever legitimately re-makes one. Risk without a player-visible gain. |
| SM-10 drop to CPU | deleted | Replaced by away/back (`away`, `back` messages, banners for "you" and for others). |
| SM-11 per-match resets | yes | `port_match_reset` (journal, NaN report, items). |
| SM-12 portal | yes | One retry timer with back-off; `notReady` matched by the relay's 16-character rule; "match in progress" is gone: a newcomer waits in the room. |
| SM-15 banner | yes | One banner with ranks (info < stall < desync); "Your connection stopped." when pongs stop, "Waiting for the server…" when bundles stop, "Catching up…" while behind. Starvation is "no bundle for 250 ms", not `NetNow - C >= W` (pacing stops a page before W, so the old banner never showed). |
| Phase 5 | part | Engine state validated by the relay (`loading`/`ready`/`failed`), kick removed. No `proto` field: the build id gate already pairs a page with its relay, and now covers the page code. |

Also found and fixed: a pad ring cell from 1024 frames earlier could be used as a pad (frames are tagged now);
a replaced socket's close cleared the new socket (connections are numbered); a relay stall marked everyone
away (a scan more than 1 s late resets silence); a present player whose pads stop repeated their last input for
3 s (after 0.5 s quiet the relay sends the zero pad: `QUIET_MS`); `PortItemsSeen` counted predicted frames, so
pages could disagree on it (it counts confirmed frames now).

## Protocol (page and relay deploy together)

| Message | Change |
|---|---|
| `cpu` | removed |
| `away{slot, frame}`, `back{slot, frame}` | new, relay → pages, informational (the bundles carry the zero pads) |
| `{t:'away'}` | new, page → relay: hidden tab |
| `{t:'failed'}` | new, page → relay: the match could not start here |
| `welcome` | adds `race` (0 when idle) and `racer` |
| `go` | adds `race` |
| `roster` | adds `racing`; `engine` is `loading`/`ready`/`failed`; `host` is the lowest online id |
| `resume{from, race}` | `race` added; a different race means "send me the match from its start" (`go` again) |
| `resumed{ok, from, to}` / `{ok:false, oldest}` | new |
| binary 5 | new: a batch of closed frames `[5][first u32][count u16][n] + count × (mask, n pads)` |
| binary 3 | ping, now binary `[3][0][f64]`, echoed unchanged (was JSON `ping`/`pong`) |
| `clock` | new, informational: the race clock started |
| `over` | adds `why`: `finished`, `desync`, `abandoned` |
| `kick`, `match in progress` | removed |

## Proofs (final build)

Logs in this folder; `run.log` is the run order with the machine load at each start (another agent's mk64
tests ran at the same time, load 4 to 95).

| Suite | Result |
|---|---|
| `cargo test` (relay/src/room.rs) | 18/18 |
| `net_proof.mjs` | 16/16 |
| `away_proof.mjs` (new) | 9/9: hidden tab 6 s stands and plays again; away by silence; a background page does not hold up `over` (over within 1 ms of the end); a failed start holds nobody; desync → over → rematch in sync; socket gone 2 s, 20 s (catches up), 40 s (`gone: lost`, the other plays on); reload mid-match rejoins |
| `modes_proof.mjs` (new, browser) | 9/9 |
| `lobby_proof.mjs` | 13/13 |
| `lobby8_proof.mjs` | 5/5 (first run under load 48: the key-press check failed on one fighter, 21 px; rerun at load 11: pass. It also needed `{force:true}` on hidden selects, broken at HEAD since the extras) |
| `lobby_look.mjs` | 11/11 (needed `>= 12` tiles: the extras row, broken at HEAD) |
| `extras_proof.mjs` | 23/23 |
| `extras_lobby_proof.mjs` | 16/16 |
| `sound_proof.mjs` | 1/1; negative control `old=1` fails as it must (17 ids differ) |
| `bgm_proof.mjs` (new) | 1/1: far page restarts 0 (old rule 2), pages agree |
| Phase-1 hash logs (`hashlogs.sh`) | 57/57 identical to the build before any change: 54 seed runs (9 stages × 6 modes, 12000 frames), 3 matches in one process, `RB=15` rollback byte-compare (0 mismatches), sound count |
| IDO: `src/sys/audio.c` preprocessed without `PORT` | identical to HEAD (the only `src/` file changed) |

## Relay, before and after

`relay/examples/load.rs`: 40 rooms × 8 bots (320 sockets), a pad per frame each, a ping 4 times a second, 20 s.
Lateness is a bundle's arrival against its ideal time. Same machine, interleaved, under load 13 and 32.

| | p50 | p90 | p99 | p99.9 | CPU per 100k bundles delivered |
|---|---|---|---|---|---|
| before (2 ms ticker, one global lock) | 2.07 / 2.13 ms | 3.59 / 3.66 | 5.34 / 5.58 | 8.3 / 8.4 | 8.2 / 8.4 % of a core |
| after (clock thread, room locks) | 1.10 / 1.11 ms | 2.00 / 2.12 | 3.95 / 4.33 | 7.2 / 7.5 | 8.5 / 8.8 % of a core |

Bundles reach pages about 1 ms sooner, and the tail is a quarter shorter. CPU per bundle is the same within the
noise of a loaded machine (the load test's room starts vary, so totals are normalised per bundle). Memory: the
same (about 55 MB for 320 sockets). What the change buys beyond the numbers: no lock shared between rooms, no
copy of a bundle per member, no unbounded queue (a stuck page is cut off at 1024 messages and resumes), messages
over 64 KB refused, and no JSON parse for pings. `bench-loaded.log` is an earlier run at load 40-50.

## Not proven / open

- A real Chrome's autoplay after an auto-join: headless Chromium runs every audio context even with
  `--autoplay-policy=user-gesture-required`, so the check emulates a press-first policy in the page.
- A paused match stays paused if the player who paused goes away (only they can unpause). Same as before.
- The portal (`~/n64`) carries its own copy of the relay and pins the page; it needs these changes.
- No real internet match.
