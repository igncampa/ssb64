#!/bin/bash
# Build a deployable folder. Checks that wasm, data, page and relay belong together, then stops.
# It never restarts anything: a person copies the folder to the server and runs the restart.
#   scripts/stage.sh <empty-output-dir>
set -e
cd "$(dirname "$0")/.."
OUT=${1:?usage: scripts/stage.sh <empty-output-dir>}
[ ! -e "$OUT" ] || [ -z "$(ls -A "$OUT")" ] || { echo "refusing: $OUT is not empty"; exit 1; }
node tools/port/stamp_build.js >/dev/null
ID=$(sed -n 's/.*BUILD_ID = "\([0-9a-f]*\)".*/\1/p' web/build.js)
DH=$(sed -n 's/.*DATA_HASH = "\([0-9a-f]*\)".*/\1/p' web/build.js)
W=$(sha256sum web/ssb64.wasm | cut -c1-16)
D=$(sha256sum web/ssb64.data | cut -c1-16)
[ "$ID" = "$W" ] || { echo "FAIL build id $ID does not match wasm $W"; exit 1; }
[ "$DH" = "$D" ] || { echo "FAIL data hash $DH does not match web/ssb64.data $D (copy the new data file)"; exit 1; }
cmp -s web/ssb64.data build/us/port/ssb64.data || { echo "FAIL web/ssb64.data is not the built data file"; exit 1; }
[ -x relay/target/release/sb64-relay ] || { echo "FAIL relay is not built"; exit 1; }
[ relay/target/release/sb64-relay -nt relay/src/main.rs ] || { echo "FAIL relay is older than its source"; exit 1; }
mkdir -p "$OUT/web"
cp web/index.html web/*.js web/ssb64.wasm web/ssb64.data "$OUT/web/"
cp relay/target/release/sb64-relay "$OUT/"
cat > "$OUT/run.env" <<ENV
# Fill in RELAY_SECRET before starting. TLS belongs in front of this port.
PORT=8080
SITE=web
BUILD_ID=$ID
RELAY_SECRET=
ENV
echo "staged build $ID data $DH in $OUT"
echo "next, by a person: copy the folder to the server, set RELAY_SECRET in run.env, start sb64-relay with those variables behind TLS"
