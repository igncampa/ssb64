// SM-04: after a rollback the music is put right against what the game asked for, not against what the
// audio thread still plays. Hammer music is a finite song; once it ends by itself the old rule started it
// again from the top at the next rollback. Two pages, one far away (rollbacks), Hammer only, Very high.
// Counts the rollback restarts with the new rule and with the old one (negative control). Same hashes.
import { spawn } from 'node:child_process'; import path from 'node:path';
import { ITEMS, itemMask } from '../items.js';
const root = path.resolve(new URL('../..', import.meta.url).pathname);
let port = (+process.env.PORT_BASE || 18100) + 60 + Math.floor(Math.random() * 20);
const client = args => new Promise(res => {
  const c = spawn('node', [root + '/web/tests/net_client.mjs', ...Object.entries(args).map(([k, v]) => `${k}=${v}`)], { stdio: ['ignore', 'pipe', 'inherit'] });
  let out = ''; c.stdout.on('data', d => out += d); c.on('close', () => { try { res(JSON.parse(out.trim().split('\n').pop())); } catch { res({ why: 'crash', out: out.slice(0, 300) }); } });
});
const hammer = itemMask(ITEMS.map(it => it.name === 'Hammer'));
async function run(old) {
  const own = ++port;
  const p = spawn(root + '/relay/target/release/ssb64-relay', [], { env: { ...process.env, PORT: String(own), SITE: root + '/web', RELAY_SECRET: '', RELAY_TEST: '1' }, stdio: ['ignore', 'pipe', 'inherit'] });
  await new Promise(r => p.stdout.once('data', r));
  const { code } = await (await fetch(`http://127.0.0.1:${own}/api/lobby`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{}' })).json();
  const extra = old ? { oldbgm: 1 } : {};
  const runs = [client({ port: own, code, name: 'near', frames: 5400, pad: 2, timeout: 200000, ...extra })];
  await new Promise(r => setTimeout(r, 900));
  runs.push(client({ port: own, code, name: 'far', frames: 5400, pad: 2, lagup: 90, lagdown: 90, jitter: 30, timeout: 200000, ...extra }));
  await new Promise(r => setTimeout(r, 900));
  await fetch(`http://127.0.0.1:${own}/api/start/${code}`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ config: { fighters: [0, 1, 2, 3], cpu: [0, 0, 1, 1], levels: Array(8).fill(9), stage: 0, rules: 1, time: 3, itemRate: 5, itemToggles: hammer } }) });
  const res = await Promise.all(runs); p.kill();
  return res;
}
let fails = 0;
const neu = await run(false), old = await run(true);
for (const [label, r] of [['new rule', neu], ['old rule', old]]) console.log(`     ${label}: restarts ${r.map(x => x.bgmRestarts).join('/')} rollbacks ${r.map(x => x.stats?.rollbacks).join('/')} items seen 0x${(r[0].itemsSeen >>> 0).toString(16)} hashes equal ${r[0].hashCsv === r[1].hashCsv.slice(0, r[0].hashCsv.length) || r[1].hashCsv === r[0].hashCsv.slice(0, r[1].hashCsv.length)}`);
const sameNew = neu.every(r => r.why === 'frames') && (neu[0].hashCsv.startsWith(neu[1].hashCsv) || neu[1].hashCsv.startsWith(neu[0].hashCsv));
const hammerSeen = (neu[0].itemsSeen & ITEMS.find(i => i.name === 'Hammer').bits) !== 0;
const ok = sameNew && neu[1].bgmRestarts <= old[1].bgmRestarts;
console.log(`${ok ? 'ok  ' : 'FAIL'} music after rollbacks follows the request: far page restarts ${neu[1].bgmRestarts} (old rule ${old[1].bgmRestarts}), pages agree, hammer present ${hammerSeen}`);
process.exit(ok ? 0 : 1);
