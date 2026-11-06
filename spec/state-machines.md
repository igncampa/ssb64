# Implicit state machines in the ssb64 port: findings and phased spec

Read-only audit of the project's own code at `39b057b2a` (branch `spec/state-machines`): `web/*.js`, `web/index.html`, `relay/src/main.rs`, `port/`, and the `#ifdef PORT` blocks in `src/`. The stock game's own state machines (fighter status, the BGM mailbox in `src/sys/audio.c`, the taskman status) are described only where port code drives or overrules them.

Every line number below was read in this tree. "Observed in code" means the behaviour follows from the lines cited without running anything. "Suspected" means the mechanism is in the code but whether and how often it fires depends on timing, a browser, or a person, and nothing in `proof/` settles it.

## 0. Summary

| ID | Name | Where | Severity | Phase |
|---|---|---|---|---|
| SSB-SM-01 | Page screen and mode | `web/lobby.js` | High: a page can end up with a frozen canvas and no way back to the lobby | 1 |
| SSB-SM-02 | Engine load and AudioContext life cycle | `web/engine.js`, `web/lobby.js` | Medium: a context created without a gesture is never resumed | 1 |
| SSB-SM-03 | NetClient session state and rejoin | `web/net.js`, `relay/src/main.rs` | High: rejoin and resume have unguarded paths that stall a page for good | 2 |
| SSB-SM-04 | Who owns the music (four copies of "what plays") | `web/lobby.js`, `port/net/lobby_audio.c`, `src/sys/audio.c`, `src/mp/mpcollision.c`, `src/ft/ftparam.c` | Medium: BGM reconcile after a rollback can restart item music | 3 |
| SSB-SM-05 | Relay room and race life cycle | `relay/src/main.rs` | High: a desync leaves the room "in progress" forever; one background tab blocks everyone's match end | 2 |
| SSB-SM-06 | Rollback core modes (resim, held finish, dropped slots) | `port/net/rollback.c`, `port/net/audio_journal.c`, `src/sys/taskman.c` | Medium: sudden death's first frames run as re-simulations; a dropped page rolls back every bundle | 3, 4 |
| SSB-SM-07 | Scene driver and "a match is on" held in five places | `port/loop/scene_driver.c`, `port/net/harness.c`, `port/net/rollback.c`, `web/lobby.js`, `web/net.js` | Low: dead states and duplicated end tails; disagreement after a desync | 4 |
| SSB-SM-08 | HUD threads as coroutines | `src/if/ifcommon.c` | Low: already explicit; one-instance rule not asserted | 4 (optional) |
| SSB-SM-10 | Drop to CPU life cycle | relay `clock`, `web/net.js`, `web/lobby.js`, `port/net/rollback.c` | Medium: permanent, invisible to the dropped player, and reported as if play resumed | 2, 4 |
| SSB-SM-11 | Per-match reset paths | `port/net/harness.c`, `port/net/rollback.c`, `port/net/audio_journal.c`, `web/net.js` | Low: the audio journal and a few statics carry over from match to match | 3 |
| SSB-SM-12 | Portal (embed) mode versus standalone | `web/lobby.js` | Medium: two retry timers compound when the relay is down; a long portal name disables START | 1 |
| SSB-SM-15 | Stall banner and banner ownership | `web/lobby.js` | Low: "Your connection stopped" is unreachable; three writers race one element | 1 |

SSB-SM-09, -13 and -14 were merged into -05, the "checked, no finding" list (section 3) and -03 respectively, so the numbering has gaps on purpose.

Where the page, the wasm and the relay disagree today (details in the findings):

| Fact | Page | wasm | Relay | When they disagree |
|---|---|---|---|---|
| A match is on | `loopOn`, `net.state` (lobby.js:228, net.js:25) | `sState` (scene_driver.c:27), `sOn`/`sEnded` (rollback.c:46), `sMatchOn` (harness.c:263) | `race.active` (main.rs:31) | After a desync: page in lobby, wasm PARKED, relay active (SM-05a) |
| Who is host | `hostId` from `roster.host` = min id over every member (main.rs:84) | n/a | `is_host` = min id over online members (main.rs:146) | Host offline during a race (SM-05c) |
| A slot's player kind | roster `pick`, banner on `cpu` | `fp->pkind` Com (rollback.c:143) but `players[].pkind` still Man | `humans[]`, `dropped[]` (main.rs:32, 40) | After a drop (SM-10d) |
| Engine ready | `engineReady`, `eng` (lobby.js:49) | n/a | `Member.engine` free string, "booting" or whatever the page sent (main.rs:117, 151) | Between `welcome` and the page's `engine ready` (SM-02) |
| Which music plays | `lobbyAudio`, `soundOn`, `duck` (lobby.js:102) | `playing` (lobby_audio.c:64), mailbox `sSYAudioCSPlayerStatuses`/`sSYAudioBGMPlayingIDs` (audio.c:1157), `gMPCollisionBGMCurrent` | n/a | After a finite song ends on its own (SM-04a) |

## 1. Findings

### SSB-SM-01. Page screen and mode

**Where.** `web/lobby.js` 33-34 (`err`, `show`), 59-68 (engine load), 72-91 (`join`), 198-226 (`onEvent`), 228-248 (`startLoop`, `endRace`), 250-261 (buttons), 270-282 (start-up branch); `web/index.html` 16 (the `hidden` rules), 86-146 (the four layers: `#stage`, `#turn`, `#lobby`, `#strip`, `#banner`).

**The machine as it exists.** The page has these modes, none of them named:

| Mode | How it is encoded today |
|---|---|
| LOADING | `eng === null`, `engineReady === false`, `#load` text (lobby.js:49, 59-68) |
| JOIN or HOST screen | `show('sJoin')` / `show('sHost')`: `hidden` on three `<section>`s (lobby.js:34); `#lobby` visible |
| CONNECTING | inside `join()` between `new NetClient` and `connect()` resolving (lobby.js:79-84); nothing in the DOM says so |
| ROOM | `show('sRoom')`, `net` set, `net.state === 'lobby'`, `lobbyAudio === true` (lobby.js:85-90) |
| RACING | `$('lobby').hidden = true`, `loopOn === true`, `net.state === 'racing'` (lobby.js:218, 229-230) |
| RACING, DESYNC WAIT | `net.desync !== null`, a 3 s timer to `endRace` (lobby.js:219, net.js:64) |
| RACING, ENDED LOCALLY | `net.finished === true`, "Match over." banner, still waiting for the relay's `over` (lobby.js:223-224, net.js:123) |
| BACK IN ROOM | `endRace()`: `loopOn=false`, `net.state='lobby'`, lobby unhidden, `_Park()`, `lobbySound(true)` (lobby.js:248) |
| LEFT | `net.gaveUp = true`, socket closed, `show(...)` (lobby.js:261); `net` is not nulled |

Transitions live in six places: `join()`, `onEvent` (eight cases), `endRace()`, three button handlers and the start-up branch at 270-282.

**Problems.**

- (a) **Observed in code.** `case 'error'` (lobby.js:217) calls `show(sHost|sJoin)` but never sets `$('lobby').hidden = false` nor `loopOn = false`. If an `error` arrives while racing, which happens on a rejoin after a redeploy (relay sends `build mismatch` before the join, main.rs:99-101, and `NetClient.rejoin` reconnects while racing, net.js:34, 38-41), the user sees a frozen canvas and the stall banner; the error text sits in `#err` inside the hidden lobby. No way back without a reload.
- (b) **Observed in code.** `case 'closed'` shows "Connection lost. Reconnecting…" whenever `net.state === 'racing'` (lobby.js:221), but `NetClient` only rejoins when `!this.finished && !this.gaveUp` (net.js:34). After a local match end (`finished = true`, net.js:123) a socket drop means no reconnect, no `over`, and a banner that promises one. The page stays on the final frame.
- (c) **Observed in code.** `join()` waits `while (!eng)` forever (lobby.js:77). If `Engine.load` rejected (lobby.js:68) the Join button spins with "Still loading the game. Your place is saved once it finishes." and never returns.
- (d) **Observed in code.** `$('leave').onclick` (lobby.js:261) does not call `lobbySound(false)`; `lobbyFrame` keeps running (lobby.js:114-120) and the select-screen music plays on the Join screen. A later `join()` → `lobbySound(true)` is a no-op (`on && !lobbyAudio` is false, lobby.js:107), so the music is never restarted either; it simply never stopped.
- (e) **Suspected, low.** `ws.onclose` of an old socket calls the shared `onEvent`, which reads the module-level `net` (lobby.js:221). After Leave and a quick re-Join, the old socket's close can report "Connection lost." against the new client. Needs a fast hand to hit.
- (f) **Observed in code.** START after a desync sends `config` and `start` (lobby.js:260); the relay ignores `start` while `race.active` (main.rs:161) and the page gives no feedback. See SM-05a for why the race is still active.
- (g) `window.sb.net.state` is read by `web/tests/lobby_proof.mjs:68`; any renaming must keep a compatible getter.

