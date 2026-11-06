// Extras proof: Meta Crystal, Duel Zone and Final Destination; Metal Mario, Giant DK and the Fighting
// Polygons as human picks and as CPUs; the item checklist's mask. Real relay, real engines in separate
// processes (net_client.mjs), real sockets, 30 ms each way. The sim reports what StartMatch handed it
// (PortMatchInfo) and which item kinds appeared (PortItemsSeen); every page must agree hash for hash.
//   node web/tests/extras_proof.mjs [filter]
import { spawn } from 'node:child_process'; import path from 'node:path';
import { ITEMS, itemMask } from '../items.js';
const root = path.resolve(new URL('../..', import.meta.url).pathname);
const relayBin = root + '/relay/target/release/ssb64-relay';
let fails = 0, port = (+process.env.PORT_BASE || 18100) + Math.floor(Math.random() * 20);
const check = (name, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${extra ? '  ' + extra : ''}`); if (!ok) fails++; };
const startRelay = () => new Promise(res => {
  const own = ++port;
  const p = spawn(relayBin, [], { env: { ...process.env, PORT: String(own), SITE: root + '/web', RELAY_SECRET: '', RELAY_TEST: '1' }, stdio: ['ignore', 'pipe', 'inherit'] });
  p.stdout.once('data', () => res({ p, port: own }));
});
const client = args => new Promise(res => {
  const c = spawn('node', [root + '/web/tests/net_client.mjs', ...Object.entries(args).map(([k, v]) => `${k}=${v}`)], { stdio: ['ignore', 'pipe', 'inherit'] });
  let out = ''; c.stdout.on('data', d => out += d); c.on('close', () => { try { res(JSON.parse(out.trim().split('\n').pop())); } catch { res({ why: 'crash', out: out.slice(0, 300) }); } });
});
// picks: one FTKind per page (slots 0, 1, ...). cpus: FTKinds for CPU slots after the pages.
async function race({ picks, cpus = [], stage, frames = 1200, itemRate = 3, itemToggles = 0xFFFFFFFF, time = 3 }) {
  const { p, port: prt } = await startRelay();
  const { code } = await (await fetch(`http://127.0.0.1:${prt}/api/lobby`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{}' })).json();
  const runs = [];
  for (let i = 0; i < picks.length; i++) { runs.push(client({ port: prt, code, name: 'c' + i, frames, pad: 2, lagup: 30, lagdown: 30, fighter: picks[i], timeout: 240000 })); await new Promise(r => setTimeout(r, 900)); }
  const n = picks.length, fighters = [0, 1, 2, 3, 4, 5, 6, 7], cpu = Array(8).fill(0);
  cpus.forEach((k, j) => { fighters[n + j] = k; cpu[n + j] = 1; });
  const config = { fighters, cpu, levels: Array(8).fill(9), stage, rules: 1, time, stocks: 2, itemRate, itemToggles };
  await fetch(`http://127.0.0.1:${prt}/api/start/${code}`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ config }) });
  const res = await Promise.all(runs);
  p.kill();
  const alive = res.filter(r => r.why === 'frames' || r.why === 'ended');
  const common = Math.min(...alive.map(r => r.hashCsv.split(',').length));
  const same = alive.length === res.length && alive.every(r => r.hashCsv.split(',').slice(0, common).join() === alive[0].hashCsv.split(',').slice(0, common).join());
  const agree = alive.every(r => JSON.stringify(r.info) === JSON.stringify(alive[0].info) && r.itemsSeen === alive[0].itemsSeen);
  return { res, same: same && agree && !res.some(r => r.desync != null), info: alive[0]?.info, seen: alive[0]?.itemsSeen, frames: alive[0]?.stats?.now, rb: res.map(r => r.stats?.rollbacks) };
}
const NAMES = { 13: 'Metal Mario', 26: 'Giant DK' }; ['Mario', 'Fox', 'DK', 'Samus', 'Luigi', 'Link', 'Yoshi', 'Falcon', 'Kirby', 'Pikachu', 'Jigglypuff', 'Ness'].forEach((n, i) => { NAMES[i] = n; NAMES[14 + i] = 'Polygon ' + n; });
// Stage music (src/gm/gmsound.h SYAudioBGM): Meta Crystal and Duel Zone from their maps; Final Destination's
// map names the boss entrance jingle, and the VS game must play the stage's theme (nSYAudioBGMLast) instead.
const BGM = { 13: 37, 14: 36, 16: 25 }; // nSYAudioBGMMetal, Zako, Last (BossEntry is 24)
const STAGE = { 0: "Peach's Castle", 13: 'Meta Crystal', 14: 'Duel Zone', 16: 'Final Destination' };
const hex = v => '0x' + (v >>> 0).toString(16).padStart(8, '0');
const only = process.argv[2];
const T = async (name, o, test) => {
  if (only && !name.includes(only)) return;
  const r = await race(o);
  const why = test(r);
  check(name, r.same && why === true, `${r.same ? '' : 'PAGES DISAGREE '}${why === true ? '' : why} | stage ${r.info?.stage} fkind ${r.info?.fkind?.join(',')} pkind ${r.info?.pkind?.join(',')} mask ${hex(r.info?.mask)} seen ${hex(r.seen)} frames ${r.frames} rollbacks ${r.rb.join('/')}`);
  for (const x of r.res) if (x.why !== 'frames' && x.why !== 'ended') console.log('     ', x.name, x.why, x.out ?? '');
  if (!r.same) for (const x of r.res) console.log('      diag', x.name, x.why, 'desync', x.desync, 'seen', (x.itemsSeen >>> 0).toString(16), JSON.stringify(x.info), 'hashes', x.hashCsv.split(',').slice(-2).join(','), 'conf', x.stats?.confirmed, 'now', x.stats?.now);
};
const kindsOk = (r, want) => want.every((k, i) => r.info?.fkind[i] === k) || `fkind want ${want.join(',')}`;

