#!/bin/bash
# usage: hashcmp.sh <baseline dir> <new dir>: how many of the 57 phase-1 hash logs are byte-identical
n=0; t=0; for f in $1/*; do t=$((t+1)); cmp -s $f $2/$(basename $f) && n=$((n+1)) || echo "DIFF $(basename $f)"; done; echo "$n/$t identical"
