// usage: node perf.mjs webdir stage fighters [modes] [fromFrame] [toFrame] [extraQuery]
// proof/gfx/perf.mjs plus WebGL calls per drawn frame (counted in JS, so before and after builds are
// measured the same way) and the live texture counters when the build has them.
import { serveOn, launch, COUNT_GL, url } from './lib.mjs';
const [dir, stage, fighters, modes = 'hhcc', from = '600', to = '1500', extra = ''] = process.argv.slice(2);
const srv = await serveOn(dir);
const br = await launch(); const pg = await br.newPage({ viewport: { width: 1000, height: 760 } });
await pg.addInitScript(COUNT_GL);
pg.on('pageerror', e => console.log('pageerror:', e.message.slice(0, 400)));
await pg.goto(url(srv.port, stage, fighters, modes, extra));
await pg.waitForFunction(() => window.eng && window.eng.m._SimFrame() >= 2, null, { timeout: 120000 });
await pg.evaluate(() => {
  const m = window.eng.m, orig = m._StepFrame;
  window.__w = { n: 0, draw: 0, ms: 0, msDraw: 0, max: 0, t: performance.now(), raf: 0, gl: 0 };
  m._StepFrame = d => { const g0 = window.__gl.calls, t0 = performance.now(); const r = orig(d); const dt = performance.now() - t0; const w = window.__w; w.n++; w.ms += dt; if (d) { w.draw++; w.msDraw += dt; w.gl += window.__gl.calls - g0; if (dt > w.max) w.max = dt; } return r; };
  const tick = () => { window.__w.raf++; requestAnimationFrame(tick); }; requestAnimationFrame(tick);
});
const snap = () => pg.evaluate(() => { const m = window.eng.m; const p = []; for (let i = 0; i < 32; i++) p.push(m._PortProf(i)); return { f: m._SimFrame(), p, w: { ...window.__w, t: performance.now() } }; });
await pg.waitForFunction(fr => window.eng.m._SimFrame() >= fr, +from, { timeout: 120000 });
const a = await snap();
await pg.waitForFunction(fr => window.eng.m._SimFrame() >= fr, +to, { timeout: 180000 });
const b = await snap();
const d = i => b.p[i] - a.p[i];
const frames = b.f - a.f, drawn = d(8 + 0), wallS = (b.w.t - a.w.t) / 1000, nd = b.w.draw - a.w.draw;
const names = ['update', 'dlbuild', 'gfxrun', 'rbdraw', 'texdecode', 'resim', 'step', 'audio'];
const cnt = ['drawn', 'ops', 'tris', 'rects', 'draws', 'vtx', 'texdecode', 'texlookup', 'glupload', 'progswitch', 'texbind', 'bufbytes', 'resimframes', 'simframes', 'blendchg', 'depthchg', 'texhash'];
const out = { stage: +stage, fighters, extra, frames, drawn, wallS: +wallS.toFixed(2), simFps: +(frames / wallS).toFixed(1), rafFps: +((b.w.raf - a.w.raf) / wallS).toFixed(1),
  jsStepMsPerFrame: +((b.w.ms - a.w.ms) / (b.w.n - a.w.n)).toFixed(3), jsStepDrawMs: +((b.w.msDraw - a.w.msDraw) / nd).toFixed(3), jsStepDrawMaxMs: +b.w.max.toFixed(2),
  glCallsPerFrame: Math.round((b.w.gl - a.w.gl) / nd) };
names.forEach((n, i) => out[n + 'Ms'] = +(d(i) / (i === 5 ? Math.max(1, d(8 + 12)) : (i <= 1 || i >= 6) ? frames : drawn)).toFixed(3));
cnt.forEach((n, i) => out[n] = +(d(8 + i) / (n === 'drawn' ? 1 : drawn)).toFixed(0));
// live cache sizes (after this branch's phase 1): absolute values at the end
if (b.p[8 + 17] || b.p[8 + 18]) { out.texLive = b.p[8 + 17]; out.glTexLive = b.p[8 + 18]; out.progLive = b.p[8 + 19]; out.texEvicted = b.p[8 + 20]; }
console.log(JSON.stringify(out));
await br.close(); srv.close();
