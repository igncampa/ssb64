# Phase 7: deploy and people. Not started on a server

Ready:
- One relay binary serves the page, the data and the game traffic.
- Host page with a NEW LOBBY button (`index.html?host=1`).
- `scripts/stage.sh <empty-dir>` checks the build and makes a folder to copy. It restarts nothing.

Needs the owner:
- A server between the players, with TLS in front.
- Confirming the data file may be served to friends (open question 1 in the plan). It is built from the owner's ROM.
- People to play.

Worth knowing: the relay sends the 22 MB data file uncompressed, so it should be compressed at the TLS layer in front.
