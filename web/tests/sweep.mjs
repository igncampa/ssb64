// Every fighter's specials, smashes and taunt, each on a different stage, as contact sheets for review.
// usage: SITE=<page dir> node sweep.mjs outdir [fighters csv]
import { serve, launch } from './lib.mjs';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
const NAMES = ['Mario', 'Fox', 'DK', 'Samus', 'Luigi', 'Link', 'Yoshi', 'Falcon', 'Kirby', 'Pikachu', 'Jigglypuff', 'Ness'];
const STAGES = ['Castle', 'SectorZ', 'Jungle', 'Zebes', 'Hyrule', 'YoshiIsland', 'DreamLand', 'Saffron', 'Mushroom'];
const out = process.argv[2]; const only = process.argv[3] ? process.argv[3].split(',').map(Number) : NAMES.map((_, i) => i);
fs.mkdirSync(out, { recursive: true });
const srv = await serve(process.env.SITE);
const br = await launch();
// [label, keys held, frames after the press to shoot]
const MOVES = [['B', ['KeyX'], [6, 18, 34]], ['up B', ['ArrowUp', 'KeyX'], [8, 24, 44]], ['down B', ['ArrowDown', 'KeyX'], [8, 24, 44]],
  ['F smash', ['ArrowRight', 'KeyZ'], [10, 18]], ['up smash', ['ArrowUp', 'KeyZ'], [10, 18]], ['down smash', ['ArrowDown', 'KeyZ'], [10]], ['taunt', ['KeyQ'], [24]]];
for (const f of only) {
  const stage = f % 9, tag = `${String(f).padStart(2, '0')}_${NAMES[f]}_${STAGES[stage]}`;
  const pg = await br.newPage({ viewport: { width: 960, height: 720 } });
  const errs = []; pg.on('pageerror', e => errs.push(e.message));
  await pg.goto(`http://127.0.0.1:${srv.port}/index.html?netN=${f + 3}&stage=${stage}&fighters=${f},${(f + 6) % 12}&modes=kc`);
  const frame = () => pg.evaluate(() => window.eng ? window.eng.m._SimFrame() : 0);
  const until = (fr) => pg.waitForFunction(x => window.eng && window.eng.m._SimFrame() >= x, fr, { timeout: 120000 });
  await until(470);
  const shots = [];
  const shot = async (label) => { const p = `${out}/${tag}_${shots.length}.png`; await pg.locator('#game').screenshot({ path: p }); shots.push([p, label]); };
  await shot('start');
  for (const [label, keys, at] of MOVES) {
    const f0 = await frame();
    for (const k of keys) await pg.keyboard.down(k);
    await until(f0 + 3);
    for (const k of keys.slice().reverse()) await pg.keyboard.up(k);
    for (const d of at) { await until(f0 + d); await shot(`${label} +${d}`); }
    await until(f0 + 90);
  }
  await pg.close();
  const args = [];
  for (const [p, l] of shots) args.push('-label', l, p);
  execFileSync('montage', [...args, '-tile', '6x', '-geometry', '320x240+2+2', '-pointsize', '14', '-title', `${NAMES[f]} on ${STAGES[stage]}${errs.length ? ' ERRORS: ' + errs.join(' ').slice(0, 80) : ''}`, `${out}/sheet_${tag}.png`]);
  for (const [p] of shots) fs.unlinkSync(p);
  console.log('sheet', tag, errs.length ? 'errors ' + errs.join(' | ') : '');
}
await br.close(); srv.close();
