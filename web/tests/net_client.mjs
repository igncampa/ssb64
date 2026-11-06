// One racer as its own process: the real engine and net.js, no window. Prints a JSON result line at the end.
import fs from 'node:fs'; import path from 'node:path'; import { pathToFileURL } from 'node:url';
import { NetClient } from '../net.js';
const a = Object.fromEntries(process.argv.slice(2).map(s => { const [k, v] = s.split('='); return [k, v ?? '1']; }));
const R = path.resolve(new URL('../../build/us/port', import.meta.url).pathname);
const { default: SSB64 } = await import(pathToFileURL(R + '/ssb64_net.mjs'));
const m = await SSB64({ printErr: () => {}, print: () => {} });
const img = fs.readFileSync(R + '/ssb64.data'); const p = m._malloc(img.length); m.HEAPU8.set(img, p); m._PortBoot(p, img.length);
const mode = +(a.pad ?? 2), slotPad = f => { const v = m._ScriptPadPacked(mode, cl.slot, f); return { buttons: v >>> 16, x: (v << 16) >> 24, y: (v << 24) >> 24 }; };
const cl = new NetClient({ m, url: `ws://127.0.0.1:${a.port}/ws/${a.code}?name=${a.name ?? 'bot'}&token=${a.token ?? Math.random()}&build=${a.build ?? ''}`, padFn: slotPad,
  lagUp: +(a.lagup ?? 0), lagDown: +(a.lagdown ?? 0), jitter: +(a.jitter ?? 0), window: +(a.window ?? 18), lockstep: a.lockstep === '1',
  onEvent: e => { if (['ended', 'cpu', 'desync', 'closed', 'error'].includes(e.t)) events.push(e); if (e.t === 'error') finish(e.reason); } });
const events = [];
// What the sim was handed (port/net/harness.c PortMatchInfo): stage, item mask, each slot's FTKind and player kind.
const matchInfo = () => m._PortMatchInfo ? { stage: m._PortMatchInfo(0, 0), mask: m._PortMatchInfo(1, 0) >>> 0, fkind: [...Array(8)].map((_, i) => m._PortMatchInfo(2, i)), pkind: [...Array(8)].map((_, i) => m._PortMatchInfo(3, i)), bgm: m._PortMatchInfo(6, 0) } : null;
let done = false;
const finish = why => {
  if (done) return; done = true;
  const sfx = []; { const n = m._SfxLogCount(), p = m._SfxLog() >> 2; for (let i = 0; i < n; i++) if (!m.HEAP32[p + i * 3 + 2]) sfx.push(m.HEAP32[p + i * 3] + ':' + m.HEAP32[p + i * 3 + 1]); }
  const out = { sfx, name: a.name, slot: cl.slot, why, stats: cl.state === 'lobby' ? null : cl.stats(), hashes: cl.hashes.length, lastHash: cl.hashes.at(-1), hashCsv: cl.hashes.filter(h => h[0] % 300 === 0).map(h => h[0] + ':' + h[1].toString(16)).join(','), desync: cl.desync, events, fps: tickRate, info: matchInfo(), itemsSeen: m._PortItemsSeen ? m._PortItemsSeen() >>> 0 : null };
  fs.writeSync(1, JSON.stringify(out) + '\n'); process.exit(0);
};
if (a.oldsfx) m._SfxJournalOld(1);
await cl.connect();
if (a.fighter !== undefined) cl.json({ t: 'pick', pick: { fighter: +a.fighter, costume: +(a.costume ?? 0) } });
let frames = 0, tickRate = 0, t0 = performance.now(), last = t0, acc = 0;
const stopAt = +(a.stopframe ?? 0), leaveAt = +(a.leaveframe ?? 0), maxFrame = +(a.frames ?? 1200), busy = +(a.busy ?? 0);
const loop = () => {
  const t = performance.now(); acc += Math.min(t - last, 100); last = t;
  while (acc >= 1000 / 60) {
    acc -= 1000 / 60;
    if (busy) { const e = performance.now() + busy; while (performance.now() < e); }
    if (cl.state === 'racing' && stopAt && m._NetNow() >= stopAt) { cl.padFn = () => ({ buttons: 0, x: 0, y: 0 }); cl.send = () => {}; }
    if (cl.state === 'racing' && leaveAt && m._NetNow() >= leaveAt) { finish('left'); }
    cl.tick(0); frames++;
    if (cl.state === 'racing' && cl.m._NetConfirmed() >= maxFrame) return finish('frames');
    if (cl.desync !== null) return finish('desync');
    if (cl.finished) return finish('ended');
  }
  if (t - t0 > +(a.timeout ?? 120000)) return finish('timeout');
  tickRate = Math.round(frames * 1000 / (t - t0));
  setTimeout(loop, 2);
};
loop();
