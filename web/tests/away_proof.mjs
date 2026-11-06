// The owner's rules, proven with real engines on a real relay (net_client.mjs, one process per page):
// a player who is away is away (their fighter stands on the zero pad, nobody plays it), comes back and
// plays again, and every page keeps the same hashes; an away, hidden or failed page does not hold up the
// end of the match; a desync ends the race and the room starts again; a page whose socket is gone for
// 20 s catches up, one gone for longer gives up with a message; a page reloaded mid-match rejoins it.
//   node web/tests/away_proof.mjs [filter]
import { spawn } from 'node:child_process'; import path from 'node:path';
const root = path.resolve(new URL('../..', import.meta.url).pathname);
const relayBin = root + '/relay/target/release/ssb64-relay';
let fails = 0, port = (+process.env.PORT_BASE || 18100) + Math.floor(Math.random() * 20);
const check = (name, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${extra ? '  ' + extra : ''}`); if (!ok) fails++; };
const startRelay = (env = {}) => new Promise(res => {
  const own = ++port;
  const p = spawn(relayBin, [], { env: { ...process.env, PORT: String(own), SITE: root + '/web', RELAY_SECRET: '', RELAY_TEST: '1', ...env }, stdio: ['ignore', 'pipe', 'inherit'] });
  p.stdout.once('data', () => res({ p, port: own }));
});
const client = args => new Promise(res => {
  const c = spawn('node', [root + '/web/tests/net_client.mjs', ...Object.entries(args).map(([k, v]) => `${k}=${v}`)], { stdio: ['ignore', 'pipe', 'inherit'] });
  let out = ''; c.stdout.on('data', d => out += d); c.on('close', () => { try { res(JSON.parse(out.trim().split('\n').pop())); } catch { res({ why: 'crash', out: out.slice(0, 300) }); } });
});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const cfg = o => ({ fighters: [0, 1, 2, 3], stage: 0, rules: 1, time: 3, stocks: 2, itemRate: 3, ...o });
async function race({ n = 2, per = () => ({}), config = {}, env = {}, starts = 1 }) {
  const { p, port: prt } = await startRelay(env);
  const api = (u, b) => fetch(`http://127.0.0.1:${prt}${u}`, b ? { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(b) } : undefined).then(r => r.json().catch(() => null));
  const { code } = await api('/api/lobby', {});
  const runs = [];
  for (let i = 0; i < n; i++) { runs.push(client({ port: prt, code, name: 'c' + i, token: 'tok' + i, ...per(i) })); await sleep(900); }
  await api(`/api/start/${code}`, { config: cfg(config) });
  for (let k = 1; k < starts; k++) {
    // the next race once the room is idle again
    for (;;) { await sleep(500); const s = await api('/api/stat'); if (!s.rooms.find(r => r.code === code)?.racing) break; }
    await sleep(1500);
    await api(`/api/start/${code}`, { config: cfg(config) });
  }
  const res = await Promise.all(runs);
  p.kill();
  return res;
}
// The pages that kept playing agree hash for hash over what they all reached.
const agree = rs => {
  const seqs = rs.map(r => r.hashCsv.split(',').filter(Boolean)), n = Math.min(...seqs.map(s => s.length));
  return n > 3 && seqs.every(s => s.slice(0, n).join() === seqs[0].slice(0, n).join()) ? n : 0;
};
const ev = r => (r.events || []).map(e => e.t + (e.slot ?? '') + (e.why ? ':' + e.why : '')).join(',');
const show = rs => { for (const r of rs) console.log(`     ${r.name} slot ${r.slot} ${r.why} frame ${r.stats?.now} rb ${r.stats?.rollbacks} hashes ${r.hashes} overAfterEnd ${r.overAfterEnd} idle ${JSON.stringify(r.idle)} ev ${ev(r)} ${r.out ?? ''}`); };
const only = process.argv[2];
const T = async (name, o, test) => { if (only && !name.includes(only)) return; const rs = await race(o); const why = test(rs); check(name, why === true, why === true ? '' : why); show(rs); };

