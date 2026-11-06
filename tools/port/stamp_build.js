// Stamp the wasm hash into web/build.js. The relay refuses a join with a different id.
const fs = require('fs'), c = require('crypto');
const dir = process.argv[2] || 'web';
const id = c.createHash('sha256').update(fs.readFileSync(dir + '/ssb64.wasm')).digest('hex').slice(0, 16);
const data = fs.existsSync('build/us/port/ssb64.data') ? c.createHash('sha256').update(fs.readFileSync('build/us/port/ssb64.data')).digest('hex').slice(0, 16) : '';
fs.writeFileSync(dir + '/build.js', `export const BUILD_ID = "${id}"; export const DATA_HASH = "${data}";\n`);
console.log('build id', id, 'data', data);