**Proposed explicit form.** One `page.mode` with values `LOADING | LOAD_FAILED | JOIN | HOST | CONNECTING | ROOM | RACING | RACE_DESYNC | RACE_ENDED_WAIT | REFUSED`, one `setMode(next, why)` that is the only writer of `hidden` on `#lobby`, the three sections, `#strip`, and that starts/stops `startLoop` and `lobbySound`. Event mapping:

| Event | From | To |
|---|---|---|
| `Engine.load` resolves / rejects | LOADING | JOIN or HOST (per URL) / LOAD_FAILED (Join disabled, message) |
| Join or New lobby clicked | JOIN, HOST | CONNECTING |
| `connect()` resolves / rejects | CONNECTING | ROOM / JOIN or HOST with `#err` |
| `go` | ROOM | RACING |
| `desync` | RACING | RACE_DESYNC (3 s) → ROOM |
| local `ended` | RACING | RACE_ENDED_WAIT |
| `over` | RACING, RACE_ENDED_WAIT | ROOM |
| `error` (any) | any | REFUSED → JOIN or HOST, lobby unhidden, loop stopped, `_Park()` if a sim was on |
| `closed` and no rejoin will follow (`finished` or `gaveUp`) | RACING, RACE_ENDED_WAIT | ROOM after `_Park()`, with a plain message |
| Leave | ROOM | JOIN or HOST, `lobbySound(false)`, `net = null` |

**What must not change.** Nothing here touches the sim; `startMatch` arguments, `NetTick` cadence and `pumpAudio` stay as they are. The embed hooks (`window.N64`, SM-12) keep their names.

**How to prove it.** Existing: `web/tests/lobby_proof.mjs` (13 checks: join, start, end, rematch), `lobby8_proof.mjs`, `lobby_look.mjs`. New Playwright checks: (1) start a match, restart the relay with a different `BUILD_ID`, assert the page shows the Join screen with the lobby visible and `loopOn` false; (2) Leave, assert `AudioHead()` stops advancing within a second; (3) make `Engine.load` fail (serve a wrong `ssb64.data`), click Join, assert the button is disabled and no pending promise; (4) kill the relay after the local `ended`, assert the page returns to the room with a message, not "Reconnecting…".

**Size and risk.** About 120 lines moved, not added, in `lobby.js`. Page only. Low risk.

### SSB-SM-02. Engine load and AudioContext life cycle

**Where.** `web/engine.js` 7-23 (`load`), 45-53 (`startAudio`), 54-69 (`pumpAudio`); `web/lobby.js` 59-68, 76-77, 87, 105-113 (`lobbySound`), 127-128 (`unlock`), 218 (`go`), 281 (auto-join); `relay/src/main.rs` 117, 151 (`Member.engine`).

**The machine as it exists.**

Engine: NOT_LOADED → LOADING('engine') → LOADING('game data') → READY, or FAILED. Encoded as `eng`, `engineReady`, the `#load` text, and the string the page posts as `{t:'engine', state}` (engine.js:9, 11, 21; lobby.js:59, 67, 87). The relay stores that string unchecked, truncated to 16 characters (main.rs:151), and the page treats anything but `'ready'` as not ready (lobby.js:171, 186).

Audio: NO_CONTEXT → CREATING (`this.ac` set, `this.node` not yet, during `await addModule`, engine.js:47-49) → RUNNING, or SUSPENDED when the browser's autoplay policy refuses (`ac.resume()` is called once at engine.js:51 and never again).

**Problems.**

- (a) **Observed in code; outcome suspected.** `startAudio` returns early whenever `this.ac` exists (engine.js:46). The page creates the context without a user gesture when it auto-joins (`?room=` with a stored name, lobby.js:281 → `join` → `lobbySound(true)` → `startAudio`, lobby.js:109). The `unlock` handlers (lobby.js:127-128) call `startAudio` again on the first pointer or key event, which returns at line 46 without `resume()`. If the browser created the context suspended, nothing in the page ever resumes it, in the lobby or in the match (`go` also calls `startAudio`, lobby.js:218). Whether Chrome resumes a context on its own after a later gesture is not something the code or the proofs settle; `lobby_look.mjs:26-33` checks lobby audio after real clicks, under Playwright's launch flags (`web/tests/lib.mjs:16`), which is not the auto-join path.
- (b) **Observed in code.** `pumpAudio` during CREATING consumes the ring and returns (engine.js:56), so audio produced during `addModule` is dropped. Harmless (a fraction of a second once).
- (c) **Observed in code.** The readiness protocol between page and relay is a free string compared for equality with `'ready'` (lobby.js:171, 186). The relay's own initial value is `"booting"` (main.rs:117); the page never sends that word.
- (d) **Observed in code.** `Engine.load` sends `net?.json({t:'engine', state:m})` while `net` is still null (lobby.js:59), then `join` sends `ready` after connect (lobby.js:87). Works, but the roster can show "loading" for a member whose engine finished before the socket opened only until that second message. In the portal `embed.notReady` papers over the same gap by name (SM-12b).

**Proposed explicit form.** `Engine.audioState ∈ {none, creating, running, suspended}`; `startAudio()` becomes idempotent and always ends with `if (this.ac.state !== 'running') await this.ac.resume()`; `unlock` calls it until `running`, then removes itself. Engine load: `Engine.state ∈ {idle, loading, ready, failed}` with `failed` surfaced to SM-01. Relay: keep the string on the wire (compatible) but the page defines the three legal values in one place and the relay could validate them in a later phase.

**What must not change.** The 32 kHz sample rate, the worklet ring and `AudioConsumed(head - 533)` (engine.js:68) are timing-tuned; leave them. The `{t:'engine'}` message shape stays.

**How to prove it.** New check in `lobby_look.mjs`: open `?room=CODE` with `ssb64.name` preset in `localStorage`, no gesture, assert `eng.ac.state`; click once; assert `running` and `AudioHead()` advancing. Run it with and without `--autoplay-policy=no-user-gesture-required` so the test is honest about the policy.

**Size and risk.** 20 lines in `engine.js`, 10 in `lobby.js`. Low.

### SSB-SM-03. NetClient session state and rejoin

**Where.** `web/net.js` 23-28 (fields), 29-41 (`connect`, `rejoin`), 57-78 (`onMessage`), 79-84 (`begin`), 94-125 (`tick`); `relay/src/main.rs` 104-123 (join or rejoin by token), 153-159 (`resume`), 176-184 (`finished`), 277-279 (`sent` holds 900 frames); `port/net/rollback.c` 104-116 (`NetBundle` accepts only `sTruthTop + 1`).

**The machine as it exists.** `state ∈ {'lobby','racing','done'}` (net.js:25, 66, 82), written from outside by `endRace` (lobby.js:248). Side flags: `finished`, `desync`, `gaveUp` (set only by lobby.js:211, 217, 261), `clockSeen`, `reportedRunning`, `slot`, `id`. Transitions: `go` → `begin()` → racing; `over` → done; `onclose` → rejoin if racing and not finished and not gaveUp (net.js:32-35); `rejoin` → `connect` → `resume from NetTruthTop()+1` (net.js:39).

**Problems.**

