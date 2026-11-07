// usage: node table.mjs perf.txt   (lines "<tag> <json>" from perf.mjs) -> a markdown table
import fs from 'node:fs';
const names = { 0: "Peach's Castle", 1: 'Sector Z', 4: 'Hyrule', 6: 'Dream Land', 13: 'Meta Crystal', 14: 'Duel Zone', 16: 'Final Destination' };
console.log('| build | stage | fighters | ms per drawn frame (JS) | interpreter ms | GL submit ms | draws | binds | program switches | GL calls | texture hashes |');
console.log('|---|---|---|---|---|---|---|---|---|---|---|');
for (const l of fs.readFileSync(process.argv[2], 'utf8').split('\n')) {
  const i = l.indexOf(' {'); if (i < 0) continue;
  const w = l.slice(0, i).trim(), o = JSON.parse(l.slice(i + 1));
  console.log(`| ${w} | ${names[o.stage] || o.stage} | ${o.fighters} | ${o.jsStepDrawMs} | ${(o.gfxrunMs - o.rbdrawMs).toFixed(2)} | ${o.rbdrawMs} | ${o.draws} | ${o.texbind} | ${o.progswitch} | ${o.glCallsPerFrame} | ${o.texhash ?? ''} |`);
}
