#!/bin/bash
# The proof bar for the state-machines branch, one heavy suite at a time. Logs in proof/state-machines/.
cd /home/desktop/sb64/ssb64-spec; L=proof/state-machines; mkdir -p $L/shots
run() { local n=$1; shift; echo "== $n start $(date +%T) load $(cut -d' ' -f1 /proc/loadavg)"; "$@" > $L/$n.log 2>&1; echo "== $n rc=$? $(date +%T) $(grep -cE '^ok' $L/$n.log) ok, $(grep -cE '^FAIL' $L/$n.log) FAIL"; }
run cargo     bash -c 'cd relay && cargo test --release --offline'
run net       timeout 1500 node web/tests/net_proof.mjs
run away      timeout 1800 node web/tests/away_proof.mjs
run modes     timeout 900 node web/tests/modes_proof.mjs $L/shots
run lobby     timeout 600 node web/tests/lobby_proof.mjs $L/shots
run lobby8    timeout 600 node web/tests/lobby8_proof.mjs $L/shots
run lobbylook env SITE=web timeout 600 node web/tests/lobby_look.mjs $L/shots
run extras    timeout 2400 node web/tests/extras_proof.mjs
run extraslob timeout 600 node web/tests/extras_lobby_proof.mjs $L/shots
run sound     timeout 600 node web/tests/sound_proof.mjs
run soundold  timeout 600 node web/tests/sound_proof.mjs old=1
run bgm       timeout 900 node web/tests/bgm_proof.mjs
echo "== all done $(date +%T)"