- (a) **Observed in code; outcome reasoned.** Resume gap. The relay replays only what is still in `sent`, the last 900 bundles (main.rs:279). A page away longer than 15 s asks for `from = sTruthTop + 1`, gets nothing for those frames, and every later live bundle fails `frame != sTruthTop + 1` (rollback.c:107). The page is now permanently starved: `NetTick` stalls at `sC + sW` (rollback.c:229), the banner says "Waiting for the server…" (SM-15), and no code path ever recovers. The relay does not tell the page what the oldest frame it holds is.
- (b) **Observed in code; outcome reasoned.** Rejoin after the race ended. If the match finished while the page was away, the relay set `race.active = false` and dropped offline members (main.rs:179-180). The page's token is unknown, so it is admitted as a new member in a fresh slot (main.rs:112-117) and `welcome` overwrites `this.slot` and `this.id` while `state === 'racing'` (net.js:61). The `over` was broadcast before it came back, so it never arrives; the page stays in RACING on its last frame. Its `resume` request replays old bundles into a sim that already consumed them (ignored, fine), but nothing moves it to the lobby.
- (c) **Observed in code.** `begin()` has no failure state. `startMatch` → `_StartMatch` can throw (`port_fatal` throws a JS `Error`, `port/shim/os.c:15, 21`). The throw escapes `onMessage` (net.js:65) before `this.state = 'racing'` (net.js:82); the lobby's `onEvent('go')` is never called, the page still shows the room, and the relay has an active race waiting for this member's `running` (30 s, main.rs:243-248) and later `finished` (never). See SM-05b for what that does to everyone else.
- (d) **Observed in code.** `hashes` grows without bound across matches (net.js:118); only proofs read it. `gaveUp` is never cleared on a new `join` because a new `NetClient` is made (lobby.js:79), which is fine, but `endRace` writes `net.state` directly (lobby.js:248): two owners.
- (e) **Observed in code.** `tick()` sends a ping every 250 ms in every state (net.js:96). Each one refreshes `room.touched` (main.rs:129), so a room with any page open is never collected (main.rs:345-351). Feeds SM-05a.
- (f) The lockstep path (`lockstep`, `sendLockstep`, `delay`, net.js:103-107, 126-131) is reachable only from proofs (`net_proof.mjs:54`); the page never sets it. Keep it, but it is a mode the enum should name.

**Proposed explicit form.** `NetClient.phase ∈ {connecting, lobby, racing, ended_local, reconnecting, failed, closed}` with one `setPhase()`; `rejoin` compares the new `welcome` with the remembered `slot`/`id` and goes to `failed('match ended while away')` on a mismatch; `begin()` wraps `startMatch` in try/catch and goes to `failed(reason)`, emitting `{t:'failed', reason}` for the lobby and sending `{t:'leave'}` so the relay does not wait for it. For the resume gap, add a relay reply `{t:'resumed', oldest, newest}` (new message; old pages ignore unknown `t`, net.js:60-67) and let the page go to `failed('too far behind')` when `oldest > from`. Lockstep is a constructor option named in the phase table, not a separate code path.

**What must not change.** The binary pad, hash and bundle formats (net.js:86-92, 116-122, 72-77; main.rs:206-239) and `NetBundle`'s strict ordering (rollback.c:107) stay. `resume` keeps its shape; the reply is additive.

**How to prove it.** Existing: `net_proof.mjs` scenarios "a page that leaves goes CPU", "a whole match to its end"; `lobby_proof.mjs` "a rematch starts and stays in sync". New `net_client.mjs` options: `dropsocket=<frame>,<ms>` (close the socket, reconnect after a delay) with delays of 2 s and 20 s; expect `rejoined` and play on for the first, `failed('too far behind')` for the second, and in both cases the other pages' matches end normally. A `throwongo=1` option that makes `startMatch` throw; expect `failed` on that page and `over` on the others within the grace of SM-05.

**Size and risk.** 60 lines in `net.js`, 15 in the relay. Protocol additive. Medium risk because rejoin is hard to test by hand; the proofs above are the gate.

### SSB-SM-04. Who owns the music

**Where.** `web/lobby.js` 100-128 (`soundOn`, `duck`, `lobbyAudio`, `cue`, `lobbySound`, `syncSound`), 218 (`go`), 248 (`endRace`), 284 (Sound button); `port/net/lobby_audio.c` 62-68 (`LobbyMusic`, static `playing`), 74-80 (`LobbyQuiet`); `src/sys/audio.c` 1157-1197 (the BGM mailbox: statuses 0..3), 1342-1401 (`gPortResim` gates on every BGM writer), 1593-1601 (`port_bgm_reconcile`); `src/mp/mpcollision.c` 4056-4068 (`mpCollisionSetPlayBGM`); `src/ft/ftparam.c` 110-153 (item music); `src/sc/sccommon/scvsbattle.c` 232-240, 518-526 (Final Destination theme), 579-589 (`StopAudio`, `AudioStopped`); `port/net/rollback.c` 197 (reconcile after every rollback); `port/net/snapshot.c` 1-2 (audio objects sit outside the snapshot range).

**The machine as it exists.** Four copies of "what should be playing":

1. Page: `lobbyAudio` (the lobby clock runs), `soundOn` (persisted), `duck` (portal mic). Volume = `!soundOn ? 0 : duck ? 0x1C00 : 0x5800` (lobby.js:104). Note "Sound off" is lobby-only by design (button title, index.html:141); the match's music and effects ignore it.
2. `lobby_audio.c`: `static int playing` (line 64), toggled by `LobbyMusic`, cleared by `LobbyQuiet`.
3. `audio.c` mailbox per song player: status 0 stopped, 1 "stop, then load if id ≥ 0", 2 "start", 3 "playing" (audio.c:1159-1197); `syAudioPlayBGM` writes status 1 and the id (1363-1364), `syAudioStopBGM` writes status 1 and id -1 (1377-1378). When a song ends on its own, status 3 → 0 and the id becomes -1 (1191-1197). This is the game's own explicit machine; the port adds `if (gPortResim) return` to each writer (1345, 1359, 1375, and the two volume setters at 1382-1401).
4. Sim: `gMPCollisionBGMCurrent` / `gMPCollisionBGMDefault` (mpcollision.c:4062, 4068; ftparam.c:116, 153; scvsbattle.c:237, 523). This one is in the snapshot and the hash's neighbourhood; the other three are not.

`port_bgm_reconcile` (audio.c:1596-1600) runs after every rollback (rollback.c:197) and calls `syAudioPlayBGM(0, gMPCollisionBGMCurrent)` whenever the mailbox id differs from the sim's variable.

**Problems.**

- (a) **Suspected (mechanism observed, timing not measured).** Item music restart. Hammer and Star music are finite songs on player 0 (ftparam.c:110-118). The sim restores the stage theme by its own tick count (`ftParamTryUpdateItemMusic`, ftparam.c:121-153, called from ftmain.c:1450 and ftparam.c:565). If the sequence ends in the audio thread before the sim's count runs out, the mailbox id becomes -1 (audio.c:1194-1195) while `gMPCollisionBGMCurrent` still names the item song. A rollback in that window makes `port_bgm_reconcile` start the item song again from the top. If anyone reports music "playing twice" or restarting, this is the first place to look; `PENDING.md:27` only records that nobody has judged sound by ear yet.
- (b) **Observed in code.** `lobby_audio.c`'s `playing` and the page's `lobbyAudio` are both "is lobby music on", with different resets: `LobbyQuiet` clears one (line 78 via `LobbyMusic(0,0)`), `lobbySound(false)` clears the other (lobby.js:112) and calls `LobbyQuiet`; `endRace` sets `lobbyAudio` true again but `LobbyMusic` decides from its own flag (line 65). They agree today only because every path calls both; SM-01d shows one path (Leave) that calls neither.
- (c) **Observed in code.** `scVSBattlePortStopAudio` is called twice on a normal end (scene_driver.c:72 then 85 via `Park`) and `scVSBattlePortAudioStopped` sets the volume to 0x7800 (scvsbattle.c:586), as does `LobbyQuiet` (lobby_audio.c:79). Harmless duplication, but it shows no single owner.
- (d) **Observed in code.** The audio mailbox and `playing` survive `StartMatch`'s boot-image restore because audio objects are linked outside the sentinels (snapshot.c:1-2). That is intended (audio is presentation) but it means a stale `playing = 1` would suppress the lobby theme after a reload-less sequence that skips `LobbyQuiet`. Today no such sequence exists; it is one `if` away.