await T('away (hidden tab, 6 s): the fighter stands, comes back, plays again, hashes equal', { n: 3, per: i => ({ pad: 2, frames: 2400, track: 2, lagup: 20, lagdown: 20, pause: i === 2 ? '400,6000,1' : '' }) }, rs => {
  const [a, , c] = rs;
  if (!rs.every(r => r.why === 'frames')) return 'a page did not reach the end';
  if (!a.events.some(e => e.t === 'away' && e.slot === 2) || !a.events.some(e => e.t === 'back' && e.slot === 2)) return 'no away/back';
  if (a.idle.away.n < 100 || a.idle.away.moved) return 'fighter moved while away ' + JSON.stringify(a.idle.away);
  if (!a.idle.back.moved) return 'fighter never moved after coming back';
  if (a.events.some(e => e.t === 'cpu')) return 'cpu';
  const n = agree(rs); return n ? true : 'hashes differ';
});
await T('away by silence (no hidden message, 5 s): stands, comes back, hashes equal', { n: 2, env: { SILENCE_MS: '2000' }, per: i => ({ pad: 2, frames: 2400, track: 1, pause: i === 1 ? '400,5000' : '' }) }, rs => {
  const [a] = rs;
  if (!rs.every(r => r.why === 'frames')) return 'a page did not reach the end';
  if (!a.events.some(e => e.t === 'back' && e.slot === 1)) return 'never back';
  if (a.idle.away.moved) return 'moved while away';
  return agree(rs) ? true : 'hashes differ';
});
await T('a background page does not hold up the end of the match', { n: 2, config: { time: 1 }, per: i => ({ pad: 2, frames: 99999, waitover: 1, timeout: 200000, pause: i === 1 ? '1000,150000' : '' }) }, rs => {
  const [a, b] = rs;
  if (a.why !== 'ended') return 'host page did not end: ' + a.why;
  if (!(a.overAfterEnd >= 0 && a.overAfterEnd < 3000)) return 'over came ' + a.overAfterEnd + ' ms after the end';
  return b.why.startsWith('over') ? true : 'background page: ' + b.why;
});
await T('a page whose match fails to start does not hold anyone', { n: 2, config: { time: 1 }, per: i => ({ pad: 2, frames: 99999, waitover: 1, timeout: 200000, throwongo: i === 1 ? 1 : '' }) }, rs => {
  const [a, b] = rs;
  if (a.why !== 'ended' || !(a.overAfterEnd < 3000)) return `host ${a.why} over after ${a.overAfterEnd}`;
  return b.events.some(e => e.t === 'failed') ? true : 'no failed event on the failing page';
});
await T('a desync ends the race, then the room starts again and stays in sync', { n: 2, starts: 2, config: { time: 1 }, per: i => ({ pad: 2, frames: 900, races: 2, waitover: 1, timeout: 300000, desyncat: i === 1 ? 300 : '' }) }, rs => {
  const [a, b] = rs;
  if (!rs.every(r => r.races.length === 2)) return 'not two races: ' + rs.map(r => r.races.length);
  if (a.races[0].why !== 'desync' || b.races[0].why !== 'desync') return 'first race did not end as a desync';
  if (a.races[1].desync !== null || b.races[1].desync !== null) return 'second race desynced';
  return a.races[1].hashCsv && a.races[1].hashCsv === b.races[1].hashCsv.slice(0, a.races[1].hashCsv.length) || b.races[1].hashCsv === a.races[1].hashCsv.slice(0, b.races[1].hashCsv.length) ? true : 'second race hashes differ';
});
await T('a socket gone for 2 s comes back and plays on', { n: 2, per: i => ({ pad: 2, frames: 2400, dropsocket: i === 1 ? '500,2000' : '' }) }, rs =>
  rs.every(r => r.why === 'frames') && rs[1].events.some(e => e.t === 'rejoined') ? (agree(rs) ? true : 'hashes differ') : 'did not recover');
await T('a socket gone for 20 s (past the old 15 s history) catches up and plays on', { n: 2, per: i => ({ pad: 2, frames: 3000, timeout: 200000, track: 1, dropsocket: i === 1 ? '500,20000' : '' }) }, rs =>
  rs.every(r => r.why === 'frames') && rs[1].events.some(e => e.t === 'rejoined') ? (agree(rs) ? true : 'hashes differ') : 'did not recover');
await T('a socket gone for 40 s gives up with a message, the others play on', { n: 2, per: i => ({ pad: 2, frames: 3600, timeout: 200000, dropsocket: i === 1 ? '500,40000' : '' }) }, rs =>
  rs[0].why === 'frames' && rs[1].why === 'gone:lost' ? true : `${rs[0].why} / ${rs[1].why}`);
await T('a page reloaded mid-match rejoins it from the start and agrees', { n: 2, per: i => ({ pad: 2, frames: 2400, timeout: 200000, reload: i === 1 ? '900,3000' : '' }) }, rs =>
  rs.every(r => r.why === 'frames') ? (agree(rs) ? true : 'hashes differ') : 'did not rejoin');
console.log('fails=' + fails);
process.exit(fails ? 1 : 0);
