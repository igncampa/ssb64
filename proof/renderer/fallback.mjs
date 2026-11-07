// usage: node fallback.mjs webdir outdir: the picture settings on a WebGL2 without multisampling and with a
// framebuffer that never completes. Each case must still draw the match (canvas not black) and say why.
import { serveOn, launch, url } from './lib.mjs';
import { execFileSync } from 'node:child_process';
const [dir, out] = process.argv.slice(2);
const srv = await serveOn(dir); const br = await launch();
let fails = 0, checks = 0;
const cases = [
  ['normal', () => {}, ''],
  ['no-msaa', () => { const g = WebGL2RenderingContext.prototype.getParameter; WebGL2RenderingContext.prototype.getParameter = function (p) { return p === 0x8D57 ? 0 : g.call(this, p); }; }, ''],
  ['no-msaa-n64', () => { const g = WebGL2RenderingContext.prototype.getParameter; WebGL2RenderingContext.prototype.getParameter = function (p) { return p === 0x8D57 ? 0 : g.call(this, p); }; }, '&picture=n64'],
  ['fbo-incomplete', () => { WebGL2RenderingContext.prototype.checkFramebufferStatus = () => 0x8CD6; }, ''],
  ['fbo-incomplete-n64', () => { WebGL2RenderingContext.prototype.checkFramebufferStatus = () => 0x8CD6; }, '&picture=n64'],
];
for (const [name, init, extra] of cases) {
  const pg = await br.newPage({ viewport: { width: 1000, height: 760 } }); const log = [];
  pg.on('console', m => log.push(m.text()));
  await pg.addInitScript(init);
  await pg.goto(url(srv.port, 16, '13,14,17,3', 'hhhh', extra));
  await pg.waitForFunction(() => window.eng && window.eng.m._SimFrame() >= 400, null, { timeout: 120000 });
  const f = `${out}/fallback_${name}.png`; await (await pg.$('#game')).screenshot({ path: f });
  const mean = +execFileSync('convert', [f, '-format', '%[fx:mean]', 'info:']).toString();
  const said = log.some(l => l.includes('framebuffer target unavailable'));
  const ok = mean > 0.08 && (name.startsWith('fbo-incomplete') ? said : true);
  checks++; if (!ok) fails++;
  console.log(`${ok ? 'ok' : 'FAIL'} ${name}: mean ${mean.toFixed(3)}${said ? ', fell back to the canvas' : ''}`);
  await pg.close();
}
console.log(`checks=${checks} fails=${fails}`);
await br.close(); srv.close(); process.exit(fails ? 1 : 0);
