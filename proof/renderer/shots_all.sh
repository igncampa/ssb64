#!/bin/bash
# usage: shots_all.sh <webdir> <outdir> <tag>   env EXTRA (query string for picture settings)
# The fixed shot set of this branch: seven stages, Metal Mario and the polygons, the GO intro.
W=$1; O=$2; T=$3; D=$(cd "$(dirname "$0")" && pwd); mkdir -p $O
run() { node $D/shots.mjs $W $O $T "$@" > $O/$T.$1.log 2>&1; }
run 13 13,0,2,3 250 400 1200 &
run 16 13,14,17,3 250 296 400 1200 &
run 6 0,1,2,3 400 1200 &
run 0 4,5,6,7 400 1200 &
wait
run 4 8,9,10,11 400 1200 &
run 1 1,5,9,11 400 1200 &
run 14 14,15,16,17 400 1200 &
wait
grep -h shot $O/$T.*.log | wc -l