// 1. The three stages, each with the three new kinds of fighter on it, two as people and two as CPUs.
for (const [stage, picks, cpus] of [[13, [13, 26], [14, 25]], [14, [26, 17], [13, 21]], [16, [19, 13], [26, 22]]])
  await T(`stage ${stage} ${STAGE[stage]}: people ${picks.map(k => NAMES[k]).join(' + ')}, CPUs ${cpus.map(k => NAMES[k]).join(' + ')}`, { picks, cpus, stage, frames: 1800 },
    r => r.info?.stage !== stage ? `stage ${r.info?.stage}` : r.info.bgm !== BGM[stage] ? `music ${r.info.bgm} want ${BGM[stage]}` : kindsOk(r, [...picks, ...cpus]));
// 1b. Eight different extra fighters at once (2 people, 6 CPUs): every file loads in one match.
await T('eight different extra fighters at once on Duel Zone', { picks: [13, 26], cpus: [14, 16, 18, 20, 22, 25], stage: 14, frames: 1500 }, r => kindsOk(r, [13, 26, 14, 16, 18, 20, 22, 25]));
// 2. Every one of the 14 new fighters as a person's pick, two pages per race, rotating stages.
const extra = [13, 26, ...Array.from({ length: 12 }, (_, i) => 14 + i)];
for (let i = 0; i < extra.length; i += 2) {
  const picks = extra.slice(i, i + 2), stage = [13, 14, 16, 0][(i / 2) % 4];
  await T(`picks ${picks.map(k => NAMES[k]).join(' + ')} on ${STAGE[stage]}`, { picks, stage, frames: 900 }, r => kindsOk(r, picks) === true && r.info.pkind[0] === 0 && r.info.pkind[1] === 0 ? true : 'not human or wrong kind');
}
// 3. Master Hand (12) is never playable: a page that sends it gets its slot's default fighter.
await T('a pick of 12 (Master Hand) falls back to the slot default', { picks: [12, 13], stage: 16, frames: 300 }, r => kindsOk(r, [0, 13]));
// 4. Items. Default: everything on, Medium. One item on: only that item and containers' kinds appear.
// Everything off: no item at all. Rate Very high and a long match so items do appear.
const all = Array(15).fill(true), none = Array(15).fill(false), one = i => ITEMS.map((_, j) => j === i);
const COMMON = 0xFFFFF; // ITKind 0 to 19: containers and the 15 switches
check('default checklist (all on) is 0xffffffff, as the lobby sent before', itemMask(all) === 0xFFFFFFFF, hex(itemMask(all)));
check('checklist all off is 0', itemMask(none) === 0);
check('the shell switch clears both shells', (itemMask(ITEMS.map(it => it.name !== 'Shell')) & ((1 << 17) | (1 << 18))) === 0);
check('one item on keeps the 4 containers on', (itemMask(one(0)) & 0xF) === 0xF, hex(itemMask(one(0))));
// Final Destination's map has no item weights (src/relocData/266_GRLastMap.c, item_weights NULL): the game
// never spawns items there, whatever the switch. The proof holds the port to that.
for (const [label, on, stage] of [['all on, Medium', all, 0], ['all on', all, 13], ['all on (none expected, as in the game)', all, 16], ['only Hammer', one(2), 0], ['only Beam Sword', one(0), 14], ['only Maxim Tomato', one(12), 13], ['all off', none, 0]]) {
  const mask = itemMask(on), rate = label.includes('Medium') ? 3 : 5;
  await T(`items ${label} on ${STAGE[stage]} (rate ${rate})`, { picks: [0, 1], cpus: [2, 3], stage, frames: 5400, itemRate: rate, itemToggles: mask, time: 3 }, r => {
    if (r.info?.mask !== mask) return `mask in sim ${hex(r.info?.mask)} want ${hex(mask)}`;
    const common = r.seen & COMMON;
    if (mask === 0 || stage === 16) return common === 0 || `items appeared ${hex(common)}`;
    if (common & ~mask) return `kinds outside the mask appeared ${hex(common & ~mask)}`;
    return common !== 0 || 'no item appeared';
  });
}
console.log('fails=' + fails);
process.exit(fails ? 1 : 0);
