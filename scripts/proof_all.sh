#!/bin/bash
# Every automated proof, one after another, logs in proof/. Network and browser runs go first, alone, because they keep real time.
cd "$(dirname "$0")/.."; mkdir -p proof/shots; rc=0
run() { local n=$1; shift; "$@" > proof/$n.log 2>&1; local r=$?; echo "$n rc=$r"; [ $r = 0 ] || rc=1; }
run net      timeout 900 node web/tests/net_proof.mjs
run lobby    timeout 500 node web/tests/lobby_proof.mjs proof/shots
run lobby8   timeout 500 node web/tests/lobby8_proof.mjs proof/shots
run gamepad  timeout 200 node web/tests/gamepad_proof.mjs
run budget   timeout 300 node web/tests/budget.mjs
run soak4    timeout 1200 node web/tests/net_proof.mjs soak4
run soak8    timeout 1200 node web/tests/net_proof.mjs soak8
run phase5   bash scripts/proof_phase5.sh
run bounds   bash scripts/proof_bounds.sh
run phase1   bash scripts/proof_phase1.sh
echo "all rc=$rc"; exit $rc
