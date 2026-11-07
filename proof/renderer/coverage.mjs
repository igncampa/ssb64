// Every draw state the game uses: 9 stages, 8 scripted fighters each so all 12 appear, items on. Prints one merged table.
// usage: SITE=<page dir> node coverage.mjs out.txt [frames]
import { serveOn, launch } from './lib.mjs';
import fs from 'node:fs';
const out = process.argv[2], frames = +(process.argv[3] || 2400);
const srv = await serveOn(process.env.SITE); const br = await launch();
const rows = new Map(); const extra = new Map();
async function one(stage) {
  const pg = await br.newPage({ viewport: { width: 640, height: 480 } });
  const lines = []; pg.on('console', m => { const t = m.text(); if (t.startsWith('COV')) lines.push(t); });
  const fighters = Array.from({ length: 8 }, (_, i) => stage >= 13 ? (stage === 13 ? [13, 0, 2, 3, 14, 15, 16, 17][i] : 14 + (stage + i) % 12) : (stage * 5 + i) % 12).join(',');
  await pg.goto(`http://127.0.0.1:${srv.port}/index.html?netN=${stage + 11}&stage=${stage}&fighters=${fighters}&modes=hhhhhhhh`);
  await pg.waitForFunction(() => window.eng, null, { timeout: 60000 });
  await pg.evaluate(() => window.eng.m._GfxCoverage(1));
  await pg.waitForFunction(f => window.eng.m._SimFrame() >= f || window.eng.m._GameStatus() !== 0 && window.eng.m._SimFrame() > 600, frames, { timeout: frames * 40 + 60000, polling: 1000 }).catch(() => {});
  await pg.evaluate(() => window.eng.m._GfxCoverageDump());
  await pg.waitForFunction(() => true); await new Promise(r => setTimeout(r, 2000));
  for (const l of lines) {
    const m = l.match(/^COV (\S+ cc=\S+ oml=\S+ omh=\S+ geom=\S+ tex=\S+) n=(\d+)/);
    if (m) { const r = rows.get(m[1]) || { n: 0, stages: new Set() }; r.n += +m[2]; r.stages.add(stage); rows.set(m[1], r); }
    else if (!l.startsWith('COVEND')) { const [k, a, b] = l.split(' '); extra.set(k + ' ' + a, (extra.get(k + ' ' + a) || 0) + +b); }
    else console.log('stage', stage, l);
  }
  await pg.close();
}
const stages = (process.env.STAGES || '0,1,2,3,4,5,6,7,8,13,14,16').split(',').map(Number);
await Promise.all([0, 1, 2].map(async (w) => { for (let i = w; i < stages.length; i += 3) await one(stages[i]); }));
const text = [...rows.entries()].sort((a, b) => b[1].n - a[1].n).map(([k, r]) => `${k} n=${r.n} stages=${[...r.stages].join('')}`).join('\n') + '\n' + [...extra.entries()].map(([k, v]) => `${k} ${v}`).join('\n') + '\n';
fs.writeFileSync(out, text); console.log('states', rows.size);
await br.close(); srv.close();
