// Extras browser proof: two real pages through the lobby UI and a private relay serving this tree's web/.
// The host clicks the Final Destination tile, the Metal Mario tile and unchecks items; the friend clicks
// a Fighting Polygon. Both pages must see the same settings, and the sim must get them (PortMatchInfo).
//   node web/tests/extras_lobby_proof.mjs <out-dir>
import { launch } from './lib.mjs'; import { spawn } from 'node:child_process'; import path from 'node:path'; import fs from 'node:fs';
import { ITEMS, itemMask } from '../items.js';
const root = path.resolve(new URL('../..', import.meta.url).pathname);
const out = process.argv[2] || '/tmp'; fs.mkdirSync(out, { recursive: true });
const port = 19600 + Math.floor(Math.random() * 90);
const build = /BUILD_ID = "(\w+)"/.exec(fs.readFileSync(root + '/web/build.js', 'utf8'))[1];
const relay = spawn(root + '/relay/target/release/ssb64-relay', [], { env: { ...process.env, PORT: String(port), SITE: root + '/web', RELAY_SECRET: 'testsecret', BUILD_ID: build }, stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise(r => relay.stdout.once('data', r));
const base = `http://127.0.0.1:${port}/`;
let fails = 0; const check = (name, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name} ${extra}`); if (!ok) fails++; };
const br = await launch();
const errs = [];
const page = async label => { const c = await br.newContext({ viewport: { width: 1280, height: 900 } }); const p = await c.newPage(); p.on('pageerror', e => { if (String(e.message) !== '0') { errs.push(label + ': ' + e.message); console.log(label, 'pageerror:', e.message.slice(0, 300)); } }); return p; };
const info = p => p.evaluate(() => { const m = window.sb.net.m; return { stage: m._PortMatchInfo(0, 0), mask: m._PortMatchInfo(1, 0) >>> 0, fkind: [0, 1, 2, 3].map(i => m._PortMatchInfo(2, i)) }; });
try {
  const a = await page('host');
  await a.goto(base + 'index.html?host=1'); await a.fill('#hostName', 'Ann'); await a.fill('#secret', 'testsecret'); await a.click('#newLobby');
  await a.waitForSelector('#sRoom:not([hidden])', { timeout: 90000 });
  const code = await a.textContent('#roomCode');
  const b = await page('friend');
  await b.goto(base + `index.html?room=${code}`); await b.fill('#name', 'Ben'); await b.click('#join');
  await b.waitForSelector('#sRoom:not([hidden])', { timeout: 90000 });
  await a.waitForFunction(() => document.querySelectorAll('#roster tr').length === 2, null, { timeout: 15000 });
  await a.waitForSelector('.sg[data-s="16"]', { timeout: 30000 });
  // what the lobby offers
  const offer = await a.evaluate(() => ({
    stages: [...document.querySelectorAll('.sg')].map(b => [+b.dataset.s, b.textContent, b.querySelector('img').src.startsWith('data:image/png') && b.querySelector('img').naturalWidth > 0]),
    extras: [...document.querySelectorAll('#extras .ft')].map(b => [+b.dataset.f, b.title, b.querySelector('img').naturalWidth > 0]),
    fighters: [...document.querySelectorAll('#fighters .ft')].map(b => +b.dataset.f),
    items: [...document.querySelectorAll('#itemList label')].map(l => [l.textContent, l.querySelector('input').checked]),
    rate: document.getElementById('items').value,
  }));
  console.log('stages', JSON.stringify(offer.stages.map(s => s[0] + ':' + s[1])));
  console.log('extras', JSON.stringify(offer.extras.map(s => s[0] + ':' + s[1])));
  for (const k of [13, 14, 16]) check(`stage tile ${k} offered with a picture`, offer.stages.some(s => s[0] === k && s[2]));
  check('12 regular fighters + 14 extras, no Master Hand', offer.fighters.length === 12 && offer.extras.length === 14 && !offer.extras.some(e => e[0] === 12) && offer.extras.every(e => e[2]));
  check('item checklist: 15 switches, all on by default', offer.items.length === 15 && offer.items.every(i => i[1]), offer.items.map(i => i[0]).join(', '));
  check('item rate defaults to Medium', offer.rate === '3');
  check('the friend cannot change items', await b.evaluate(() => [...document.querySelectorAll('#itemList input')].every(x => x.disabled)));
  await a.screenshot({ path: out + '/lobby_default.png', fullPage: true });
  // host: Final Destination, Metal Mario, only Hammer + Maxim Tomato; friend: Polygon Kirby
  await a.click('.sg[data-s="16"]'); await a.click('#extras .ft[data-f="13"]');
  await a.click('#itemsNone');
  await a.click('#itemList label:has-text("Hammer") input'); await a.click('#itemList label:has-text("Maxim Tomato") input');
  await b.click('#extras .ft[data-f="22"]');
  const want = itemMask(ITEMS.map(it => it.name === 'Hammer' || it.name === 'Maxim Tomato'));
  await b.waitForFunction(() => document.getElementById('stage_').value === '16', null, { timeout: 5000 });
  const fb = await b.evaluate(() => [...document.querySelectorAll('#itemList input')].map(x => x.checked));
  check('friend sees Final Destination and the host checklist', fb.filter(Boolean).length === 2 && fb[2] && fb[12], JSON.stringify(fb));
  await a.waitForFunction(() => [...document.querySelectorAll('#roster .st')].some(e => e.textContent.startsWith('Polygon Kirby')), null, { timeout: 5000 });
  check('host roster shows the friend as Polygon Kirby and self as Metal Mario', await a.evaluate(() => { const t = [...document.querySelectorAll('#roster .st')].map(e => e.textContent); return t.some(x => x.startsWith('Metal Mario')) && t.some(x => x.startsWith('Polygon Kirby')); }));
  check('colour buttons are off for a one-look fighter', await a.evaluate(() => [...document.querySelectorAll('#costumes button')].every(x => x.disabled)));
  await a.screenshot({ path: out + '/lobby_picked.png', fullPage: true });
  await a.waitForFunction(() => !document.getElementById('start').disabled, null, { timeout: 60000 });
  await a.click('#start');
  await a.waitForFunction(() => window.sb.net?.state === 'racing' && window.sb.net.m._NetConfirmed() > 300, null, { timeout: 120000 });
  await b.waitForFunction(() => window.sb.net?.state === 'racing' && window.sb.net.m._NetConfirmed() > 300, null, { timeout: 120000 });
  const ia = await info(a), ib = await info(b);
  const slotA = await a.evaluate(() => window.sb.net.slot), slotB = await b.evaluate(() => window.sb.net.slot);
  console.log('host sim', JSON.stringify(ia), 'slot', slotA, ' friend sim', JSON.stringify(ib), 'slot', slotB);
  check('both sims run Final Destination', ia.stage === 16 && ib.stage === 16);
  check(`both sims got the item mask ${want.toString(16)}`, ia.mask === want && ib.mask === want);
  check('both sims: host slot Metal Mario (13), friend slot Polygon Kirby (22)', ia.fkind[slotA] === 13 && ia.fkind[slotB] === 22 && JSON.stringify(ia.fkind) === JSON.stringify(ib.fkind));
  // real key presses move Metal Mario
  const x0 = await a.evaluate(s => window.sb.net.m._port_fighter_x(s), slotA);
  await a.keyboard.down('ArrowRight'); await a.waitForTimeout(1500); await a.keyboard.up('ArrowRight');
  const x1 = await a.evaluate(s => window.sb.net.m._port_fighter_x(s), slotA);
  check('a real key press moved Metal Mario', Math.abs(x1 - x0) > 50, `x ${x0} to ${x1}`);
  await a.waitForTimeout(2500);
  await a.screenshot({ path: out + '/match_host.png' }); await b.screenshot({ path: out + '/match_friend.png' });
  const h = await Promise.all([a, b].map(p => p.evaluate(() => ({ h: window.sb.net.hashes.map(x => x.join(':')), d: window.sb.net.desync }))));
  const n = Math.min(h[0].h.length, h[1].h.length);
  check('both pages agree on every hash so far', n > 5 && h[0].h.slice(0, n).join() === h[1].h.slice(0, n).join() && h[0].d == null && h[1].d == null, `${n} hashes`);
  check('no page errors', errs.length === 0, errs.join(' | '));
} catch (e) { console.log('FAIL exception', e.message); fails++; }
await br.close(); relay.kill();
console.log('fails=' + fails);
process.exit(fails ? 1 : 0);
