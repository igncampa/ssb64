// usage: node shots2.mjs webdir outdir stage fighters waitframes...   (serves webdir)
import { serve, launch } from '/home/desktop/sb64/ssb64-gfx/web/tests/lib.mjs';
const [dir, out, stage, fighters, ...frames] = process.argv.slice(2);
const srv = await serve(dir);
const br = await launch(); const pg = await br.newPage({ viewport: { width: 1000, height: 760 } });
pg.on('console', m => console.log('page:', m.text().slice(0, 300))); pg.on('pageerror', e => console.log('pageerror:', e.message.slice(0, 400)));
await pg.goto(`http://127.0.0.1:${srv.port}/index.html?netN=1&stage=${stage}&fighters=${fighters}&modes=${process.env.MODES||'hhhh'}`);
for (const f of frames) {
  await pg.waitForFunction(fr => window.eng && window.eng.m._SimFrame() >= fr, +f, { timeout: 120000 });
  await pg.screenshot({ path: `${out}/s${stage}_f${f}.png` });
  console.log('shot', f, await pg.evaluate(() => document.getElementById('strip').textContent));
}
await br.close(); srv.close();
