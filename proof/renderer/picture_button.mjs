// usage: node picture_button.mjs outdir: the lobby's Picture button on a scratch relay (port 18490-18499):
// present, cycles Smooth -> Sharp -> N64 pixels -> Smooth, remembers the choice across a reload.
import { spawn } from 'node:child_process'; import path from 'node:path';
import { launch } from '../../web/tests/lib.mjs';
const root = path.resolve(new URL('../..', import.meta.url).pathname), out = process.argv[2];
const port = 18490 + Math.floor(Math.random() * 10);
const relay = spawn(root + '/relay/target/release/ssb64-relay', [], { env: { ...process.env, PORT: String(port), SITE: root + '/web', RELAY_SECRET: '', RELAY_TEST: '1' }, stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise(r => relay.stdout.once('data', r));
const br = await launch(); let fails = 0, checks = 0;
const check = (n, ok, x = '') => { checks++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${n}${x ? ' : ' + x : ''}`); if (!ok) fails++; };
try {
  const { code } = await (await fetch(`http://127.0.0.1:${port}/api/lobby`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{}' })).json();
  const c = await br.newContext({ viewport: { width: 1200, height: 900 } }); const p = await c.newPage(); const errs = []; p.on('pageerror', e => errs.push(e.message));
  // the first visit types a name; a reload with the room in the URL joins by itself (the name is remembered)
  const enter = async (first) => { await p.goto(`http://127.0.0.1:${port}/?room=${code}`); if (first) { await p.fill('#name', 'Ann'); await p.click('#join'); } await p.waitForFunction(() => window.eng && !document.getElementById('sRoom').hidden, null, { timeout: 60000 }); };
  await enter(true);
  const label = () => p.textContent('#picture');
  check('Picture button shown, Smooth by default', await p.isVisible('#picture') && await label() === 'Smooth picture', await label());
  await p.locator('.go').screenshot({ path: `${out}/lobby_go_row.png` });
  const seen = []; for (let i = 0; i < 3; i++) { await p.click('#picture'); seen.push(await label()); }
  check('cycles Sharp, N64 pixels, Smooth', seen.join('|') === 'Sharp picture|N64 pixels|Smooth picture', seen.join('|'));
  await p.click('#picture'); await p.click('#picture'); // N64 pixels
  await enter();
  check('the choice survives a reload', await label() === 'N64 pixels', await label());
  check('no page errors', errs.filter(e => e !== '0').length === 0, errs.join('; '));
} finally { console.log(`checks=${checks} fails=${fails}`); await br.close(); relay.kill(); }
process.exit(fails ? 1 : 0);
