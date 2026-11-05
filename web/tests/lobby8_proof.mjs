// 8-player browser proof: eight real pages join one lobby through the UI, press real keys, and agree on the game.
import { launch } from './lib.mjs'; import { spawn } from 'node:child_process'; import path from 'node:path'; import fs from 'node:fs';
const root = path.resolve(new URL('../..', import.meta.url).pathname);
const out = process.argv[2] || '/tmp'; const port = 18800 + Math.floor(Math.random() * 90);
const build = /BUILD_ID = "(\w+)"/.exec(fs.readFileSync(root + '/web/build.js', 'utf8'))[1];
const relay = spawn(root + '/relay/target/release/sb64-relay', [], { env: { ...process.env, PORT: String(port), SITE: root + '/web', RELAY_SECRET: 'testsecret', BUILD_ID: build }, stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise(r => relay.stdout.once('data', r));
const base = `http://127.0.0.1:${port}/`;
let fails = 0; const check = (name, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name} ${extra}`); if (!ok) fails++; };
const br = await launch();
const page = async label => { const c = await br.newContext({ viewport: { width: 640, height: 500 } }); const p = await c.newPage(); p.on('pageerror', e => { if (String(e.message) !== '0') console.log(label, 'pageerror', e.message.slice(0, 200)); }); return p; };
const NAMES = ['Ann', 'Ben', 'Cal', 'Dee', 'Eli', 'Fay', 'Gus', 'Hal'];
const FIGHTERS = ['0', '1', '2', '3', '4', '5', '6', '7'];
try {
  const pages = [await page(NAMES[0])];
  await pages[0].goto(base + 'index.html?host=1'); await pages[0].fill('#hostName', NAMES[0]); await pages[0].fill('#secret', 'testsecret'); await pages[0].click('#newLobby');
  await pages[0].waitForSelector('#sRoom:not([hidden])', { timeout: 90000 });
  const code = await pages[0].textContent('#roomCode');
  for (let i = 1; i < 8; i++) {
    const p = await page(NAMES[i]); pages.push(p);
    await p.goto(base + `index.html?room=${code}`); await p.fill('#name', NAMES[i]); await p.click('#join');
    await p.waitForSelector('#sRoom:not([hidden])', { timeout: 90000 });
  }
  const host = pages[0];
  await host.waitForFunction(() => document.querySelectorAll('#roster tr').length === 8, null, { timeout: 30000 });
  check('eight players on the roster', true);
  await Promise.all(pages.map((p, i) => p.selectOption('#fighter', FIGHTERS[i])));
  await host.selectOption('#stage_', '6'); await host.selectOption('#time', '1');
  const nine = await page('ninth');
  await nine.goto(base + `index.html?room=${code}`); await nine.fill('#name', 'Ivy'); await nine.click('#join');
  await nine.waitForFunction(() => document.getElementById('err').textContent.length > 0, null, { timeout: 90000 });
  check('a ninth player is told the lobby is full', true, await nine.textContent('#err'));
  await nine.context().close();
  await host.screenshot({ path: out + '/lobby8.png' });
  await host.waitForFunction(() => !document.getElementById('start').disabled, null, { timeout: 120000 });
  await host.click('#start');
  await Promise.all(pages.map(p => p.waitForFunction(() => window.sb.net?.state === 'racing' && window.sb.net.m._NetConfirmed() > 300, null, { timeout: 180000 })));
  const slots = await Promise.all(pages.map(p => p.evaluate(() => window.sb.net.slot)));
  check('eight distinct slots', new Set(slots).size === 8, slots.join(','));
  // real keys on every page, a different move for each
  const keys = ['ArrowRight', 'ArrowLeft', 'ArrowRight', 'ArrowLeft', 'ArrowRight', 'ArrowLeft', 'ArrowRight', 'ArrowLeft'];
  const xs = () => Promise.all(pages.map((p, i) => p.evaluate(s => window.sb.net.m._port_fighter_x(s), slots[i])));
  const x0 = await xs();
  await Promise.all(pages.map((p, i) => p.keyboard.down(keys[i])));
  await pages[0].waitForTimeout(1500);
  await Promise.all(pages.map((p, i) => p.keyboard.up(keys[i])));
  const x1 = await xs();
  check('a real key press moved every fighter', x1.every((x, i) => Math.abs(x - x0[i]) > 50), x0.map((x, i) => `${x}>${x1[i]}`).join(' '));
  await Promise.all(pages.map((p, i) => p.keyboard.press(['KeyZ', 'KeyX', 'KeyC', 'ArrowUp'][i % 4])));
  await pages[0].waitForTimeout(8000);
  await host.screenshot({ path: out + '/match8.png' });
  const st = await Promise.all(pages.map(p => p.evaluate(() => ({ h: window.sb.net.hashes, s: window.sb.net.stats(), d: window.sb.net.desync }))));
  const n = Math.min(...st.map(x => x.h.length)); let same = n > 5;
  for (const x of st) for (let i = 0; i < n; i++) if (x.h[i][0] !== st[0].h[i][0] || x.h[i][1] !== st[0].h[i][1]) same = false;
  check('all eight pages agree on every hash', same && st.every(x => x.d === null), `${n} hashes, frames ${st.map(x => x.s.now).join(' ')}`);
  console.log('     rollbacks ' + st.map(x => x.s.rollbacks).join(' ') + ' stalls ' + st.map(x => x.s.stalls).join(' '));
} catch (e) { console.log('FAIL exception', e.message.split('\n')[0]); fails++; }
await br.close(); relay.kill();
console.log('fails=' + fails); process.exit(fails ? 1 : 0);
