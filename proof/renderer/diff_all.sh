#!/bin/bash
# usage: diff_all.sh <dir> <tagA> <tagB>: differing pixels per shot (ImageMagick AE, fuzz 0), bottom
# 20 rows (the page's frame/fps strip drawn over the canvas) cropped off.
cd $1; for a in $2_s*.png; do b=${a/$2_/$3_}; [ -f $b ] || continue
  h=$(identify -format %h $a); w=$(identify -format %w $a)
  n=$(compare -metric AE <(convert $a -crop ${w}x$((h-20))+0+0 png:-) <(convert $b -crop ${w}x$((h-20))+0+0 png:-) null: 2>&1)
  echo "${a#$2_} $n"; done
