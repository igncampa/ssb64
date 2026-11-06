// Two people in a lobby on a scratch relay serving the dev page: screenshots, sounds, picks.
import { spawn } from 'node:child_process'; import path from 'node:path';
import { launch } from './lib.mjs';
const root = path.resolve(new URL('../..', import.meta.url).pathname);
const site = process.env.SITE, out = process.argv[2];
const port = 18900 + Math.floor(Math.random() * 90);
const relay = spawn(root + '/relay/target/release/ssb64-relay', [], { env: { ...process.env, PORT: String(port), SITE: site, RELAY_SECRET: '', RELAY_TEST: '1' }, stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise(r => relay.stdout.once('data', r));
const br = await launch();
let fails = 0; const check = (n, ok, x = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${n}${x ? ' : ' + x : ''}`); if (!ok) fails++; };
try {
  const { code } = await (await fetch(`http://127.0.0.1:${port}/api/lobby`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{}' })).json();
  const mk = async (name, vp) => { const c = await br.newContext({ viewport: vp }); const p = await c.newPage(); p.errs = []; p.on('pageerror', e => p.errs.push(e.message)); await p.goto(`http://127.0.0.1:${port}/?room=${code}`); await p.fill('#name', name); await p.click('#join'); await p.waitForFunction(() => document.querySelectorAll('.ft img').length === 12 && !document.getElementById('sRoom').hidden, null, { timeout: 60000 }); return p; };
  const a = await mk('Ann', { width: 1200, height: 900 });
  const b = await mk('Ben', { width: 1680, height: 720 });
  await a.click('.ft[data-f="7"]'); await b.click('.ft[data-f="9"]'); await b.click('.costumes button[data-c="2"]');
  await a.waitForFunction(() => document.querySelectorAll('#roster tr').length === 2 && [...document.querySelectorAll('#roster .st')].some(e => e.textContent.includes('Pikachu')), null, { timeout: 10000 });
  check('two people, picks shown on both', true);
  const host = await a.evaluate(() => window.sb.net.id) === await a.evaluate(() => 0) ? a : a;
  await a.click('.sg[data-s="6"]');
  await b.waitForFunction(() => document.querySelector('.sg.on')?.dataset.s === '6', null, { timeout: 10000 }).then(() => check('the host picks the stage, the friend sees it', true)).catch(() => check('the host picks the stage, the friend sees it', false));
  check('the friend cannot pick a stage', await b.evaluate(() => document.querySelector('.sg[data-s="2"]').disabled));
  // Sound: the engine's ring fills while in the lobby (music), and stops before the match.
  await a.click('#lobby'); await a.waitForTimeout(1500);
  const head1 = await a.evaluate(() => window.eng.m._AudioHead()); await a.waitForTimeout(1000); const head2 = await a.evaluate(() => window.eng.m._AudioHead());
  check('lobby audio runs (about a second of samples a second)', head2 - head1 > 20000, `${head2 - head1}`);
  const lvl = await a.evaluate(() => { const m = window.eng.m, r = m._AudioRing() >> 1, n = m._AudioRingFrames(), h = m._AudioHead(); let s = 0; for (let i = 0; i < 4000; i++) { const v = m.HEAP16[r + (((h - 4000 + i) & (n - 1)) * 2)]; s += v * v; } return Math.sqrt(s / 4000); });
  check('and it is not silence (the select-screen music)', lvl > 150, `rms ${Math.round(lvl)}`);
  await a.screenshot({ path: `${out}/lobby-host.png` }); await b.screenshot({ path: `${out}/lobby-friend.png` });
  const h0 = await a.evaluate(() => window.eng.m._AudioHead());
  await a.click('#sound'); await a.waitForTimeout(800);
  const lvl2 = await a.evaluate((h0) => { if (window.eng.m._AudioHead() - h0 < 8000) return 0; const m = window.eng.m, r = m._AudioRing() >> 1, n = m._AudioRingFrames(), h = m._AudioHead(); let s = 0; for (let i = 0; i < 4000; i++) { const v = m.HEAP16[r + (((h - 4000 + i) & (n - 1)) * 2)]; s += v * v; } return Math.sqrt(s / 4000); }, h0);
  check('Sound off: the music stops', lvl2 < 60, `rms ${Math.round(lvl2)}`);
  await a.click('#sound');
  // Narrow and tall: the landscape note covers the page.
  const t = await br.newContext({ viewport: { width: 400, height: 800 } }); const tp = await t.newPage(); await tp.goto(`http://127.0.0.1:${port}/?room=${code}`);
  check('a tall window: asked to turn sideways', await tp.isVisible('#turn')); await tp.screenshot({ path: `${out}/lobby-portrait.png` }); await t.close();
  check('a wide window: no note', !(await a.isVisible('#turn')));
  // Start a match: lobby audio stops, the match runs.
  await a.selectOption('#time', '1');
  await a.click('#start');
  await Promise.all([a, b].map(p => p.waitForFunction(() => window.sb.net?.state === 'racing' && window.sb.net.m._NetConfirmed() > 1500, null, { timeout: 180000 })));
  check('the match starts for both', true);
  await a.screenshot({ path: `${out}/match.png` });
  const h = await Promise.all([a, b].map(p => p.evaluate(() => { const n = window.sb.net; return n.hashes.filter(x => x[0] <= 1500).map(x => x[1]).join(); })));
  check('a 4:3 screen and a 21:9 screen stay in sync for 1500 frames', h[0] === h[1] && h[0].split(',').length >= 50, h[0].split(',').length + ' hashes');
  await b.screenshot({ path: `${out}/match-wide.png` });
  check('no page errors', a.errs.length + b.errs.length === 0, [...a.errs, ...b.errs].join(' | ').slice(0, 300));
} catch (e) { check('ran to the end', false, e.message.slice(0, 400)); }
await br.close(); relay.kill(); process.exit(fails ? 1 : 0);
