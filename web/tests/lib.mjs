// Shared proof harness: local static server, browser launch, screenshots.
import http from 'node:http'; import fs from 'node:fs'; import path from 'node:path';
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
export const { chromium } = require('playwright');
const types = { '.html':'text/html', '.js':'text/javascript', '.wasm':'application/wasm', '.data':'application/octet-stream', '.css':'text/css', '.mjs':'text/javascript' };
export function serve(dir, port = 0) {
  return new Promise(res => {
    const s = http.createServer((q, r) => {
      const p = path.join(dir, decodeURIComponent(q.url.split('?')[0]).replace(/\/$/, '/index.html'));
      fs.readFile(p, (e, b) => { if (e) { r.writeHead(404); r.end(); } else { r.writeHead(200, { 'content-type': types[path.extname(p)] || 'application/octet-stream' }); r.end(b); } });
    }).listen(port, '127.0.0.1', () => res({ port: s.address().port, close: () => s.close() }));
  });
}
export async function launch() {
  return chromium.launch({ executablePath: process.env.SB_CHROME || '/home/desktop/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome', args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-webgl', '--ignore-gpu-blocklist', '--enable-unsafe-swiftshader'] });
}
