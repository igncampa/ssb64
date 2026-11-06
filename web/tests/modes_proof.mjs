// The page's modes in a real browser (spec/state-machines.md SM-01, -02, -12, -15): no frozen screen on
// any path, one banner owner, one retry timer, sound that asks for a press.
//   node web/tests/modes_proof.mjs [outdir]
import { launch } from './lib.mjs'; import { spawn } from 'node:child_process'; import path from 'node:path'; import fs from 'node:fs';
const root = path.resolve(new URL('../..', import.meta.url).pathname);
const out = process.argv[2] || '/tmp';
const build = /BUILD_ID = "(\w+)"/.exec(fs.readFileSync(root + '/web/build.js', 'utf8'))[1];
let port = (+process.env.PORT_BASE || 18100) + 40 + Math.floor(Math.random() * 20);
const relay = (env = {}, p = ++port) => new Promise(res => {
  const r = spawn(root + '/relay/target/release/ssb64-relay', [], { env: { ...process.env, PORT: String(p), SITE: root + '/web', RELAY_SECRET: '', BUILD_ID: build, RELAY_TEST: '1', ...env }, stdio: ['ignore', 'pipe', 'inherit'] });
  r.stdout.once('data', () => res({ r, port: p, base: `http://127.0.0.1:${p}/` }));
});
const stop = r => new Promise(res => { r.once('exit', res); r.kill(); });
const lobby = async base => (await (await fetch(base + 'api/lobby', { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{}' })).json()).code;
const only = process.env.ONLY;
let fails = 0; const check = (name, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name} ${extra}`); if (!ok) fails++; };
const br = await launch();
const page = async (label, init) => {
  const c = await br.newContext({ viewport: { width: 1000, height: 780 } });
  if (init) await c.addInitScript(init);
  const p = await c.newPage(); p.on('pageerror', e => { if (String(e.message) !== '0') console.log(label, 'pageerror:', e.message.slice(0, 300)); });
  return p;
};
const joinAs = async (p, base, code, name) => { await p.goto(base + `index.html?room=${code}`); await p.fill('#name', name); await p.click('#join'); await p.waitForFunction(() => window.sb.mode === 'room', null, { timeout: 90000 }); };
const mode = p => p.evaluate(() => window.sb.mode);
const startMatch = async (a, b) => {
  await a.waitForFunction(() => !document.getElementById('start').disabled, null, { timeout: 60000 });
  await a.click('#start');
  await Promise.all([a, b].map(p => p.waitForFunction(() => window.sb.mode === 'racing' && window.sb.net.m._NetConfirmed() > 120, null, { timeout: 90000 })));
};
try {
  // 1. Leave stops the lobby music (SM-01d)
  if (!only || only.includes('1')) {
    const { r, base } = await relay(); const code = await lobby(base);
    const a = await page('leave'); await joinAs(a, base, code, 'Ann');
    await a.click('#lobby'); await a.waitForTimeout(800);
    const h = () => a.evaluate(() => window.eng.m._AudioHead());
    const h0 = await h(); await a.waitForTimeout(600); const h1 = await h();
    await a.click('#leave'); await a.waitForTimeout(400); const h2 = await h(); await a.waitForTimeout(1000); const h3 = await h();
    check('Leave: back to the Join screen and the lobby music stops', h1 > h0 && h3 === h2 && await mode(a) === 'join' && !(await a.isHidden('#sJoin')), `heads ${h0} ${h1} ${h2} ${h3}`);
    await a.context().close(); await stop(r);
  }
  // 2. The game data fails to load: Join says so and does not wait for ever (SM-01c)
  if (!only || only.includes('2')) {
    const { r, base } = await relay(); const code = await lobby(base);
    const a = await page('badload'); await a.route('**/ssb64.data*', rt => rt.fulfill({ status: 200, body: 'not the game' }));
    await a.goto(base + `index.html?room=${code}`); await a.fill('#name', 'Ann');
    await a.waitForFunction(() => document.getElementById('err').textContent.includes('could not load'), null, { timeout: 60000 });
    const dis = await a.isDisabled('#join');
    check('the game fails to load: a message, Join disabled, no hanging join', dis && await mode(a) === 'join', await a.textContent('#err'));
    await a.context().close(); await stop(r);
  }
  // 3. Sound waits for a press after an auto-join from a link (SM-02): with the browser's default policy and
  // with a press-first policy as real Chrome applies it to a page nobody pressed yet (headless Chromium runs every
  // context at once even with --autoplay-policy=user-gesture-required, so the policy is emulated in the page)
  if (!only || only.includes('3')) for (const strict of [false, true]) {
    const { r, base } = await relay(); const code = await lobby(base);
    const B = br;
    const c = await B.newContext({ viewport: { width: 1000, height: 780 } }); await c.addInitScript(() => { localStorage.setItem('ssb64.name', 'Ann'); });
    if (strict) await c.addInitScript(() => {
      let pressed = false; addEventListener('pointerdown', () => { pressed = true; }, true); addEventListener('keydown', () => { pressed = true; }, true);
      const A = window.AudioContext, resume = A.prototype.resume;
      window.AudioContext = class extends A { constructor(o) { super(o); if (!pressed) this.suspend(); } };
      window.AudioContext.prototype.resume = function () { return pressed ? resume.call(this) : Promise.resolve(); };
    });
    const a = await c.newPage();
    await a.goto(base + `index.html?room=${code}`);
    await a.waitForFunction(() => window.sb.mode === 'room' && window.eng?.audioState !== 'creating' && window.eng?.audioState !== 'none', null, { timeout: 90000 });
    await a.waitForTimeout(500);
    const before = await a.evaluate(() => window.eng.audioState), label = await a.textContent('#sound');
    await a.mouse.click(5, 5); await a.waitForFunction(() => window.eng.audioState === 'running', null, { timeout: 5000 }).catch(() => {});
    const after = await a.evaluate(() => window.eng.audioState), label2 = await a.textContent('#sound');
    const h0 = await a.evaluate(() => window.eng.m._AudioHead()); await a.waitForTimeout(600); const h1 = await a.evaluate(() => window.eng.m._AudioHead());
    check(`auto-join, ${strict ? 'press required' : 'default policy'}: sound runs after one press`, after === 'running' && label2 === 'Sound on' && h1 > h0 && (!strict || (before === 'suspended' && label === 'Click for sound')), `before ${before} (button "${label}") after ${after} ("${label2}")`);
    await c.close(); await stop(r);
  }
  // 4. The relay comes back with another build while racing: the page leaves the match with a message (SM-01a)
  if (!only || only.includes('4')) {
    const R = await relay(); const code = await lobby(R.base);
    const a = await page('a'), b = await page('b');
    await joinAs(a, R.base, code, 'Ann'); await joinAs(b, R.base, code, 'Ben');
    await a.selectOption('#time', '1', { force: true });
    await startMatch(a, b);
    // the stall banner: the relay stops answering for 2.5 s (SM-15)
    process.kill(R.r.pid, 'SIGSTOP');
    const t0 = Date.now(); let seen = '', diag = [];
    while (Date.now() - t0 < 3000) { const t = await a.evaluate(() => document.getElementById('banner').hidden ? '' : document.getElementById('banner').textContent); if (t) seen = t; diag.push(await a.evaluate(() => { const n = window.sb.net, m = n.m; return [m._NetNow() - m._NetConfirmed(), Math.round(performance.now() - n.lastPong), n.link, n.catching, window.sb.mode].join('/'); })); await a.waitForTimeout(100); }
    process.kill(R.r.pid, 'SIGCONT');
    await a.waitForFunction(() => document.getElementById('banner').hidden, null, { timeout: 8000 }).catch(() => {});
    check('the relay stops answering: "Your connection stopped.", then it clears', seen === 'Your connection stopped.' && await a.isHidden('#banner'), `saw "${seen}" after "${await a.evaluate(() => document.getElementById("banner").hidden ? "" : document.getElementById("banner").textContent)}" ${diag.filter((_, i) => i % 5 === 0).join(" ")}`);
    await stop(R.r);
    const R2 = await relay({ BUILD_ID: 'another' }, R.port);
    await a.waitForFunction(() => window.sb.mode === 'join', null, { timeout: 20000 }).catch(() => {});
    const lob = !(await a.isHidden('#lobby')), loop = await a.evaluate(() => window.sb.mode), text = await a.textContent('#err');
    check('a refusal while racing: the match stops, the lobby shows why', lob && loop === 'join' && text.includes('updated'), `mode ${loop} err "${text}"`);
    await a.screenshot({ path: out + '/refused.png' });
    await a.context().close(); await b.context().close(); await stop(R2.r);
  }
  // 5. The relay dies after the local end of a match: no frozen last frame (SM-01b)
  if (!only || only.includes('5')) {
    const R = await relay(); const code = await lobby(R.base);
    const a = await page('a'), b = await page('b');
    await joinAs(a, R.base, code, 'Ann'); await joinAs(b, R.base, code, 'Ben');
    await startMatch(a, b);
    // end the match on the spot: both pages ended locally, and the relay is gone before it says over
    await Promise.all([a, b].map(p => p.evaluate(() => { const n = window.sb.net; n.finished = true; n.phase = 'ended'; n.json = () => {}; })));
    await stop(R.r);
    await a.waitForFunction(() => window.sb.mode === 'join', null, { timeout: 45000 }).catch(() => {});
    check('the relay is gone after the end: the Join screen with a message, not the last frame', await mode(a) === 'join' && !(await a.isHidden('#lobby')), `"${await a.textContent('#err')}"`);
    await a.context().close(); await b.context().close();
  }
  // 6. The portal: one retry timer with back-off while the relay is down (SM-12a)
  if (!only || only.includes('6')) {
    const R = await relay(); const code = await lobby(R.base);
    const stub = `window.N64 = { key: 'k' + Math.random(), hostKey: '', onRoom(cb) { this.cb = cb; }, report() {}, fatal(m) { window.fatalMsg = m; }, notReady: [] };
      window.wsCount = 0; const W = window.WebSocket; window.WebSocket = class extends W { constructor(...a) { super(...a); window.wsCount++; } };`;
    const a = await page('portal', stub);
    await a.route('**/n64-embed.js', rt => rt.fulfill({ status: 200, contentType: 'text/javascript', body: '' }));
    await a.goto(R.base + `index.html?room=${code}&name=Ann`);
    await a.waitForFunction(() => window.sb.mode === 'room', null, { timeout: 90000 });
    await stop(R.r);
    const c0 = await a.evaluate(() => window.wsCount); await a.waitForTimeout(20000); const c1 = await a.evaluate(() => window.wsCount);
    check('portal, relay down for 20 s: a few reconnects, not dozens', c1 - c0 <= 8, `${c1 - c0} sockets`);
    await a.context().close();
  }
  // 7. The portal's "still loading" list with a name longer than the relay keeps (SM-12b)
  if (!only || only.includes('7')) {
    const R = await relay(); const code = await lobby(R.base);
    const long = 'Bartholomew-the-Second';
    const stub = n => `window.N64 = { key: 'k${n}', hostKey: '', onRoom(cb) { this.cb = cb; }, report() {}, fatal(m) { window.fatalMsg = m; }, notReady: [] };`;
    const a = await page('host', stub(1)), b = await page('long', stub(2));
    for (const p of [a, b]) await p.route('**/n64-embed.js', rt => rt.fulfill({ status: 200, contentType: 'text/javascript', body: '' }));
    await a.goto(R.base + `index.html?room=${code}&name=Ann`);
    await a.waitForFunction(() => window.sb.mode === 'room' && window.N64.cb, null, { timeout: 90000 });
    await a.evaluate(n => window.N64.cb({ notReady: [n] }), long);
    const note = await a.textContent('#startNote');
    await b.goto(R.base + `index.html?room=${code}&name=${long}`);
    await a.waitForFunction(() => !document.getElementById('start').disabled, null, { timeout: 90000 }).catch(() => {});
    check('portal: a 22-character name still loading, then joined: START is enabled', !(await a.isDisabled('#start')) && note.includes('Barth'), `note was "${note}"`);
    await a.context().close(); await b.context().close(); await stop(R.r);
  }
} catch (e) { console.log('FAIL exception', e.message.split('\n')[0]); fails++; }
await br.close();
console.log('fails=' + fails); process.exit(fails ? 1 : 0);
