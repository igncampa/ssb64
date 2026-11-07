#!/bin/bash
# Page + node builds of this worktree into its own web/ (git-ignored), with the data image beside it.
cd "$(dirname "$0")/../.." && source ~/sb64/emsdk/emsdk_env.sh >/dev/null 2>&1 || exit 1
make -f Makefile.port -j16 all WEB=web 2>&1 | grep -iE "error|build id" ; cp build/us/port/ssb64.data web/ssb64.data && node tools/port/stamp_build.js web
