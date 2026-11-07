// usage: node perf.mjs webdir stage fighters [modes] [fromFrame] [toFrame]
// Loads the offline page, lets its own 60 Hz loop run, and reports per-frame CPU time inside the wasm
// (PortProf phases) plus JS wall time of StepFrame(1) between two sim frames. Headless Chromium on
// SwiftShader: GL time is CPU time here, so rb_draw ms is an upper bound on a real GPU's driver cost.
import { serve, launch } from '/home/desktop/sb64/ssb64-gfx/web/tests/lib.mjs';
const [dir, stage, fighters, modes = 'hhcc', from = '600', to = '1500'] = process.argv.slice(2);
const srv = await serve(dir);
const br = await launch(); const pg = await br.newPage({ viewport: { width: 1000, height: 760 } });
pg.on('pageerror', e => console.log('pageerror:', e.message.slice(0, 400)));
await pg.goto(`http://127.0.0.1:${srv.port}/index.html?netN=1&stage=${stage}&fighters=${fighters}&modes=${modes}`);
await pg.waitForFunction(() => window.eng && window.eng.m._SimFrame() >= 2, null, { timeout: 120000 });
await pg.evaluate(() => {
  const m = window.eng.m, orig = m._StepFrame;
  window.__w = { n: 0, draw: 0, ms: 0, msDraw: 0, max: 0, t: performance.now(), raf: 0 };
  m._StepFrame = d => { const t0 = performance.now(); const r = orig(d); const dt = performance.now() - t0; const w = window.__w; w.n++; w.ms += dt; if (d) { w.draw++; w.msDraw += dt; if (dt > w.max) w.max = dt; } return r; };
  const tick = () => { window.__w.raf++; requestAnimationFrame(tick); }; requestAnimationFrame(tick);
});
const snap = () => pg.evaluate(() => { const m = window.eng.m; const p = []; for (let i = 0; i < 24; i++) p.push(m._PortProf(i)); return { f: m._SimFrame(), p, w: { ...window.__w, t: performance.now() } }; });
await pg.waitForFunction(fr => window.eng.m._SimFrame() >= fr, +from, { timeout: 120000 });
const a = await snap();
await pg.waitForFunction(fr => window.eng.m._SimFrame() >= fr, +to, { timeout: 180000 });
const b = await snap();
const d = i => b.p[i] - a.p[i];
const frames = b.f - a.f, drawn = d(8 + 0), wallS = (b.w.t - a.w.t) / 1000;
const names = ['update', 'dlbuild', 'gfxrun', 'rbdraw', 'texdecode', 'resim', 'step', 'audio'];
const cnt = ['drawn', 'ops', 'tris', 'rects', 'draws', 'vtx', 'texdecode', 'texlookup', 'glupload', 'progswitch', 'texbind', 'bufbytes', 'resimframes', 'simframes', 'blendchg', 'depthchg'];
const out = { stage: +stage, fighters, frames, drawn, wallS: +wallS.toFixed(2), simFps: +(frames / wallS).toFixed(1), rafFps: +((b.w.raf - a.w.raf) / wallS).toFixed(1),
  jsStepMsPerFrame: +((b.w.ms - a.w.ms) / (b.w.n - a.w.n)).toFixed(3), jsStepDrawMs: +((b.w.msDraw - a.w.msDraw) / (b.w.draw - a.w.draw)).toFixed(3), jsStepDrawMaxMs: +b.w.max.toFixed(2) };
names.forEach((n, i) => out[n + 'Ms'] = +(d(i) / (i === 5 ? Math.max(1, d(8 + 12)) : (i <= 1 || i >= 6) ? frames : drawn)).toFixed(3));
cnt.forEach((n, i) => out[n] = +(d(8 + i) / (n === 'drawn' ? 1 : drawn)).toFixed(0));
console.log(JSON.stringify(out));
await br.close(); srv.close();