**Proposed explicit form.** Page: `audioOwner ∈ {none, lobby, match}` set only by `setMode` (SM-01). Port: keep the game's mailbox untouched; add under PORT a `static s32 sPortBgmRequested[SYAUDIO_BGMPLAYERS_NUM]` written by `syAudioPlayBGM` (the id) and `syAudioStopBGM`/`StopBGMAll` (-1), and make `port_bgm_reconcile` compare `gMPCollisionBGMCurrent` with `sPortBgmRequested[0]`, not with `sSYAudioBGMPlayingIDs[0]`. A song that ended on its own then stays ended until the sim asks for something new, exactly as on the N64. Drop `lobby_audio.c`'s `playing` in favour of `sPortBgmRequested[0] == nSYAudioBGMBattleSelect`.

**What must not change.** `gMPCollisionBGMCurrent` and every sim-side BGM call stay bit-identical; the new static lives in `audio.c` under PORT, which is outside the snapshot and the hash. Nothing under IDO changes.

**How to prove it.** Existing: `web/tests/sound_proof.mjs` (two clients, 90 ms and jitter, the same sounds heard) and `lobby_look.mjs:26-33`. New: a headless `run_match` with items forced to Hammer only (`item_toggles` from `web/items.js`) under `RB=K` rollbacks, logging `PortMatchInfo(6)` and the mailbox id each frame; assert the item song is requested once per pickup before and after the change. Hash logs must be identical before and after.

**Size and risk.** 25 lines in `audio.c` under PORT, 10 in `lobby_audio.c`, page changes ride on SM-01. Audio-only. Low risk to determinism, medium value.

### SSB-SM-05. Relay room and race life cycle

**Where.** `relay/src/main.rs` 27 (`Member`), 30-45 (`Race`), 82-85 (`roster`, host), 104-123 (join), 138-143 (disconnect), 146 (`is_host`), 160-161 (`config`, `start` guards), 168-175 (`running` → `t0`), 176-184 (`finished` → `over`), 185-187 (`leave`), 192-204 (`start_race`), 227-236 (hash fan-in and `desync`), 242-293 (`clock`: `t0` fallback at 30 s, frame closing, drop detection), 338-353 (2 ms ticker and 30 s GC).

**The machine as it exists.** Per room: IDLE (`race.active == false`) → STARTED (`active`, `t0 == None`) → CLOCKED (`t0` set by all online `running`, or 30 s) → back to IDLE when every online member sent `finished`. Encoded in `race.active`, `race.t0`, `race.started_at`, `Member.running/finished/tx`. Per slot: `humans[s]`, `dropped[s]`, `newest[s]`, `last_recv[s]`. Host: computed two ways (below).

**Problems.**

- (a) **Observed in code.** A desync never ends the race. `on_bin` type 2 broadcasts `desync` (main.rs:233) and nothing else changes. Pages park and return to the lobby (lobby.js:219, 248) and never send `finished` (`tick` returns at net.js:98 forever, so `NetEnded` is never reached). The room stays `active`: `start` is ignored (main.rs:161), newcomers get `match in progress` (main.rs:115), and because every open page pings (SM-03e) `touched` never ages, so the GC at main.rs:347-351 never fires. The lobby looks normal and START does nothing (SM-01f). The only exit is everyone leaving and a new code.
- (b) **Observed in code; one trigger suspected.** Match end needs every connected member's `finished` (main.rs:178): `dropped` is not consulted. A page that is connected but not ticking blocks `over` for everyone. The trigger that will happen in practice is a background tab: browsers pause `requestAnimationFrame` in hidden tabs, so `startLoop`'s `frame` (lobby.js:232) stops, `NetTick` is never called, the relay drops the slot to CPU after 3 s (main.rs:287) and the match plays on, but at its end the other pages show "Match over." (lobby.js:223) and wait on their final frame until that tab is foregrounded. Same for SM-03c's thrown `begin()`.
- (c) **Observed in code.** Two host definitions. `roster()` names the member with the smallest id among all members (main.rs:84); `is_host()` requires the socket to be open (main.rs:146). They diverge whenever a member is kept offline, which happens only while `race.active` (main.rs:141), including the stuck-after-desync case (a): the pages see a HOST badge on a disconnected member and nobody's START is shown, while the relay would accept `start` from the lowest online id.
- (d) **Observed in code.** `t0` falls back after 30 s with no message to anyone (main.rs:243-248). Until then every page predicts to `C + W` and shows "Waiting for the server…" (SM-15), with no hint that one peer has not reported `running`.
- (e) **Observed in code.** `start` does not check `Member.engine` (main.rs:161); the gate is the page's `updateStart` only (lobby.js:172). Narrow window today because `join()` waits for the engine before opening the socket (lobby.js:77).
- (f) **Observed in code.** `kick` exists (main.rs:162-167) and no page sends it. Not a bug; a mode nobody can reach.

**Proposed explicit form.** In the relay:

```rust
enum RacePhase { Idle, Starting { since: Instant }, Running { t0: Instant }, Ending { since: Instant, reason: EndReason } }
enum EndReason { AllFinished, Desync(u32), Timeout }
```

Transitions, all in one `fn advance(room, now)` called from the 2 ms ticker and after each message:

| From | Event | To | Side effect |
|---|---|---|---|
| Idle | host `start` | Starting | `go` |
| Starting | all online `running`, or 30 s | Running | `t0`; broadcast `{t:'clock'}` (new, informational; old pages ignore it) |
| Running | all online non-dropped `finished` | Ending(AllFinished, 0 s) | — |
| Running | first `finished` + 10 s | Ending(Timeout) | — |
| Running | hash mismatch | Ending(Desync) | `desync` as today |
| Ending | 3 s elapsed (so page `endRace` timers have fired) | Idle | `over`, drop offline members, `roster` |

Host = min id over online members in both `roster()` and `is_host()`. Keep `active` as a derived accessor so `/api/stat` (main.rs:307) is unchanged.

**What must not change.** Frame closing (main.rs:252-280), the bundle format, `GRACE_FRAMES`, `SILENCE_MS` and the drop rule stay bit for bit: they define the truth every sim follows. `go`, `roster`, `cpu`, `desync`, `over`, `welcome`, `error` keep their shapes. New messages are additive.

**How to prove it.** Existing: `net_proof.mjs` all 16, `lobby_proof.mjs` "both pages return to the lobby after the match", "a rematch starts and stays in sync". New `net_proof` scenarios: (1) `flip=<frame>` on one client (`SetFlipFrame` is already exported, harness.c:269, and applies to scripted slot 0 at `frame - 1`, harness.c:103) → expect `desync` on all, then `over` within 5 s, then a successful second `start`; (2) `freezeframe=<frame>` (client keeps the socket but stops calling `tick`) → the others reach `over` within the timeout; (3) host client leaves mid-race → roster `host` equals the lowest online id on every page.

**Size and risk.** About 80 lines in `main.rs`. Wire-compatible. Medium risk only in the end-quorum rule, which is why it is a phase-2 item with the proofs above as the gate and the owner's answer to open question 2.

### SSB-SM-06. Rollback core modes

