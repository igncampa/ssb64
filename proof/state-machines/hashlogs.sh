#!/bin/bash
# Usage: hashlogs.sh <ssb64_run.js> <ssb64.data> <outdir>. Writes one hash log per phase-1 seed run.
J=$1; D=$2; O=$3; mkdir -p $O
run() { node $J $D "$@" 2>/dev/null | grep -vE '^(boot|match)'; }
F=("0,1,2,3" "4,5,6,7" "8,9,10,11" "0,5,10,3" "2,7,11,4" "9,1,6,8")
F8=("0,1,2,3,4,5,6,7" "4,5,6,7,8,9,10,11" "8,9,10,11,0,1,2,3" "0,5,10,3,7,2,9,4" "2,7,11,4,6,1,8,0" "9,1,6,8,3,10,5,11")
for st in 0 1 2 3 4 5 6 7 8; do
  run $st 1 12000 aaaa "${F[$((st%6))]}" > $O/s${st}_a4 &
  run $st 2 12000 hhhh "${F[$(((st+1)%6))]}" > $O/s${st}_h4 &
  run $st 3 12000 hhcc "${F[$(((st+2)%6))]}" > $O/s${st}_hc &
  run $st 4 12000 aaaaaaaa "${F8[$((st%6))]}" > $O/s${st}_a8 &
  run $st 5 12000 hhhhhhhh "${F8[$(((st+1)%6))]}" > $O/s${st}_h8 &
  run $st 6 12000 hhhhcccc "${F8[$(((st+2)%6))]}" > $O/s${st}_h4c4 &
  wait
done
run 5 7 3000 aaaa "0,1,2,3" 3 > $O/matches3
RB=15 RBK=12 run 0 1 3000 aaaa > $O/rb 2>&1
SOUNDS=1 run 3 9 6000 hhcc > $O/sounds
echo done
