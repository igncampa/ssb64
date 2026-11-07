// usage: node shots.mjs webdir outdir tag stage fighters frames...   env MODES (default hhhh), EXTRA (query)
// One page, the offline seeded match; pauses the page's loop at each frame so the shot is that frame.
import { serveOn, launch, url } from './lib.mjs';
const [dir, out, tag, stage, fighters, ...frames] = process.argv.slice(2);
const srv = await serveOn(dir);
const br = await launch(); const pg = await br.newPage({ viewport: { width: 1000, height: 760 } });
pg.on('pageerror', e => console.log('pageerror:', e.message.slice(0, 400)));
await pg.goto(url(srv.port, stage, fighters, process.env.MODES || 'hhhh', process.env.EXTRA || ''));
await pg.waitForFunction(() => window.eng && window.eng.m._SimFrame() >= 1, null, { timeout: 120000 });
// Take over the loop: step exactly to the frame, draw it, shoot. Same frames on every build.
await pg.evaluate(() => { const m = window.eng.m, orig = m._StepFrame; window.__hold = 1e9; m._StepFrame = d => { const s = m._SimFrame(); if (s >= window.__hold) return 1; return orig(s === window.__hold - 1 ? 1 : d); }; });
for (const f of frames) {
  await pg.evaluate(fr => { window.__hold = fr; }, +f);
  await pg.waitForFunction(fr => window.eng.m._SimFrame() >= fr, +f, { timeout: 180000 });
  await pg.waitForTimeout(150);
  const el = await pg.$('#game');
  await el.screenshot({ path: `${out}/${tag}_s${stage}_f${f}.png` });
  console.log('shot', tag, stage, f, await pg.evaluate(() => window.eng.m._SimFrame()));
}
await br.close(); srv.close();
