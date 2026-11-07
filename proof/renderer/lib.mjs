// Proof helpers for the renderer branch: a static server on a fixed test port (18450-18499), headless
// Chromium from web/tests/lib.mjs, and a WebGL call counter injected before the page's scripts run.
import http from 'node:http'; import fs from 'node:fs'; import path from 'node:path';
import { launch } from '../../web/tests/lib.mjs';
export { launch };
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.mjs': 'text/javascript' };
function listen(dir, port) {
  return new Promise((res, rej) => {
    const s = http.createServer((q, r) => {
      const p = path.join(dir, decodeURIComponent(q.url.split('?')[0]).replace(/\/$/, '/index.html'));
      fs.readFile(p, (e, b) => { if (e) { r.writeHead(404); r.end(); } else { r.writeHead(200, { 'content-type': types[path.extname(p)] || 'application/octet-stream' }); r.end(b); } });
    });
    s.once('error', rej);
    s.listen(port, '127.0.0.1', () => res({ port, close: () => s.close() }));
  });
}
export async function serveOn(dir) {
  const base = +(process.env.PROOF_PORT || 18450);
  for (let i = 0; i < 50; i++) { const p = base + ((i + process.pid) % (18500 - base)); try { return await listen(dir, p); } catch {} }
  throw new Error('no free port in ' + base + '-18499');
}
// Counts every WebGL2 method call the page makes (window.__gl.calls), for the perf table.
export const COUNT_GL = () => {
  window.__gl = { calls: 0 };
  const P = WebGL2RenderingContext.prototype;
  for (const k of Object.getOwnPropertyNames(P)) {
    const d = Object.getOwnPropertyDescriptor(P, k);
    if (!d || typeof d.value !== 'function' || k === 'constructor') continue;
    const f = d.value; P[k] = function (...a) { window.__gl.calls++; return f.apply(this, a); };
  }
};
// Page URL for an offline seeded match; extra carries the picture settings (&aa=0&filter=0&n64=1).
export const url = (port, stage, fighters, modes = 'hhhh', extra = '') => `http://127.0.0.1:${port}/index.html?netN=1&stage=${stage}&fighters=${fighters}&modes=${modes}${extra}`;
