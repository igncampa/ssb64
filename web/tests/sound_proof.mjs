// Every client hears the same sounds, however late its friends' inputs arrive.
// Two engines on a real relay, one with 90 ms each way and jitter so it rolls back often, all Captain Falcon.
// Compares the sounds each heard up to a frame both confirmed. Run with old=1 for the negative control.
import { spawn } from 'node:child_process'; import path from 'node:path';
const root = path.resolve(new URL('../..', import.meta.url).pathname);
const port = (+process.env.PORT_BASE || 18100) + Math.floor(Math.random() * 20);
const old = process.argv.includes('old=1');
const pair = (process.argv.find(a => a.startsWith('fighters=')) || 'fighters=7,7').slice(9).split(',').map(Number);
const relay = spawn(root + '/relay/target/release/ssb64-relay', [], { env: { ...process.env, PORT: String(port), SITE: root + '/web', RELAY_SECRET: '', RELAY_TEST: '1' }, stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise(r => relay.stdout.once('data', r));
const client = (args) => new Promise(res => {
  const c = spawn('node', [root + '/web/tests/net_client.mjs', ...Object.entries(args).map(([k, v]) => `${k}=${v}`)], { stdio: ['ignore', 'pipe', 'inherit'] });
  let out = ''; c.stdout.on('data', d => out += d); c.on('close', () => { try { res(JSON.parse(out.trim().split('\n').pop())); } catch { res({ why: 'crash', out: out.slice(0, 300) }); } });
});
const { code } = await (await fetch(`http://127.0.0.1:${port}/api/lobby`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{}' })).json();
const frames = 2400, extra = old ? { oldsfx: 1 } : {};
const runs = [client({ port, code, name: 'near', frames, ...extra })];
await new Promise(r => setTimeout(r, 900));
runs.push(client({ port, code, name: 'far', frames, lagup: 90, lagdown: 90, jitter: 30, ...extra }));
await new Promise(r => setTimeout(r, 900));
await fetch(`http://127.0.0.1:${port}/api/start/${code}`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ config: { fighters: pair, stage: pair[0] % 9, rules: 1, time: 3, stocks: 3, itemRate: 0 } }) });
const res = await Promise.all(runs); relay.kill();
const upto = Math.min(...res.map(r => r.stats?.confirmed ?? 0)) - 30;
const heard = res.map(r => r.sfx.filter(s => +s.split(':')[0] <= upto).map(s => s.split(':')[1]).sort());
const count = (l) => l.reduce((m, x) => (m[x] = (m[x] || 0) + 1, m), {});
const [a, b] = heard.map(count);
const diff = [...new Set([...Object.keys(a), ...Object.keys(b)])].filter(k => (a[k] || 0) !== (b[k] || 0)).map(k => `${k}: ${a[k] || 0} vs ${b[k] || 0}`);
for (const r of res) console.log(`     ${r.name} ${r.why} rollbacks ${r.stats?.rollbacks} resim ${r.stats?.resim} sounds ${r.sfx.length}`);
const same = res.every(r => r.why === 'frames') && diff.length === 0 && res[1].stats?.rollbacks > 20;
console.log(`${same ? 'ok  ' : 'FAIL'} ${old ? '(old journal) ' : ''}fighters ${pair}: both heard the same ${heard[0].length} sounds up to frame ${upto}${diff.length ? ' : ' + diff.length + ' ids differ, ' + diff.slice(0, 8).join(', ') : ''}`);
process.exit(same ? 0 : 1);
