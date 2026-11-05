#!/bin/bash
# Phase 1 proof: determinism, headless, in wasm under node. Exit 1 on any failure.
cd "$(dirname "$0")/.."
R=build/us/port; T=$(mktemp -d); fails=0; runs=0
run() { node $R/ssb64_run.js $R/ssb64.data "$@" 2>/dev/null | grep -vE '^(boot|match)'; }
check() { # name, args...
  local name=$1; shift
  run "$@" > $T/a; local rc=$?
  run "$@" > $T/b
  runs=$((runs+1))
  local n=$(wc -l < $T/a) last=$(tail -1 $T/a | cut -c1-70)
  if ! grep -q '^end frame' $T/a; then echo "FAIL crash $name: $last"; fails=$((fails+1)); return; fi
  if ! cmp -s $T/a $T/b; then echo "FAIL differ $name: $(diff $T/a $T/b | head -2 | tail -1)"; fails=$((fails+1)); return; fi
  echo "ok   $name lines=$n $last"
}
F=("0,1,2,3" "4,5,6,7" "8,9,10,11" "0,5,10,3" "2,7,11,4" "9,1,6,8")
F8=("0,1,2,3,4,5,6,7" "4,5,6,7,8,9,10,11" "8,9,10,11,0,1,2,3" "0,5,10,3,7,2,9,4" "2,7,11,4,6,1,8,0" "9,1,6,8,3,10,5,11")
for st in 0 1 2 3 4 5 6 7 8; do
  check "stage$st scriptA 4p" $st 1 12000 aaaa "${F[$((st%6))]}"
  check "stage$st human 4p"   $st 2 12000 hhhh "${F[$(((st+1)%6))]}"
  check "stage$st 2p+2cpu"    $st 3 12000 hhcc "${F[$(((st+2)%6))]}"
  check "stage$st scriptA 8p" $st 4 12000 aaaaaaaa "${F8[$((st%6))]}"
  check "stage$st human 8p"   $st 5 12000 hhhhhhhh "${F8[$(((st+1)%6))]}"
  check "stage$st 4p+4cpu"    $st 6 12000 hhhhcccc "${F8[$(((st+2)%6))]}"
done
# different seed differs from the first hashes on
node $R/ssb64_run.js $R/ssb64.data 0 1 600 aaaa 2>/dev/null | grep -E '^[0-9]+ ' > $T/s1
node $R/ssb64_run.js $R/ssb64.data 0 2 600 aaaa 2>/dev/null | grep -E '^[0-9]+ ' > $T/s2
if [ "$(head -1 $T/s1)" != "$(head -1 $T/s2)" ]; then echo "ok   seed changes first hash: $(head -1 $T/s1) / $(head -1 $T/s2)"; else echo "FAIL seed does not change first hash"; fails=$((fails+1)); fi
# match after match on one process equals a standalone match
node $R/ssb64_run.js $R/ssb64.data 5 7 3000 aaaa "0,1,2,3" 1 2>/dev/null | grep -E '^[0-9]+ ' > $T/one
node $R/ssb64_run.js $R/ssb64.data 5 7 3000 aaaa "0,1,2,3" 3 2>/dev/null | awk '/^match/{m=$2} /^[0-9]+ /{print > "'$T'/m" m}'
for m in 0 1 2; do if cmp -s $T/one $T/m$m; then echo "ok   match $m in one process equals standalone"; else echo "FAIL match $m differs: $(diff $T/one $T/m$m | head -2 | tail -1)"; fails=$((fails+1)); fi; done
# drawing must not change the sim: full display list interpretation on and off
for st in 0 1 2 3 4 5 6 7 8; do
  run $st 11 12000 hhhh "${F[$((st%6))]}" 1 0 > $T/nd
  run $st 11 12000 hhhh "${F[$((st%6))]}" 1 1 > $T/dr
  if cmp -s $T/nd $T/dr; then echo "ok   draw on and off identical stage $st"; else echo "FAIL draw changes sim stage $st: $(diff $T/nd $T/dr | head -2 | tail -1)"; fails=$((fails+1)); fi
done
# audio must not change the sim
for st in 0 1 2 3 4 5 6 7 8; do
  run $st 13 12000 hhhh "${F[$((st%6))]}" > $T/au; NOAUDIO=1 run $st 13 12000 hhhh "${F[$((st%6))]}" > $T/na
  if cmp -s $T/au $T/na; then echo "ok   audio on and off identical stage $st"; else echo "FAIL audio changes sim stage $st: $(diff $T/au $T/na | head -2 | tail -1)"; fails=$((fails+1)); fi
done
# uninitialised memory: two fill patterns for the arena must give the same run
for st in 0 3 6; do
  POISON=165 run $st 12 6000 hhcc > $T/p1; POISON=90 run $st 12 6000 hhcc > $T/p2
  if cmp -s $T/p1 $T/p2; then echo "ok   poison patterns identical stage $st"; else echo "FAIL poison stage $st: $(diff $T/p1 $T/p2 | head -2 | tail -1)"; fails=$((fails+1)); fi
done
# negative control: one flipped button must show up in the hash, and not before it
run 0 1 3000 aaaa > $T/c0; FLIP=900 run 0 1 3000 aaaa > $T/c1
first=$(diff $T/c0 $T/c1 | grep '^<' | head -1 | awk '{print $2}')
if [ -n "$first" ] && [ "$first" -ge 900 ] && [ "$first" -le 930 ]; then echo "ok   flipped button at 900 first seen in hash at $first"; else echo "FAIL negative control: first difference at '$first'"; fails=$((fails+1)); fi
# the same side-effect checks with 8 fighters
for st in 0 4 8; do
  run $st 14 6000 hhhhhhhh "${F8[$((st%6))]}" 1 0 > $T/nd; run $st 14 6000 hhhhhhhh "${F8[$((st%6))]}" 1 1 > $T/dr
  if cmp -s $T/nd $T/dr; then echo "ok   8p draw on and off identical stage $st"; else echo "FAIL 8p draw changes sim stage $st"; fails=$((fails+1)); fi
  run $st 15 6000 hhhhhhhh "${F8[$((st%6))]}" > $T/au; NOAUDIO=1 run $st 15 6000 hhhhhhhh "${F8[$((st%6))]}" > $T/na
  if cmp -s $T/au $T/na; then echo "ok   8p audio on and off identical stage $st"; else echo "FAIL 8p audio changes sim stage $st"; fails=$((fails+1)); fi
  POISON=165 run $st 16 6000 hhhhcccc > $T/p1; POISON=90 run $st 16 6000 hhhhcccc > $T/p2
  if cmp -s $T/p1 $T/p2; then echo "ok   8p poison patterns identical stage $st"; else echo "FAIL 8p poison stage $st"; fails=$((fails+1)); fi
done
echo "runs=$runs fails=$fails"; rm -rf $T; [ $fails = 0 ]
