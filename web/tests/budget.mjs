// Time budget in real Chrome: sim, draw, save, restore and a 12-frame rollback, with 4 and 8 fighters, at full speed and at 4x CPU slowdown.
// Headless Chrome draws in software here, so its draw figure is an upper limit, not what a graphics card costs.
import { serve, launch } from './lib.mjs';
const srv = await serve(new URL('..', import.meta.url).pathname); const br = await launch();
console.log('fighters slowdown   sim_ms  draw_ms  save_ms  restore_ms  rollback12_ms  snapshot_MB');
for (const modes of ['aaaa', 'aaaaaaaa']) for (const rate of [1, 4]) {
  const pg = await br.newPage();
  await pg.goto(`http://127.0.0.1:${srv.port}/index.html`);
  await pg.waitForFunction(() => window.eng, null, { timeout: 90000 });
  const cdp = await pg.context().newCDPSession(pg);
  await cdp.send('Emulation.setCPUThrottlingRate', { rate });
  const r = await pg.evaluate(({ modes }) => {
    const e = window.eng, m = e.m;
    e.startOffline({ seed: 5, stage: 0, modes, fighters: [0, 1, 2, 3, 4, 5, 6, 7] });
    for (let i = 0; i < 700; i++) m._StepFrame(0);
    const run = (n, d) => { const t = performance.now(); for (let i = 0; i < n; i++) m._StepFrame(d); return (performance.now() - t) / n; };
    const sim = run(300, 0), withDraw = run(120, 1);
    m._RbInit(14);
    for (let i = 0; i < 40; i++) m._RbStep(0, 0, 0);
    const o = m._malloc(24); m._RbBudget(o);
    const f = new Float64Array(m.HEAPU8.buffer, o, 3);
    return { sim, draw: withDraw - sim, save: f[0], restore: f[1], rb: f[2], snap: m._NetSnapshotSize() / 1048576 };
  }, { modes });
  console.log(`${String(modes.length).padEnd(8)} ${String(rate + 'x').padEnd(9)} ${r.sim.toFixed(2).padStart(6)} ${r.draw.toFixed(2).padStart(8)} ${r.save.toFixed(3).padStart(8)} ${r.restore.toFixed(3).padStart(11)} ${r.rb.toFixed(2).padStart(14)} ${r.snap.toFixed(2).padStart(12)}`);
  await cdp.send('Emulation.setCPUThrottlingRate', { rate: 1 }); await pg.close();
}
await br.close(); srv.close();
