#!/bin/bash
# The browser and relay suites against this worktree's web/ and relay, one at a time, test ports 18400+.
# Logs in $1 (default proof/renderer/suites). Prints each suite's ok/FAIL line counts and exit code.
cd "$(dirname "$0")/../.."; L=${1:-proof/renderer/suites}; mkdir -p $L/shots; export PORT_BASE=18400
[ -x relay/target/release/ssb64-relay ] || { echo "relay not built"; exit 1; }
run() { local n=$1; shift; echo "== $n start $(date +%T) load $(cut -d' ' -f1 /proc/loadavg)"; "$@" > $L/$n.log 2>&1; echo "== $n rc=$? $(date +%T) $(grep -cE '^ *ok' $L/$n.log) ok, $(grep -cE '^ *FAIL' $L/$n.log) FAIL"; }
for s in ${SUITES:-net away modes lobby lobby8 lobbylook extras extraslob sound bgm}; do case $s in
  net)       run net       timeout 1500 node web/tests/net_proof.mjs ;;
  away)      run away      timeout 1800 node web/tests/away_proof.mjs ;;
  modes)     run modes     timeout 900 node web/tests/modes_proof.mjs $L/shots ;;
  lobby)     run lobby     timeout 600 node web/tests/lobby_proof.mjs $L/shots ;;
  lobby8)    run lobby8    timeout 600 node web/tests/lobby8_proof.mjs $L/shots ;;
  lobbylook) run lobbylook env SITE=web timeout 600 node web/tests/lobby_look.mjs $L/shots ;;
  extras)    run extras    timeout 2400 node web/tests/extras_proof.mjs ;;
  extraslob) run extraslob timeout 600 node web/tests/extras_lobby_proof.mjs $L/shots ;;
  sound)     run sound     timeout 600 node web/tests/sound_proof.mjs ;;
  bgm)       run bgm       timeout 900 node web/tests/bgm_proof.mjs ;;
esac; done
echo "== all done $(date +%T)"
