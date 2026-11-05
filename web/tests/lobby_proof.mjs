// Phase 4 and 6 browser proof: two real pages through the lobby UI and the relay, with real key presses.
import { launch } from './lib.mjs'; import { spawn } from 'node:child_process'; import path from 'node:path'; import fs from 'node:fs';
const root = path.resolve(new URL('../..', import.meta.url).pathname);
const out = process.argv[2] || '/tmp'; const port = 18900 + Math.floor(Math.random() * 90);
const build = /BUILD_ID = "(\w+)"/.exec(fs.readFileSync(root + '/web/build.js', 'utf8'))[1];
const relay = spawn(root + '/relay/target/release/sb64-relay', [], { env: { ...process.env, PORT: String(port), SITE: root + '/web', RELAY_SECRET: 'testsecret', BUILD_ID: build }, stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise(r => relay.stdout.once('data', r));
const base = `http://127.0.0.1:${port}/`;
let fails = 0; const check = (name, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name} ${extra}`); if (!ok) fails++; };
const br = await launch();
const page = async (label) => { const c = await br.newContext({ viewport: { width: 1000, height: 780 } }); const p = await c.newPage(); p.on('pageerror', e => { if (String(e.message) !== '0') console.log(label, 'pageerror:', e.message.slice(0, 300)); }); return p; };
try {
  // error paths
  const e1 = await page('e1');
  await e1.goto(base + 'index.html?host=1'); await e1.fill('#hostName', 'x'); await e1.fill('#secret', 'wrong'); await e1.click('#newLobby');
  await e1.waitForFunction(() => document.getElementById('err').textContent.includes('secret'), null, { timeout: 15000 });
  check('wrong host secret gives a plain message', true);
  await e1.goto(base + 'index.html'); await e1.fill('#name', 'x'); await e1.fill('#code', 'ZZZZZ'); await e1.click('#join');
  await e1.waitForFunction(() => document.getElementById('err').textContent.includes('does not exist'), null, { timeout: 60000 });
  check('unknown lobby gives a plain message', true);
  await e1.context().close();
  // host
  const a = await page('host');
  await a.goto(base + 'index.html?host=1'); await a.fill('#hostName', 'Ann'); await a.fill('#secret', 'testsecret'); await a.click('#newLobby');
  await a.waitForSelector('#sRoom:not([hidden])', { timeout: 60000 });
  const code = await a.textContent('#roomCode');
  check('host made a lobby', /^[A-Z2-9]{5}$/.test(code), code);
  // friend with a link
  const b = await page('friend');
  await b.goto(base + `index.html?room=${code}`); await b.fill('#name', 'Ben'); await b.click('#join');
  await b.waitForSelector('#sRoom:not([hidden])', { timeout: 60000 });
  await a.waitForFunction(() => document.querySelectorAll('#roster tr').length === 2, null, { timeout: 15000 });
  check('both players on the roster', true);
  check('friend cannot start', await b.isHidden('#start'));
  await a.selectOption('#fighter', '5'); await b.selectOption('#fighter', '9');
  await a.selectOption('#stage_', '6'); await a.selectOption('#time', '1');
  // two idle humans would tie, and a sudden death nobody plays never ends; CPUs guarantee a result
  await a.selectOption('#cpu', '4'); await a.selectOption('#level', '9');
  await b.waitForFunction(() => document.getElementById('stage_').value === '6', null, { timeout: 5000 });
  check('friend sees the host settings', true);
  await a.screenshot({ path: out + '/lobby.png' });
  await a.waitForFunction(() => !document.getElementById('start').disabled, null, { timeout: 60000 });
  await a.click('#start');
  await a.waitForFunction(() => window.sb.net?.state === 'racing' && window.sb.net.m._NetConfirmed() > 400, null, { timeout: 120000 });
  // real key presses on the host: walk and jump, on the friend: attack
  const before = await a.evaluate(() => window.sb.net.hashes.length);
  const xOf = (p, s) => p.evaluate(sl => window.sb.net.m._port_fighter_x(sl), s);
  const slotA = await a.evaluate(() => window.sb.net.slot), slotB = await b.evaluate(() => window.sb.net.slot);
  const ax0 = await xOf(a, slotA);
  await a.keyboard.down('ArrowRight'); await a.waitForTimeout(1500); await a.keyboard.up('ArrowRight');
  const ax1 = await xOf(a, slotA);
  check('a real key press moved the host fighter', Math.abs(ax1 - ax0) > 50, `x ${ax0} to ${ax1}`);
  await a.keyboard.press('ArrowUp'); await b.keyboard.down('KeyZ'); await b.waitForTimeout(300); await b.keyboard.up('KeyZ');
  const bx0 = await xOf(b, slotB);
  await b.keyboard.down('ArrowLeft'); await b.waitForTimeout(1500); await b.keyboard.up('ArrowLeft');
  const bx1 = await xOf(b, slotB);
  check('a real key press moved the friend fighter', Math.abs(bx1 - bx0) > 50, `x ${bx0} to ${bx1}`);
  await a.waitForTimeout(3000);
  await a.screenshot({ path: out + '/match_host.png' }); await b.screenshot({ path: out + '/match_friend.png' });
  const sa = await a.evaluate(() => ({ h: window.sb.net.hashes, s: window.sb.net.stats(), d: window.sb.net.desync, canv: (() => { const g = document.getElementById('game'); return [g.width, g.height]; })() }));
  const sb_ = await b.evaluate(() => ({ h: window.sb.net.hashes, s: window.sb.net.stats(), d: window.sb.net.desync }));
  const n = Math.min(sa.h.length, sb_.h.length); let same = n > 5;
  for (let i = 0; i < n; i++) if (sa.h[i][0] !== sb_.h[i][0] || sa.h[i][1] !== sb_.h[i][1]) same = false;
  check('both pages agree on every hash', same && sa.d === null && sb_.d === null, `${n} hashes, frame ${sa.s.now}/${sb_.s.now} rollbacks ${sa.s.rollbacks}/${sb_.s.rollbacks} rtt ${sa.s.rtt}/${sb_.s.rtt}`);
  check('frames advance in real time', sa.s.now > 500, `frame ${sa.s.now}`);
  // to the end of a one minute match, then back to the lobby
  // report where it is while waiting
  const pr = setInterval(async () => { try { console.log('     ', await a.evaluate(() => { const m = window.sb.net.m, s = m._GameStatus(); return `frame ${m._NetNow()} status ${s} ended ${m._NetEnded()} state ${window.sb.net.state} finished ${window.sb.net.finished}`; })); } catch {} }, 20000);
  await a.waitForFunction(() => !document.getElementById('lobby').hidden, null, { timeout: 240000 });
  clearInterval(pr);
  await b.waitForFunction(() => !document.getElementById('lobby').hidden, null, { timeout: 30000 });
  check('both pages return to the lobby after the match', true);
  const fa = await a.evaluate(() => window.sb.net.stats()), fb = await b.evaluate(() => window.sb.net.stats());
  check('match ended on the same frame', fa.now === fb.now, `${fa.now} ${fb.now}`);
  await a.waitForFunction(() => !document.getElementById('start').disabled && !document.getElementById('start').hidden, null, { timeout: 10000 });
  await a.selectOption('#stage_', '2'); await a.click('#start');
  await a.waitForFunction(() => document.getElementById('lobby').hidden && window.sb.net.m._NetConfirmed() > 200, null, { timeout: 90000 });
  await a.waitForTimeout(2000); await a.screenshot({ path: out + '/rematch.png' });
  const r1 = await a.evaluate(() => window.sb.net.hashes.at(-1)), d1 = await a.evaluate(() => window.sb.net.desync);
  check('a rematch starts and stays in sync', d1 === null, JSON.stringify(r1));
} catch (e) { console.log('FAIL exception', e.message.split('\n')[0]); fails++; }
await br.close(); relay.kill();
console.log('fails=' + fails); process.exit(fails ? 1 : 0);
