#!/bin/bash
# Phase 5 proof: snapshot and re-simulation offline. Every K frames restore k back, re-simulate, compare every sim byte.
cd "$(dirname "$0")/.."
R=build/us/port; fails=0
F=("0,1,2,3" "4,5,6,7" "8,9,10,11" "0,5,10,3" "2,7,11,4" "9,1,6,8")
F8=("0,1,2,3,4,5,6,7" "4,5,6,7,8,9,10,11" "8,9,10,11,0,1,2,3" "0,5,10,3,7,2,9,4" "2,7,11,4,6,1,8,0" "9,1,6,8,3,10,5,11")
rb() { node $R/ssb64_run.js $R/ssb64.data "$@" 2>&1 | grep -E '^rb|^budget|FATAL'; }
for st in 0 1 2 3 4 5 6 7 8; do
  for m in aaaa hhcc; do
    o=$(RB=11 RBK=12 rb $st 21 2400 $m "${F[$((st%6))]}")
    case "$o" in *"mismatches 0 "*) echo "ok   stage $st $m $o";; *) echo "FAIL stage $st $m $o"; fails=$((fails+1));; esac
  done
  for m in aaaaaaaa hhhhcccc; do
    o=$(RB=11 RBK=12 rb $st 22 2400 $m "${F8[$((st%6))]}")
    case "$o" in *"mismatches 0 "*) echo "ok   stage $st $m $o";; *) echo "FAIL stage $st $m $o"; fails=$((fails+1));; esac
  done
done
# negative control: a broken restore must fail
o=$(RB=11 RBK=12 RBBREAK=1100 rb 0 21 2400 aaaa)
case "$o" in *"mismatches 0 "*) echo "FAIL negative control passed: $o"; fails=$((fails+1));; *) echo "ok   negative control fails as it must: $o";; esac
# sounds started must equal a run with no rollbacks
for st in 0 4; do
  a=$(RB=11 RBK=12 node $R/ssb64_run.js $R/ssb64.data $st 31 2400 aaaaaaaa "${F8[$st]}" 2>&1 | grep -a "^sounds")
  b=$(SOUNDS=1 node $R/ssb64_run.js $R/ssb64.data $st 31 2400 aaaaaaaa "${F8[$st]}" 2>&1 | grep -a "^sounds")
  if [ -n "$a" ] && [ "$a" = "$b" ]; then echo "ok   stage $st $a with and without rollbacks"; else echo "FAIL stage $st sounds differ: $a / $b"; fails=$((fails+1)); fi
done
# budget at a busy moment
RB=11 RBK=12 RBTIME=1 rb 7 21 3000 hhcc "${F[1]}"
RB=11 RBK=12 RBTIME=1 rb 7 21 3000 hhhhcccc "${F8[1]}"
echo "fails=$fails"; [ $fails = 0 ]