**Where.** `port/net/rollback.c` 45-53 (counters and flags), 73-82 (`port_net_scene_ready`), 119 (`NetCpu`), 122-127 (`pad_for`), 149-170 (`step`), 173-182 (`port_net_can_end`), 186-198 (`rollback_to`), 201-236 (`NetTick`); `port/net/audio_journal.c` 36-55 (frame begin/end), 57-86 (`cursor`, `record`, `claim`); `src/sys/taskman.c` 1171-1189 (`syTaskmanCommonTaskUpdate`: the held finish), 1127-1133 (`syTaskmanPortForceEnd`); `port/loop/scene_driver.c` 66-79 (`port_force_scene_end`); `relay/src/main.rs` 266-270 (a dropped slot's pad is zeros).

**The machine as it exists.** Per frame `f`: FIRST-TIME (`f > sTop`) or RESIM (`f ≤ sTop`), chosen at rollback.c:230 and passed as `resim` to `step`, which sets `gPortResim` (rollback.c:157) for the audio gates and the journal. The scene's request to end goes through a three-state dance: the game sets `nSYTaskmanStatusLoadScene` (ifcommon.c:3608 → taskman.c:898-901); under PORT `syTaskmanCommonTaskUpdate` asks `port_net_can_end()` and if refused writes the status back to `Default` (taskman.c:1177-1183), while `port_net_can_end` records `sEndFrame = f` (rollback.c:179); `NetTick` stops stepping while `sEndFrame ≥ 0` (rollback.c:228), rolls back and clears it if a correction reaches it (rollback.c:195), and finally forces the end when `sC ≥ sEndFrame` (rollback.c:221-225). Slots: LOCAL (`sLocalMask`), REMOTE, CPU-FROM (`sCpuAt[s]`, rollback.c:119, 135-146).

**Problems.**

- (a) **Observed in code; audible effect suspected.** Sudden death's first frames run as RESIM. `port_force_scene_end` → `port_net_scene_ready(sFrame)` resets `sNow`, `sC`, `sBase` but keeps `sTop` if it is higher (rollback.c:78), and it is higher by the pacing lead whenever a finish was held. So the first `sTop - end_frame` frames of the new scene step with `resim = 1` (rollback.c:230). Consequences: local pads for those frames are not re-captured (rollback.c:154; this is correct, the relay already holds them), and the audio journal treats them as re-runs of the old scene's predicted frames with the same numbers (`sF[i] == f`, audio_journal.c:40): a sudden-death sound whose id matches a predicted old-scene sound at the same frame claims the stale handle (audio_journal.c:77-86 → n_env.c:4999, 5019) and makes no sound; sounds without a match start and are logged as "late" (audio_journal.c:71). BGM is unaffected because `scVSBattleStartSuddenDeath` runs inside `port_force_scene_end`, not inside `step`. Not covered by `sound_proof.mjs` (Time matches of 1 minute that are not ties).
- (b) **Observed in code; cost not measured.** A dropped page rolls back on every bundle while its player holds anything. `pad_for` returns `sLocal` for a local slot on first-time frames (rollback.c:125); the relay sends zeros for a dropped slot (main.rs:270); the compare loop (rollback.c:208-218) then flags a mismatch every frame the player touches a key or stick. No desync (truth wins), but one rollback per bundle on that page for the rest of the match.
- (c) **Observed in code.** `sOn` is never cleared by `Park()` (harness.c:339) or by a desync; only `NetOff()` from `run_match.c:60` clears it. After a desync the parked page keeps `port_net_on() == 1`, which `port_pad_for_frame` (harness.c:101) and `port_net_can_end` (rollback.c:175) branch on. No sim runs in that window today, so no effect; it becomes one if offline play (PENDING "Phase 8 extras") reuses a page that was in a lobby.
- (d) **Observed in code.** The held finish is spread over `taskman.c`, `rollback.c` and `scene_driver.c` with no name. It is correct as far as reading goes: stepping stops at the first held frame, so `port_force_scene_end(e)` runs on memory at frame `e`. It deserves a comment block and a single predicate rather than a change.
- (e) **Observed in code.** `NetInit` does not clear `sHash`/`sReplaced`; every frame that `NetPollHash` emits has been stepped in the current match (rollback.c:243 skips below `sBase + 1`), so this is safe by construction. Worth an assert, not a fix.

**Proposed explicit form.** Name the frame mode: `enum { FRAME_FIRST, FRAME_RESIM }` returned by one `frame_mode(f)`; name the finish: `enum { FINISH_NONE, FINISH_HELD, FINISH_FINAL }` with `sEndFrame` behind it. For (a): add `port_sfx_scene_reset()` (clear `sF[]`, `sN[]`) called from `port_net_scene_ready`, and set `sTop = sNow` there as well (the pads for frames above are already at the relay; re-sending from `NetLocalPacked` is harmless because the relay ignores frames ≤ `newest`, main.rs:219). For (b): in `pad_for`, when `sCpuAt[slot] ≥ 0 && f ≥ sCpuAt[slot]` return the zero pad; this is what truth will be, so the sim is unchanged and the mismatches stop. For (c): `Park()` calls `NetOff()`; `NetInit` turns it back on.

**What must not change.** The compare rule, `R`, `MAX_W`, the snapshot ring, the hash cadence and `port_hash_sim` are the determinism contract (PLAN 3.7); none of the changes above alters a pad the sim sees on any page: (a) only touches the audio journal and `sTop`, (b) returns the value the relay will send anyway, (c) is dead state. Every change needs the hash proofs below to pass unchanged.

**How to prove it.** Existing: `run_match` with `RB=K` (phase 5, byte-compare rollbacks), `net_proof.mjs` "a page that stops goes CPU", `sound_proof.mjs`. New: (1) a `net_proof` Time match forced to a tie (two scripted humans that never attack: `pad` mode with buttons masked, or a 1-minute match with `SetScriptedSlot` off and neutral pads) → reaches sudden death on both pages; compare `SfxLog` between the two and against a lockstep run of the same seed; (2) "a page that stops goes CPU" extended to assert the stopped page's `rollbacks` stat does not grow after the `cpu` event; (3) hash logs of `proof/phase1.log` seeds re-run after the change, identical.

**Size and risk.** 30 lines across `rollback.c` and `audio_journal.c`. (a) is audio-only; (b) touches what the sim sees on one page but is provably equal to truth; both need the proofs before merging. Medium.

### SSB-SM-07. Scene driver and "a match is on" in five places

**Where.** `port/loop/scene_driver.c` 26-29 (`enum { PARKED, MATCH, MATCH_END_WAIT, BOOTED_MATCH_LOAD }`), 34 (`port_in_match`), 56-63 (`port_match_begin`), 66-79 (`port_force_scene_end`), 81-86 (`port_match_park`), 93-129 (`port_step`); `port/net/harness.c` 263, 333, 339 (`sMatchOn`); `port/net/rollback.c` 46, 69-70 (`sOn`, `sEnded`); `web/lobby.js` 228 (`loopOn`); `web/net.js` 25 (`state`).

**The machine as it exists.** `sState`: PARKED → MATCH (`port_match_begin`) → MATCH_END_WAIT (offline only; netplay jumps over it with `goto end_wait`, scene_driver.c:107) → MATCH again (sudden death) or PARKED. `port_force_scene_end` is a second copy of the end tail (scene_driver.c:68-78 against 116-122).

**Problems.**

- (a) **Observed in code.** `BOOTED_MATCH_LOAD` is declared and never assigned (scene_driver.c:26). `sMatchOn` is written at harness.c:333 and 339 and never read. `port_in_match` is declared in `port.h:23` and defined at scene_driver.c:34 and never called. Three dead states or flags that a reader has to rule out.
- (b) **Observed in code.** Two end tails. The offline tail waits for the BGM to stop (scene_driver.c:116, up to 120 frames); the netplay tail is immediate (scene_driver.c:66-79). Both call `scVSBattlePortAudioStopped`, `port_gfx_reset`, `scVSBattlePortCheckSuddenDeath`, `port_event(10|11)`. A change to one is a change the other must mirror by hand.
- (c) **Observed in code.** `Park()` from MATCH (the desync path) calls `syTaskmanPortEnd` and `scVSBattlePortStopAudio` (scene_driver.c:83-85) but not `syTaskmanPortForceEnd`/`gcEjectAll`, so the scene's objects stay allocated until the next `StartMatch` restores the boot image (harness.c:279). Correct today only because every match starts from the image.
- (d) **Observed in code.** The page's `startLoop` keeps calling `net.tick` → `NetTick` after `sEnded` (rollback.c:205 returns 0), and `pumpAudio` keeps draining. That is the intended "wait for `over`" state, but it is not named anywhere (SM-01).

**Proposed explicit form.** `enum PortMatchPhase { PORT_PARKED, PORT_MATCH, PORT_MATCH_END_WAIT, PORT_SUDDEN_DEATH }` (sudden death is a distinct phase so `port_event(10)` and the HUD can read it), one exported `PortMatchPhase()` the page reads for its strip, one `match_end_tail(void)` used by both paths, `sMatchOn`, `port_in_match` and `BOOTED_MATCH_LOAD` removed. `Park()` ejects like a forced end so the parked state is the same whichever path reached it.

**What must not change.** The order of calls within a frame (`sySchedulerPortTic`, `syTaskmanPortStep`, `port_net_frame_end`) and within the end tail is the sim's. Refactor by extraction only; the hash logs decide.

**How to prove it.** `run_match` hash logs for the phase-1 seed set before and after (identical), `net_proof` "a whole match to its end" and "a stock match to its end with a CPU" (same `now` on both pages), `lobby_proof` rematch.

**Size and risk.** 40 lines, mostly deletions. Low risk with the hash proof; no value to players, value to the next reader.

### SSB-SM-08. HUD threads as coroutines

**Where.** `src/if/ifcommon.c` 2173-2200 (`IFPortThread`, `IF_THREAD_BEGIN/SLEEP/END`, announce), 2298-2320 and 2472-2483 (countdown: statics aliased with `#define sobj ...`), 2553-2560 (entry focus), 2671-2683 (entry all), 2713-2775 (sudden death); `src/sys/objhelper.c` 135-139 (`gcSleepCurrentGObjThread` is fatal under PORT); `port/net/snapshot.c` 11, 43-50 (file statics in game objects are inside the snapshot).

**The machine as it exists.** This is PLAN D4 done: each former GObj thread is a `switch (pc)` coroutine with a tick counter. State is in file statics, so it is snapshotted and restored with the sim. The only runtime guard against the old model is the fatal in `gcSleepCurrentGObjThread`.

**Problems.**

- (a) **Observed in code.** "Only one instance of each is ever alive" (comment at ifcommon.c:2177) is not asserted. `ifCommonCountdownMakeInterface` is called from `ifCommonEntryAllThread` (2679) once per scene; nothing stops a second call from sharing `sIFPortCountdown`.
- (b) **Observed in code.** The countdown's locals are made static through `#define sobj sIFCommonPortCountdownSObj` and five more (2305-2310, undone at 2477-2483). It works and is deterministic; it is also the kind of code a future edit breaks silently (any new local named `timer` in that function becomes shared state).

No functional problem found. This finding is here because it is the model the other findings should follow, and because the two items above are cheap.

**Proposed explicit form.** Replace the `#define` aliases with a `struct IFPortCountdownState` held in the one static, and add `if ((t).pc != 0 || (t).wait != 0) port_fatal("HUD coroutine already alive")` to each `*MakeInterface` before it resets the state.

**What must not change.** Sleep arithmetic (`wait > 1` → decrement, resume when it reaches 1, ifcommon.c:2182-2183) reproduces `gcSleepCurrentGObjThread(n)` exactly and is in every hash; do not touch it. IDO build unaffected (all under PORT).

**How to prove it.** Phase-1 same-seed hashes (`proof/phase1.log` seeds) identical.

**Size and risk.** 20 lines. Low. Optional.

### SSB-SM-10. Drop to CPU life cycle

**Where.** `relay/src/main.rs` 282-292 (drop detection: offline, or silent for `SILENCE_MS`), 266-270 (zeros for a dropped slot); `web/net.js` 63 (`cpu` → `_NetCpu` in any state); `web/lobby.js` 220 (`cpu` banner), 221-222 (`closed`, `rejoined` banners); `port/net/rollback.c` 119, 135-146 (`sCpuAt`, `apply_cpu` flips `fp->pkind` only), 215 (a `cpu` on an already simulated frame forces a rollback); `port/net/harness.c` 344-357 (`PortMatchInfo(3)` reports `players[slot].pkind`); `src/if/ifcommon.c` 3153-3160 (announcer reads `fp->pkind`).

**The machine as it exists.** Per slot on the relay: HUMAN → SILENT (no pad for 3 s, or socket closed) → DROPPED, permanent for the race. On every page: `cpu{slot, frame}` → `sCpuAt[slot] = frame + 1` → at that frame the fighter's `fp->pkind` becomes Com and the AI is set up (rollback.c:143). The page shows "`name` dropped. A CPU takes over." for 4 s (lobby.js:220).

**Problems.**

- (a) **Observed in code.** The dropped player's own page gets the same banner with their own name, then on reconnect "Reconnected." (lobby.js:222). Nothing says "you are now a CPU and cannot play again this match". Their keys do nothing (relay sends zeros for the slot) while the page keeps rolling back on each key (SM-06b).
- (b) **Suspected.** A tab in the background for 3 s is dropped (browsers pause `requestAnimationFrame`, so no `NetTick`, so no pads; main.rs:287). The player comes back to a CPU playing their fighter, for the rest of the match. This is the rule PLAN 3.9 chose; the point here is that the page never tells them what happened or why.
- (c) **Observed in code.** `players[slot].pkind` in `gSCManagerBattleState` stays Man after a drop (only `fp->pkind` changes, rollback.c:143). HUD tags, the announcer's defeated line (ifcommon.c:3155 uses `fp->pkind`, so that one is right) and `PortMatchInfo(3)` disagree with the fighter's actual control. Deterministic on every page, so not a desync risk; a reporting inconsistency.
- (d) **Observed in code.** `cpu` is applied in `onMessage` regardless of `state` (net.js:63). A late `cpu` after `endRace` writes `sCpuAt` into a parked core; `NetInit` clears it at the next match (rollback.c:66). Harmless; unguarded.

**Proposed explicit form.** Page: `slotState[8] ∈ {empty, human, cpu_fill, dropped}` built from `go.slots`, `config.cpu` and `cpu` events, owned by one reducer; the banner distinguishes "you" ("You stopped sending input for 3 s. A CPU plays your fighter until the match ends."). Core: keep `sCpuAt` as the single source; (c) is optional and only cosmetic, see Phase 4.

**What must not change.** The 3 s rule, the `cpu` message, `apply_cpu`'s frame and effect. Un-dropping (Com → Man) is a sim change on every page and a protocol change; out of scope unless the owner asks (open question 3).

**How to prove it.** Existing: `net_proof` "a page that stops goes CPU", "eight pages, two drop out". New: assert the stopped client's `events` carry a `you: true` flag (page-side only; the proof reads `events`).

**Size and risk.** 15 lines of page code. Low.

### SSB-SM-11. Per-match reset paths

**Where.** `port/net/harness.c` 272-337 (`StartMatch`: boot restore 279, arena poison 280, pad ring 287-289, `sItemsSeen` 334), 360-375 (`static int reported` never reset); `port/net/rollback.c` 57-67 (`NetInit` resets rings, stats, counters, `sCpuAt`); `port/net/audio_journal.c` 27-31 (`sJ`, `sN`, `sF`, `sLog`, `sLogN`, `sLate`, `sCut` never reset); `web/net.js` 79-84 (`begin` resets five flags, not `hashes`); `relay/src/main.rs` 192-204 (`start_race` resets `running`, `finished`, new seed); `web/lobby.js` 248 (`endRace`).

**The machine as it exists.** A match's state is spread over the sim image (restored wholesale), the rollback core (reset by `NetInit`), port statics outside the image (reset piecemeal or not at all), and page fields. There is no list of what a new match must reset; each file resets what its author remembered.

**Problems.**

- (a) **Observed in code.** The audio journal carries over. Frame numbers restart at 0 in every match, so `port_sfx_frame_begin(f)` finds `sF[i] == f` from the previous match (audio_journal.c:40) and treats the first run of frame `f` as a re-run: the old records are kept (`claimed = 0`), new ones are appended after them, and a real rollback through that frame later calls `port_sfx_stop_if` on handles from the previous match (audio_journal.c:52), which is a no-op unless the 16-bit serial in n_env has wrapped to the same value (n_env.c:5118-5122). `sLog` also fills across matches until `LOG` (16384) and then stops recording, which only affects proofs that read `SfxLog`. Audio-only, deterministic across pages (every page carries the same garbage), low probability; still a missing reset.
- (b) **Observed in code.** `reported` (harness.c:363) means the "first NaN" log fires once per page load, not once per match.
- (c) **Observed in code.** `NetClient.hashes` grows across matches (SM-03d).
- (d) **Observed in code.** Sudden death is a scene change inside a match: `port_net_scene_ready` (rollback.c:73-82) resets `sNow/sC/sBase` and `sEndFrame` but intentionally keeps `sCpuAt` (a dropped player stays CPU) and `sTruthTop`. It also keeps `sTop` (SM-06a). The intent is right; it is nowhere written down.

**Proposed explicit form.** One `port_match_reset(void)` in `harness.c`, called from `StartMatch` before `port_match_begin`, that calls `port_sfx_reset()`, clears `reported`, and is the documented list of "port statics with per-match life". One `port_scene_reset(first)` for the sudden-death case (what `port_net_scene_ready` does today plus the journal reset from SM-06a). Page: `NetClient.begin()` resets `hashes` too.

**What must not change.** The boot-image restore and `NetInit` are already complete for sim state (phase-1 "race after race" proof); do not move anything into or out of the snapshot range.

**How to prove it.** `run_match` with `matches=3` (run_match.c:28, 41) identical to standalone runs (phase 1 D6 test) before and after; `sound_proof` passes; a new assertion in `net_client.mjs` that `SfxLogCount()` after a second match equals the count of a fresh process running only that match.

**Size and risk.** 20 lines. Audio-only. Low.

### SSB-SM-12. Portal (embed) mode versus standalone

**Where.** `web/lobby.js` 28-31 (`embed`, token, `n64` class), 45 (name source), 74-78 (`join` name and messages), 80 (`hk` query), 84 (connect failure: embed retries in 2 s), 169-178 (`updateStart`: `embed.notReady` matched by name), 209-216 (`error` in embed: `fatal` or retry in 3 s), 221 (`closed` in embed: retry in 1 s), 270-279 (portal boot: `show('sRoom')` before any socket, `onRoom`, `report` every 500 ms, auto-join), 289; `web/index.html` 5-7 (`/n64-embed.js` when framed with `?epoch`), 79 (`body.n64` hides the room panel, Leave, Copy); `relay/src/main.rs` 102 (names truncated to 16).

**The machine as it exists.** Fourteen `if (embed)` branches interleave two life cycles: standalone (screens, Leave, Copy, errors shown) and portal (one cart, no screens, errors fatal to the portal, reconnect loops). The portal path shows the room before a socket exists (lobby.js:272) and reports `{engine, playing}` on a timer (lobby.js:278).

**Problems.**

- (a) **Observed in code; growth suspected.** Two independent retry timers. A failed `connect()` rejects (net.js:36) and the lobby schedules `join` in 2 s (lobby.js:84); the same failed socket also fires `close`, so `onEvent({t:'closed'})` runs, `net.gaveUp` is unset, and the lobby schedules another `join` in 1 s (lobby.js:221). Two attempts for one failure, each of which fails the same way while the relay is down. Observed: both schedulings are unconditional in their branch. Suspected: the doubling per round, since it depends on `close` following `error` for a connection that never opened, which the WebSocket specification requires.
- (b) **Observed in code; occurrence suspected.** `embed.notReady` holds names the portal says are still loading; a name is removed only when `roster.some(m => m.name === n)` (lobby.js:171). The relay truncates names to 16 characters (main.rs:102). A portal name longer than that never matches, `others.length > 0` stays true, START stays disabled and the note says "Waiting for X to load." for ever. Whether the portal allows such names is outside this repository.
- (c) **Observed in code.** `error` with `match in progress` in embed mode sets `net.gaveUp = true` and schedules `join` every 3 s (lobby.js:211-213) — the only "wait for the next match" loop — while the standalone page tells the user to try again (lobby.js:217). Reasonable, but it is the portal mode re-implementing a lobby state (WAITING_FOR_NEXT_MATCH) that SM-01 should own.
- (d) **Observed in code.** `report()` derives `playing` from `net?.state === 'racing'` (lobby.js:278) — a third reader of `NetClient.state` (SM-03).

**Proposed explicit form.** A `shell` object chosen once (`shell = embed ? portalShell(embed) : standaloneShell()`) with five hooks: `identity()` (token, name, host key), `onRefused(reason)`, `onDisconnected(mode)`, `onFatal(msg)`, `report(state)`. `setMode` (SM-01) calls the hooks; reconnect scheduling lives in one place with a single pending timer and back-off (1, 2, 4, … 30 s). Match `notReady` by the relay's rule (truncate to 16 before comparing) or, better, ask the portal for the same token it gives the page (`embed.key`, lobby.js:29) and compare tokens.

**What must not change.** `window.N64`'s surface (`key`, `hostKey`, `onRoom`, `report`, `fatal`, `notReady`), the `?epoch` detection, `body.n64` and the `hk` query parameter: the portal is a separate repository (`~/n64`, see memory notes) and this spec does not touch it.

**How to prove it.** No portal proof exists in this repository. New: a Playwright page with a stub `window.N64` injected before `lobby.js` and the relay stopped; count `join` attempts over 20 s (expect ≤ 6 with back-off, not dozens); a stub `notReady` with a 20-character name that then joins with the truncated name; expect START enabled.

**Size and risk.** 60 lines in `lobby.js`. Page only. Needs a run in the real portal afterwards (owner).

### SSB-SM-15. Stall banner and banner ownership

**Where.** `web/lobby.js` 196 (`banner` with its own hide timer), 219-223 (four writers), 239-242 (stall logic), 248 (`endRace` hides); `web/net.js` 96 (`lastPing` is set when a ping is sent).

**The machine as it exists.** `#banner` is one element written by: the stall logic ("Waiting for the server…" / "Your connection stopped."), `desync`, `cpu` (4 s), `closed`, `rejoined` (2 s), `ended` (1.5 s). Each timed write arms its own `setTimeout(hidden = true)`; the stall logic hides it when starvation ends unless `net.desync`.

**Problems.**

- (a) **Observed in code.** The reason text is wrong. `net.rtts.length && performance.now() - net.lastPing < 1000` (lobby.js:241) tests when the page last *sent* a ping (net.js:96), which is always within 250 ms while the loop runs. So once any pong has ever arrived, the banner always says "Waiting for the server…", and "Your connection stopped." is unreachable while the page is alive. The intended test is the age of the last *pong*.
- (b) **Observed in code.** Timed banners race. A `cpu` banner's 4 s timer can hide a `desync` banner shown 1 s later; the stall logic can hide a `rejoined` banner; `endRace` hides whatever is there.

**Proposed explicit form.** `banner = { kind ∈ {none, info, stall, desync}, text, until }` rendered by one function each frame from `startLoop`; `stall` and `desync` have no timer; `info` has one; a higher kind replaces a lower one and a lower one never replaces a higher one. `NetClient` records `lastPong` in the `pong` handler; the stall reason reads it.

**What must not change.** Nothing below the DOM.

**How to prove it.** Existing: none check banner text. New: in `net_client.mjs`-style headless pages the DOM is absent, so a `lobby_proof` step that pauses the relay process (SIGSTOP) for 2 s and asserts the text reads "Your connection stopped." then clears after SIGCONT.

**Size and risk.** 25 lines. Trivial.

## 2. Phases

Order: page-only first, then page and relay (wire-compatible), then audio-only port statics, then sim-adjacent port cleanups that need hash proofs. Each phase ends with `proof/changes/<phase>.md` as the project already does.

### Phase 1. Page: one mode, one banner, one audio owner, one shell

**What.** SM-01 (`page.mode` + `setMode`, the error/closed/leave/load-failed fixes), SM-15 (banner state and `lastPong`), SM-02 (`startAudio` resumes; `Engine.state`), SM-12 (`shell` adapter, single reconnect timer with back-off, `notReady` by truncated name or token).

**Where.** `web/lobby.js`, `web/engine.js` (startAudio, load), `web/net.js` (record `lastPong`; a `phase` getter named `state` kept for `lobby_proof.mjs:68` and `lobby.js:278`).

**Dependencies.** None.

**Acceptance.** `lobby_proof.mjs` 13/13, `lobby8_proof.mjs`, `lobby_look.mjs` unchanged and green; the four new Playwright checks of SM-01, the autoplay check of SM-02, the retry-count and long-name checks of SM-12, the SIGSTOP check of SM-15. `BUILD_ID` changes (page JS is part of the build), so the deploy is a full stage via `scripts/stage.sh`.

**Out of scope.** Any `net.js` protocol behaviour, anything under `port/` or `src/`, the portal repository.

### Phase 2. Session and room life cycle (page `net.js` + relay, wire-compatible)

**What.** SM-05 (`RacePhase` in the relay: desync ends the race, end quorum = online non-dropped with a timeout after the first `finished`, `Ending` grace of 3 s, one host rule), SM-03 (`NetClient.phase`, `begin` try/catch → `failed`, rejoin validates `welcome`, `resumed{oldest,newest}` reply and `too far behind`), SM-10 page part (`slotState`, "you were dropped" wording).

**Where.** `relay/src/main.rs` (`on_text`, `clock` → `advance`, `roster`, `is_host`, `resume` handler), `web/net.js`, `web/lobby.js` (consumes `failed`, `slotState`).

**Dependencies.** Phase 1 (`setMode` is where `failed` and the new banners land).

**Acceptance.** `net_proof.mjs` 16/16 and the three new scenarios of SM-05 plus the two of SM-03; `lobby_proof.mjs` rematch check; a manual check that an old page (previous build) against the new relay still plays, since the only wire change is additive messages (`resumed`, optional `clock`). The relay restart drops matches in progress (PENDING "Operations"); schedule it.

**Out of scope.** Un-dropping a player; spectators; changing `GRACE_FRAMES`/`SILENCE_MS`; anything in the frame-closing loop.

### Phase 3. Port, audio-only statics

**What.** SM-04 (`sPortBgmRequested`, reconcile against the request, retire `lobby_audio.c`'s `playing`), SM-11 (`port_match_reset`, `port_sfx_reset`, `reported`), SM-06a (`port_sfx_scene_reset` at `port_net_scene_ready`; `sTop = sNow` there).

**Where.** `src/sys/audio.c` (PORT block near 1593), `port/net/lobby_audio.c`, `port/net/audio_journal.c`, `port/net/harness.c` (`StartMatch`), `port/net/rollback.c` (`port_net_scene_ready` only).

**Dependencies.** None on phases 1-2; can run in parallel. All code under PORT or in `port/`, so the IDO build is untouched by construction; verify by preprocessing `src/sys/audio.c` without `PORT` before and after (the method used in `proof/extras/README.md`).

**Acceptance.** Hash logs for the phase-1 seed set identical before and after (`run_match`, `RB=K` included); `sound_proof.mjs` green and its negative control still failing; the Hammer-only reconcile check of SM-04; the `matches=3` journal count of SM-11; the sudden-death sound comparison of SM-06. A `web netnode` rebuild changes `BUILD_ID`.

**Out of scope.** Anything that changes a value the sim reads; the mailbox machine in `audio.c` itself.

### Phase 4. Port, sim-adjacent cleanups

**What.** SM-07 (`PortMatchPhase`, one end tail, dead flags removed, `Park()` ejects, `Park()` → `NetOff()` per SM-06c), SM-06b (`pad_for` returns zeros for a slot at or past `sCpuAt`), SM-06d/e (named finish state, asserts), SM-08 (one-instance asserts, struct instead of `#define` aliases), optionally SM-10c (`players[slot].pkind = Com` in `apply_cpu`, cosmetic, every page at the same frame).

**Where.** `port/loop/scene_driver.c`, `port/net/harness.c`, `port/net/rollback.c`, `port/include/port.h`, `src/if/ifcommon.c` (PORT blocks only).

**Dependencies.** Phase 3 (shares `rollback.c` edits). SM-10c only with the owner's yes (it changes the HUD tag of a dropped player from `nP` to CP).

**Acceptance.** Every existing proof: `scripts/proof_all.sh`. Specifically: phase-1 seeds identical (same seed, draw vs no-draw, poison A vs B, race after race), `proof/soak4.log`-style soak with 0 desyncs, `net_proof` "a page that stops goes CPU" with the new rollback-count assertion, `lobby_proof` rematch. For SM-10c, `extras_proof.mjs` reads `PortMatchInfo(3)` and must be updated to expect Com after a drop.

**Out of scope.** The relay; the page beyond reading `PortMatchPhase()` for the stats strip.

### Phase 5 (optional, owner's call). Protocol hardening

Versioned `welcome` (`proto: 1`), the relay validating `engine` against the three legal strings, `cpu` carrying a `reason` (`silent | left`), `kick` wired to a host button or removed. Each is a small wire change; none is needed for the fixes above. Only if the owner wants the relay protocol to be the documented contract rather than the page.

## 3. Checked, no finding

- `web/input.js`: `held` is a set cleared on `blur` (input.js:6); one owner, no modes.
- `web/items.js`: pure functions.
- `web/audio-worklet.js`: a bounded queue (4800 frames) with one writer and one reader; no mode.
- Widescreen (`Engine.wide`, engine.js:6, 28-44; `PortSetCanvas`/`sK`, f3dex2.c:53-65; `wide_vp`/`wide_sc`, gfx.h:28-29): two render-only states derived from the canvas size on every resize; one owner each side; the PENDING "widescreen quirks" (name tags, magnifier, boomerang) are sim/render boundary choices documented in `proof/widescreen-spec.md` section 5, not hidden modes.
- The BGM mailbox in `src/sys/audio.c:1157-1197` is the game's own explicit four-state machine; the port only adds resim gates and the reconcile (SM-04).
- `syTaskmanCheckBreakLoop` and `sSYTaskmanStatus` (taskman.c:898-930) are the game's own; the port's one intervention is the held finish (SM-06d).
- The relay's frame-closing loop (main.rs:252-280) and `NetBundle`/`pad_for`/compare in `rollback.c` are explicit and match PLAN 3.9; no change proposed.
- `src/mp/mpcollision.c` extra spawns, `src/ef/efmanager.c`, `src/if/ifcommon.c` colour tables, `src/gm/gmcamera.c` sim matrix, `src/ft/ftmanager.c` parts save/restore, `src/sys/controller.c` port init: data and pure computations under PORT, no state.
- `web/game.js` (offline debug path): loads its own `Engine`, never touches `NetClient`; `rollback.c`'s `sOn` is 0 there so `port_net_can_end` returns 1 (rollback.c:175). Fine as a separate mode, provided SM-06c's `NetOff` on `Park()` lands before any offline mode is reachable from a lobby page.

## 4. Open questions for the owner

1. **Desync.** Should the relay end the race on its own (SM-05 `Ending(Desync)`, `over` after 3 s), so the lobby is usable again and a rematch can start? The alternative is a host-only "end match" button that sends a new message.
2. **End quorum.** After the first page reports `finished`, how long may the relay wait for the others before sending `over`: 10 s as proposed, or something else? A page that is behind by more than that is already past the rollback window.
3. **Dropped players.** Keep "a drop is permanent for the match" (today, and the rule in PLAN 3.9), with the page saying so plainly? Or do you want a way back in, which is a sim change (Com → Man at an agreed frame) and a protocol change, and would be its own spec?
4. **Portal names.** Can a portal name exceed 16 characters? If yes, SM-12b bites today. Can the portal hand the page the same token it uses for `notReady`, so the match is by token rather than by name?
5. **Music.** Has anyone heard the Hammer or Star music restart, or the stage theme come back late, in a match with rollbacks? That decides whether SM-04a is a fix or a precaution.
6. **Sound button.** It mutes the lobby only (by design today). Should it also mute the match? If yes it is a page-side gain on `pumpAudio`, outside the sim.
7. **Auto-join.** When a link with `?room=` and a remembered name auto-joins, the page cannot start sound until a click. Accept a one-line "click anywhere for sound" note, or require a Join click always (no auto-join)?
8. **Lockstep.** Keep the lockstep path in `net.js` for proofs (as PLAN D7 says), or delete it now that rollback has shipped?
9. **SM-10c.** Change a dropped player's HUD tag to CP when their fighter becomes a CPU? Cosmetic, deterministic, changes what `extras_proof.mjs` expects.
10. **Relay restart.** Phases 2 and 5 need a relay restart, which drops matches in progress (PENDING "Operations"). Bundle it with the `RELAY_SECRET` and `/api/stat` decisions already pending?
