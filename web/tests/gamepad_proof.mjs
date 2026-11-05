// Gamepad code path with a simulated pad: the browser's Gamepad API is replaced, everything after it is real. No physical controller is involved.
import { serve, launch } from './lib.mjs';
const srv = await serve(new URL('..', import.meta.url).pathname); const br = await launch(); const pg = await br.newPage();
await pg.addInitScript(() => {
  window.fakePad = { axes: [0, 0, 0, 0], buttons: Array.from({ length: 16 }, () => ({ pressed: false })) };
  navigator.getGamepads = () => [window.fakePad, null, null, null];
});
let fails = 0; const check = (n, ok, x = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${n} ${x}`); if (!ok) fails++; };
await pg.goto(`http://127.0.0.1:${srv.port}/index.html?netN=3&stage=6&fighters=0,1&modes=ko`);
await pg.waitForFunction(() => window.eng && window.eng.m._SimFrame() >= 500, null, { timeout: 90000 });
const x = () => pg.evaluate(() => window.eng.m._port_fighter_x(0));
const x0 = await x();
await pg.evaluate(() => { window.fakePad.axes[0] = 1; }); await pg.waitForTimeout(1000); await pg.evaluate(() => { window.fakePad.axes[0] = 0; });
const x1 = await x(); check('stick right moves the fighter right', x1 > x0 + 50, `x ${x0} to ${x1}`);
await pg.evaluate(() => { window.fakePad.axes[0] = -1; }); await pg.waitForTimeout(1000); await pg.evaluate(() => { window.fakePad.axes[0] = 0; });
const x2 = await x(); check('stick left moves the fighter left', x2 < x1 - 50, `x ${x1} to ${x2}`);
const m = await pg.evaluate(async () => { const { readPad } = await import('./input.js'); const out = {}; const p = window.fakePad;
  for (const [name, i] of [['A', 0], ['B', 2], ['Z', 6], ['R', 5], ['START', 9]]) { p.buttons[i].pressed = true; out[name] = readPad(0).buttons; p.buttons[i].pressed = false; }
  return out; });
const want = { A: 0x8000, B: 0x4000, Z: 0x2000, R: 0x0010, START: 0x1000 };
for (const k in want) check(`button ${k} maps to the N64 bit`, m[k] === want[k], m[k].toString(16));
await br.close(); srv.close(); console.log('fails=' + fails); process.exit(fails ? 1 : 0);
