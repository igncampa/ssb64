// usage: node probe.mjs outdir stage fighter keys...   keys: "X@120" press key at frame, shot@frame
import { serve, launch } from './lib.mjs';
import fs from 'node:fs';
const [out, stage, fighter, ...steps] = process.argv.slice(2);
const srv = await serve(process.env.SITE || new URL('..', import.meta.url).pathname);
const br = await launch(); const [vw, vh] = (process.env.VP || '960x720').split('x').map(Number); const pg = await br.newPage({ viewport: { width: vw, height: vh } });
const trace = []; pg.on('console', m => { const t = m.text(); if (t.startsWith('GT')) trace.push(t); });
pg.on('pageerror', e => console.log('pageerror:', e.message.slice(0, 400)));
await pg.goto(`http://127.0.0.1:${srv.port}/index.html?netN=1&stage=${stage}&fighters=${fighter},0&modes=${process.env.MODES || 'kn'}`);
const at = f => pg.waitForFunction(fr => window.eng && window.eng.m._SimFrame() >= fr, f, { timeout: 120000 });
for (const s of steps) {
  const [what, f] = s.split('@');
  await at(+f);
  if (what === 'shot') { await pg.locator('#game').screenshot({ path: `${out}/st${stage}_f${fighter}_${f}${process.env.VP ? '_' + process.env.VP : ''}.png` }); console.log('shot', f); }
  else if (what === "rec") { await pg.evaluate(() => { const e = window.eng; e.__rec = []; const pump = e.pumpAudio.bind(e); e.pumpAudio = function () { const m = this.m, head = m._AudioHead(); const ring = m._AudioRing() >> 1, size = m._AudioRingFrames(); const n = Math.min(head - (this.__t ?? head), 8192); for (let i = 0; i < n; i++) { const p = (((this.__t) + i) & (size - 1)) * 2; this.__rec.push(m.HEAP16[ring + p]); } this.__rec.push(-32768, m._SimFrame()); this.__t = head; return pump(); }; }); }
  else if (what === "save") { const pcm = await pg.evaluate(() => window.eng.__rec); fs.writeFileSync(`${out}/audio_${f}.json`, JSON.stringify(pcm)); console.log("audio samples", pcm.length); }
  else if (what === 'trace') { await pg.evaluate(() => window.eng.m._GfxTrace(3)); await pg.waitForTimeout(1500); (await import('node:fs')).writeFileSync(`${out}/trace_${f}.txt`, trace.join('\n')); console.log('trace', trace.length); trace.length = 0; }
  else if (what.startsWith('+')) await pg.keyboard.down(what.slice(1));
  else if (what.startsWith('-')) await pg.keyboard.up(what.slice(1));
  else { await pg.keyboard.down(what); await pg.waitForTimeout(80); await pg.keyboard.up(what); }
}
await br.close(); srv.close();
