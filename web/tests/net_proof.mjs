// Phase 4 and 6 proof: real relay, real engines in separate processes, real sockets, lag in both directions.
import { spawn } from 'node:child_process'; import path from 'node:path'; import fs from 'node:fs';
const root = path.resolve(new URL('../..', import.meta.url).pathname);
const relayBin = root + '/relay/target/release/sb64-relay';
let fails = 0, port = 18600 + Math.floor(Math.random() * 500);
const startRelay = (env = {}) => new Promise(res => {
  const own = ++port; // races may run in parallel, each with its own relay
  const p = spawn(relayBin, [], { env: { ...process.env, PORT: String(own), SITE: root + '/web', RELAY_SECRET: '', RELAY_TEST: '1', ...env }, stdio: ['ignore', 'pipe', 'inherit'] });
  p.stdout.once('data', () => res({ p, port: own }));
});
const client = (args) => new Promise(res => {
  const c = spawn('node', [root + '/web/tests/net_client.mjs', ...Object.entries(args).map(([k, v]) => `${k}=${v}`)], { stdio: ['ignore', 'pipe', 'inherit'] });
  let out = ''; c.stdout.on('data', d => out += d); c.on('close', () => { try { res(JSON.parse(out.trim().split('\n').pop())); } catch { res({ why: 'crash', out: out.slice(0, 300) }); } });
});
const startCfg = (o = {}) => ({ fighters: [0, 1, 2, 3], stage: 0, rules: 1, time: 3, stocks: 2, itemRate: 3, ...o });
async function race(name, { n = 2, frames = 1800, per = () => ({}), cfg = {}, env = {}, expect = () => true, ninth = false }) {
  const { p, port: prt } = await startRelay(env);
  const { code } = await (await fetch(`http://127.0.0.1:${prt}/api/lobby`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{}' })).json();
  const runs = [];
  for (let i = 0; i < n; i++) { runs.push(client({ port: prt, code, name: 'c' + i, frames, ...per(i) })); await new Promise(r => setTimeout(r, 900)); }
  let refused = true;
  if (ninth) { const x = await client({ port: prt, code, name: 'c8', frames: 10, timeout: 8000 }); refused = x.why === 'lobby full'; console.log('     ninth page: ' + x.why); }
  await fetch(`http://127.0.0.1:${prt}/api/start/${code}`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ config: startCfg(cfg) }) });
  const res = await Promise.all(runs);
  const stat = await (await fetch(`http://127.0.0.1:${prt}/api/stat`)).json();
  p.kill();
  // every page that kept racing must have computed the same hashes
  const alive = res.filter(r => r.why === 'frames' || r.why === 'ended');
  const seqs = alive.map(r => r.hashCsv);
  let same = seqs.length > 0;
  const common = Math.min(...alive.map(r => r.hashCsv.split(',').length));
  for (const s of seqs) if (s.split(',').slice(0, common).join() !== seqs[0].split(',').slice(0, common).join()) same = false;
  const desync = res.some(r => r.desync !== null && r.desync !== undefined);
  const ok = same && !desync && refused && expect(res, stat);
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}`);
  for (const r of res) console.log(`     ${r.name} slot ${r.slot} ${r.why} frame ${r.stats?.now} conf ${r.stats?.confirmed} rtt ${r.stats?.rtt} rb ${r.stats?.rollbacks} resim ${r.stats?.resim} depth ${r.stats?.maxDepth} repl ${r.stats?.replacedBundles} stalls ${r.stats?.stalls} hashes ${r.hashes} desync ${r.desync} ev ${(r.events || []).map(e => e.t + (e.slot ?? '')).join(',')} ${r.out ?? ''}`);
  { const h = {}; for (const r of res) (r.stats?.corrections ?? []).forEach((c, d) => { if (c) h[d] = (h[d] ?? 0) + c; }); console.log('     corrections by frames rewound ' + JSON.stringify(h)); }
  if (!ok) { fails++; console.log('     fps', res.map(r => r.fps).join(' ')); if (!same) console.log('     hashes differ'); }
  return res;
}
// Many full matches: the relay rolls a new seed for each, on three stages, every page must agree to the last hash.
async function soak(n, seeds, parallel, extra = {}) {
  const jobs = [];
  for (const stage of [0, 4, 6]) for (let i = 0; i < seeds; i++) jobs.push({ stage, i });
  for (let at = 0; at < jobs.length; at += parallel) {
    await Promise.all(jobs.slice(at, at + parallel).map(j => race(`${n} pages, full match, stage ${j.stage}, run ${j.i + 1}`, { n, frames: 5000, cfg: { stage: j.stage, time: 1, ...extra }, per: () => ({ pad: 2, lagup: 30, lagdown: 30, timeout: 200000 }) })));
  }
}
const only = process.argv[2];
if (only === 'soak4' || only === 'soak') await soak(4, 10, 5);
if (only === 'soak8' || only === 'soak') await soak(8, 3, 2);
if (only?.startsWith('soak')) { console.log('fails=' + fails); process.exit(fails ? 1 : 0); }
const T = (name, o) => (!only || name.includes(only)) ? race(name, o) : null;
await T('lockstep two pages', { n: 2, frames: 1200, per: () => ({ lockstep: 1, pad: 1 }) });
await T('rollback two pages, no lag', { n: 2, frames: 1800, per: () => ({ pad: 1 }) });
await T('rollback four pages, 40 ms each way', { n: 4, frames: 1800, per: () => ({ pad: 2, lagup: 40, lagdown: 40 }), expect: r => r.some(x => x.stats.rollbacks > 0) });
await T('rollback eight pages, 40 ms each way', { n: 8, frames: 1800, per: () => ({ pad: 2, lagup: 40, lagdown: 40 }), expect: r => r.length === 8 && new Set(r.map(x => x.slot)).size === 8 && r.some(x => x.stats.rollbacks > 0) });
await T('eight pages, mixed lag 5 to 120 ms and jitter', { n: 8, frames: 1800, per: i => ({ pad: 2, lagup: 5 + i * 16, lagdown: 5 + i * 16, jitter: 20 }), expect: r => r.length === 8 });
await T('eight pages, two drop out and go CPU', { n: 8, frames: 1500, env: { SILENCE_MS: '2000' }, per: i => ({ pad: 2, stopframe: i === 6 ? 400 : 0, leaveframe: i === 7 ? 500 : 0 }), expect: r => r[0].events.filter(e => e.t === 'cpu').length === 2 });
await T('a ninth page is refused', { n: 8, frames: 300, per: () => ({ pad: 1 }), ninth: true });
await T('five pages and CPUs up to eight fighters', { n: 5, frames: 1800, cfg: { cpu: [1, 1, 1, 1, 1, 1, 1, 1], levels: [9, 9, 9, 9, 9, 9, 9, 9] }, per: () => ({ pad: 2, lagup: 20, lagdown: 20 }) });
await T('rollback lopsided lag 100 and 10 ms, wrong predictions', { n: 2, frames: 1800, per: i => ({ pad: 1, lagup: i ? 100 : 10, lagdown: i ? 100 : 10 }), expect: r => r.some(x => x.stats.rollbacks > 20) });
await T('jitter 30 ms', { n: 3, frames: 1800, per: () => ({ pad: 1, lagup: 20, lagdown: 20, jitter: 30 }) });
await T('a very laggy page does not hold the others', { n: 2, frames: 1800, per: i => ({ pad: 2, lagup: i ? 180 : 5, lagdown: i ? 180 : 5 }), expect: r => r[0].stats.stalls < 30 });
await T('a slow page costs the others nothing', { n: 2, frames: 1500, per: i => ({ pad: 2, busy: i ? 30 : 0, timeout: 90000 }), expect: r => r[0].stats.stalls < 30 && r[1].fps < 45 });
await T('a page that stops goes CPU, the others play on', { n: 3, frames: 1500, env: { SILENCE_MS: '2000' }, per: i => ({ pad: 2, stopframe: i === 2 ? 400 : 0 }), expect: r => r[0].events.some(e => e.t === 'cpu') });
await T('a page that leaves goes CPU', { n: 3, frames: 1500, per: i => ({ pad: 2, leaveframe: i === 2 ? 400 : 0 }), expect: r => r[0].events.some(e => e.t === 'cpu') });
await T('a whole match to its end', { n: 2, frames: 99999, cfg: { time: 1 }, per: () => ({ pad: 2, lagup: 30, lagdown: 30 }), expect: r => r.every(x => x.why === 'ended') && r[0].stats.now === r[1].stats.now });
await T('a stock match to its end with a CPU', { n: 2, frames: 99999, cfg: { rules: 2, stocks: 0, time: 100, cpu: [0, 0, 1, 0], levels: [5, 5, 9, 5] }, per: () => ({ pad: 2, lagup: 20, lagdown: 20, timeout: 200000 }), expect: r => r.every(x => x.why === 'ended') });
console.log('fails=' + fails);
process.exit(fails ? 1 : 0);
