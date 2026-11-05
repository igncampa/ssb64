// usage: node shots.mjs outdir stage fighters waitframes...
import { serve, launch } from './lib.mjs';
const [out, stage, fighters, ...frames] = process.argv.slice(2);
const srv = await serve(new URL('..', import.meta.url).pathname);
const br = await launch(); const pg = await br.newPage({ viewport: { width: 1000, height: 760 } });
pg.on('console', m => console.log('page:', m.text().slice(0, 300))); pg.on('pageerror', e => console.log('pageerror:', e.message.slice(0, 400)));
await pg.goto(`http://127.0.0.1:${srv.port}/index.html?netN=1&stage=${stage}&fighters=${fighters}&modes=${process.env.MODES||'hhhh'}`);
for (const f of frames) {
  await pg.waitForFunction(fr => window.eng && window.eng.m._SimFrame() >= fr, +f, { timeout: 60000 });
  await pg.screenshot({ path: `${out}/s${stage}_f${f}.png` });
  console.log('shot', f, await pg.evaluate(() => document.getElementById('strip').textContent));
}
await br.close(); srv.close();
