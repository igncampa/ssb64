// One racer as its own process: the real engine and net.js, no window. Prints a JSON result line at the end.
// Options (key=value):
//   port code name token build pad lagup lagdown jitter window lockstep frames timeout busy fighter costume
//   stopframe=F         stop sending pads at F (the socket stays open, the page keeps running)
//   leaveframe=F        leave for good at F
//   pause=F,MS[,away]   stop ticking at F for MS, as a hidden tab does (away=1: tell the relay first)
//   dropsocket=F,MS     the socket drops at F and cannot come back for MS
//   reload=F,MS         at F the page is gone; MS later a fresh client with the same token joins
//   desyncat=F          from F this page simulates slot 0 from a script: its sim leaves the others
//   throwongo=1         starting the match throws
//   waitover=1          after the local end, wait for the relay's `over`
//   races=N             stay in the room for N races (each started by the proof)
import fs from 'node:fs'; import path from 'node:path'; import { pathToFileURL } from 'node:url';
import { NetClient } from '../net.js';
const a = Object.fromEntries(process.argv.slice(2).map(s => { const [k, v] = s.split('='); return [k, v ?? '1']; }));
const R = path.resolve(new URL('../../build/us/port', import.meta.url).pathname);
const { default: SSB64 } = await import(pathToFileURL(R + '/ssb64_net.mjs'));
const m = await SSB64({ printErr: () => {}, print: () => {} });
const img = fs.readFileSync(R + '/ssb64.data'); const p = m._malloc(img.length); m.HEAPU8.set(img, p); m._PortBoot(p, img.length);
if (a.oldsfx) m._SfxJournalOld(1);
if (a.throwongo) { m._StartMatch = () => { throw new Error('test: StartMatch threw'); }; }
const mode = +(a.pad ?? 2);
const token = a.token ?? String(Math.random());
const url = `ws://127.0.0.1:${a.port}/ws/${a.code}?name=${a.name ?? 'bot'}&token=${token}&build=${a.build ?? ''}`;
const events = [];
const T0 = performance.now(), at = () => Math.round(performance.now() - T0);
const races = [];
let cl = null;
const make = () => {
  const c = new NetClient({ m, url, padFn: f => { const v = m._ScriptPadPacked(mode, c.slot, f); return { buttons: v >>> 16, x: (v << 16) >> 24, y: (v << 24) >> 24 }; },
    lagUp: +(a.lagup ?? 0), lagDown: +(a.lagdown ?? 0), jitter: +(a.jitter ?? 0), window: +(a.window ?? 18), lockstep: a.lockstep === '1',
    onEvent: e => {
      if (c !== cl) return;
      if (['ended', 'away', 'back', 'desync', 'closed', 'error', 'over', 'failed', 'gone', 'rejoined', 'resumed', 'go', 'clock'].includes(e.t)) events.push({ t: e.t, slot: e.slot, me: e.me, why: e.why, frame: e.frame, ok: e.ok, at: at() });
      if (e.t === 'error') finish(e.reason);
      if (e.t === 'gone') finish('gone:' + e.why);
      if (e.t === 'over') onOver(e);
      if (e.t === 'go') m._SetScriptedSlot(0, 0);
      if (e.t === 'away' && e.slot === track) awayFrame = e.frame;
      if (e.t === 'back' && e.slot === track && awayFrame !== null) backSeen = true;
    } });
  return c;
};
cl = make();
// What the sim was handed (port/net/harness.c PortMatchInfo): stage, item mask, each slot's FTKind and player kind.
const matchInfo = () => m._PortMatchInfo ? { stage: m._PortMatchInfo(0, 0), mask: m._PortMatchInfo(1, 0) >>> 0, fkind: [...Array(8)].map((_, i) => m._PortMatchInfo(2, i)), pkind: [...Array(8)].map((_, i) => m._PortMatchInfo(3, i)), bgm: m._PortMatchInfo(6, 0) } : null;
const csv = hs => hs.filter(h => h[0] % 300 === 0).map(h => h[0] + ':' + h[1].toString(16)).join(',');
let done = false, endedAt = 0, statsAtEnd = null, infoAtEnd = null;
const finish = why => {
  if (done) return; done = true;
  const sfx = []; { const n = m._SfxLogCount(), q = m._SfxLog() >> 2; for (let i = 0; i < n; i++) if (!m.HEAP32[q + i * 3 + 2]) sfx.push(m.HEAP32[q + i * 3] + ':' + m.HEAP32[q + i * 3 + 1]); }
  const stats = statsAtEnd ?? (cl.race ? cl.stats() : null);
  const out = { sfx, name: a.name, slot: cl.slot, why, stats, hashes: cl.hashes.length, lastHash: cl.hashes.at(-1), hashCsv: csv(cl.hashes), allHashes: a.allhashes ? cl.hashes.map(h => h.join(':')).join(',') : undefined,
    desync: cl.desync, events, fps: tickRate, info: infoAtEnd ?? matchInfo(), itemsSeen: m._PortItemsSeen ? m._PortItemsSeen() >>> 0 : null, races, idle, overAfterEnd: endedAt ? overAt - endedAt : null };
  fs.writeSync(1, JSON.stringify(out) + '\n'); process.exit(0);
};
let overAt = 0;
const wantRaces = +(a.races ?? 1);
function onOver(e) {
  overAt = at();
  races.push({ why: e.why, now: statsAtEnd?.now ?? m._NetNow(), hashCsv: csv(cl.hashes), desync: cl.desync, ended: !!endedAt });
  if (races.length >= wantRaces) finish(endedAt ? 'ended' : 'over:' + e.why);
  endedAt = 0; statsAtEnd = null;
}
await cl.connect();
if (a.fighter !== undefined) cl.json({ t: 'pick', pick: { fighter: +a.fighter, costume: +(a.costume ?? 0) } });
let frames = 0, tickRate = 0, t0 = performance.now(), last = t0, acc = 0;
const opt = k => (a[k] ?? '').split(',').filter(Boolean).map(Number);
const stopAt = +(a.stopframe ?? 0), leaveAt = +(a.leaveframe ?? 0), maxFrame = +(a.frames ?? 1200), busy = +(a.busy ?? 0);
const [pauseF, pauseMs, pauseAway] = opt('pause'), [dropF, dropMs] = opt('dropsocket'), [reloadF, reloadMs] = opt('reload'), desyncAt = +(a.desyncat ?? 0);
const track = a.track === undefined ? -1 : +a.track, idle = { away: { n: 0, moved: 0 }, here: { n: 0, moved: 0 }, back: { n: 0, moved: 0 } }; let awayFrame = null, backSeen = false;
let pausedUntil = 0, paused = false, dropped = false, reloaded = false, desynced = false;
const racing = () => cl && cl.state === 'racing';
const loop = () => {
  const t = performance.now(); acc += Math.min(t - last, 100); last = t;
  if (pausedUntil && t < pausedUntil) { acc = 0; setTimeout(loop, 5); return; }
  if (pausedUntil) { pausedUntil = 0; last = performance.now(); }
  while (acc >= 1000 / 60) {
    acc -= 1000 / 60;
    if (busy) { const e = performance.now() + busy; while (performance.now() < e); }
    const now = m._NetNow();
    if (racing() && stopAt && now >= stopAt) { cl.padFn = () => ({ buttons: 0, x: 0, y: 0 }); cl.send = () => {}; }
    if (racing() && leaveAt && now >= leaveAt) { cl.close('left'); return finish('left'); }
    if (racing() && pauseF && !paused && now >= pauseF) { paused = true; if (pauseAway) cl.hidden(true); pausedUntil = performance.now() + pauseMs; events.push({ t: 'paused', frame: now, at: at() }); setTimeout(loop, 5); return; }
    if (racing() && dropF && !dropped && now >= dropF) {
      dropped = true; const good = cl.url; cl.url = 'ws://127.0.0.1:1/'; setTimeout(() => { cl.url = good; }, dropMs);
      events.push({ t: 'dropped', frame: now, at: at() }); cl.ws.close();
    }
    if (racing() && reloadF && !reloaded && now >= reloadF) {
      reloaded = true; events.push({ t: 'reload', frame: now, at: at() });
      const old = cl; cl = null; old.gaveUp = true; old.ws.close();
      setTimeout(async () => { cl = make(); await cl.connect(); }, reloadMs);
      setTimeout(loop, 5); return;
    }
    if (racing() && desyncAt && !desynced && now >= desyncAt) { desynced = true; m._SetScriptedSlot(0, 1); }
    if (!cl) break;
    cl.tick(0); frames++;
    if (track >= 0 && racing() && m._NetNow() > 0) { const v = m._PortMatchInfo(7, track), away = cl.away.has(track) && m._NetConfirmed() > (awayFrame ?? 1e9) + 30; const k = away ? 'away' : backSeen ? 'back' : 'here'; idle[k].n++; if (v !== 0) idle[k].moved++; }
    if (racing() && cl.m._NetConfirmed() >= maxFrame && !a.waitover) return finish('frames');
    if (cl.desync !== null && !a.waitover && wantRaces === 1) return finish('desync');
    if (cl.finished && !endedAt) {
      endedAt = at(); statsAtEnd = cl.stats(); infoAtEnd = matchInfo();
      if (!a.waitover && wantRaces === 1) return finish('ended');
    }
  }
  if (t - t0 > +(a.timeout ?? 120000)) return finish('timeout');
  tickRate = Math.round(frames * 1000 / (t - t0));
  setTimeout(loop, 2);
};
loop();
