// usage: node blendmap.mjs coverage.txt -> markdown: every (othermode L, cycle type) the game used and what
// f3dex2.c decode_blender + rb_webgl2.c make of it. Mirrors the C; for the proof notes only.
import fs from 'node:fs';
const P = ['IN', 'MEM', 'BL', 'FOG'], A = ['A_IN', 'A_FOG', 'SHADE', '0'], B = ['1-A', 'A_MEM', '1', '0'];
const F = ['1-SRC_ALPHA', 'ONE', 'ONE', 'ZERO'];
const rows = new Map();
for (const l of fs.readFileSync(process.argv[2], 'utf8').split('\n')) {
  const r = l.match(/^(\S+) cc=\S+ oml=(\S+) omh=(\S+) geom=\S+ tex=\S+(?: tx=\S+)? n=(\d+)/); if (!r) continue;
  const cyc = (parseInt(r[3], 16) >> 20) & 3, k = `${r[1]} ${r[2]} ${cyc}`;
  rows.set(k, (rows.get(k) || 0) + +r[4]);
}
console.log('| kind | othermode L | cycle | draws | cycle 1 | cycle 2 | flags | shader | GL |');
console.log('|---|---|---|---|---|---|---|---|---|');
for (const [k, n] of [...rows].sort((a, b) => b[1] - a[1])) {
  const [kind, h, c] = k.split(' '), L = parseInt(h, 16) >>> 0, cyc = +c, two = cyc === 1;
  const cy = s => ({ P: L >>> (30 - s) & 3, A: L >>> (26 - s) & 3, M: L >>> (22 - s) & 3, B: L >>> (18 - s) & 3 });
  const c1 = cy(0), c2 = cy(2), fin = two ? c2 : c1, force = !!(L & 0x4000);
  const str = x => `${P[x.P]}*${A[x.A]} + ${P[x.M]}*${B[x.B]}`;
  const fl = []; if ((L & 3) === 1) fl.push('alpha compare'); if ((L & 3) === 3) fl.push('dither compare'); if (L & 8) fl.push('AA_EN');
  if (L & 0x1000) fl.push('CVG_X_ALPHA (edge discard)'); if (L & 0x2000) fl.push('ALPHA_CVG_SEL'); if (force) fl.push('FORCE_BL');
  fl.push(['ZMODE_OPA', 'ZMODE_INTER', 'ZMODE_XLU', 'ZMODE_DEC'][L >> 10 & 3]); if (L & 4) fl.push('ZS_PRIM');
  const ain = (L & 0x2000) && !(L & 0x1000) ? '1 (coverage)' : 'combined alpha';
  let sh = two ? `cin = ${str(c1).replace(/MEM/g, 'IN')}; ` : '', gl;
  if (cyc >= 2) { sh = 'copy/fill: texel or fill colour'; gl = (cyc === 2 && (L & 0x2000)) ? 'SRC_ALPHA, 1-SRC_ALPHA' : 'off'; }
  else if (!force) { sh += fin.P === 1 ? 'colour off' : `out = ${P[fin.P]}`; gl = fin.P === 1 ? 'colour mask off' : 'off'; }
  else if (fin.P !== 1 && fin.M !== 1) { sh += `out = ${str(fin)}`; gl = 'off'; }
  else if (fin.P !== 1) { sh += `out = ${P[fin.P]}, alpha ${A[fin.A]}`; gl = `SRC_ALPHA, ${F[fin.B]}`; }
  else if (fin.M !== 1) { sh += `out = ${P[fin.M]}, alpha ${A[fin.A]}`; gl = `${F[fin.B]}, SRC_ALPHA`; }
  else { sh += 'colour off'; gl = 'colour mask off'; }
  console.log(`| ${kind} | ${h} | ${['1-cycle', '2-cycle', 'copy', 'fill'][cyc]} | ${n} | ${str(c1)} | ${two ? str(c2) : '-'} | ${fl.join(', ')} | ${sh}; A_IN = ${ain} | ${gl} |`);
}
