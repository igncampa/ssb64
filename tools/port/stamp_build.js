// Stamp the build id into web/build.js. The relay refuses a join with a different id.
// The id covers the wasm and the page's own code, so a page and a relay always speak the same protocol.
const fs = require('fs'), c = require('crypto');
const dir = process.argv[2] || 'web', src = process.argv[3] || 'web';
const h = c.createHash('sha256').update(fs.readFileSync(dir + '/ssb64.wasm'));
for (const f of ['index.html', 'lobby.js', 'net.js', 'engine.js', 'input.js', 'items.js', 'art.js', 'audio-worklet.js']) if (fs.existsSync(src + '/' + f)) h.update(f).update(fs.readFileSync(src + '/' + f));
const id = h.digest('hex').slice(0, 16);
const data = fs.existsSync('build/us/port/ssb64.data') ? c.createHash('sha256').update(fs.readFileSync('build/us/port/ssb64.data')).digest('hex').slice(0, 16) : '';
fs.writeFileSync(dir + '/build.js', `export const BUILD_ID = "${id}"; export const DATA_HASH = "${data}";\n`);
console.log('build id', id, 'data', data);
