#!/bin/bash
# 8-player proof: full matches with every statically sized array access checked.
# Accesses the original game makes with any player count are listed; anything else fails.
source "$(dirname "$0")/env.sh" >/dev/null 2>&1
cd "$(dirname "$0")/.."
B=build/us/portb; D=build/us/port/ssb64.data; T=$(mktemp -d)
mkdir -p $B/gen; cp -r build/us/port/gen/* $B/gen/
make -f Makefile.port -j12 BUILD=$B OPT="-O1 -fsanitize=array-bounds" run 2>&1 | grep -E 'error|\*\*\*'
run() { timeout 500 env $3 node $B/ssb64_run.js $D $1 $2 5400 cccccccc $4 1 1 > $T/$1_$2.log 2>&1; grep -aq '^end frame' $T/$1_$2.log || echo "FAIL crash stage $1 seed $2"; }
for st in 0 1 2 3 4 5 6 7 8; do run $st $((st+100)) X=1 4,5,6,7,8,9,10,11 & done; wait
for st in 0 3 6 8; do run $st $((st+200)) TEAM=1 0,1,2,3,4,5,6,7 & done; run 2 300 X=1 0,0,0,0,0,0,0,0 & run 5 301 X=1 7,7,7,7,7,7,7,7 & wait
# Known and unrelated to player count: shade 0 reads the entry before the shade table; script stacks overlap by design.
ALLOWED='src/ft/ftmanager.c:(724|725|726)|src/ft/ftmain.c:(500|510|1015|1016|1021|1023|1030|1035)'
cat $T/*.log | grep -a '^BOUNDS' | sed 's/ index.*//' | sort -u > $T/all
grep -vE " ($ALLOWED)\$" $T/all > $T/bad
cat $T/all; fails=$(grep -c FAIL <(cat $T/*.log) ); n=$(wc -l < $T/bad)
echo "matches=$(grep -al '^end frame' $T/*.log | wc -l) unexpected=$n"; rm -rf $T; [ "$n" = 0 ]
