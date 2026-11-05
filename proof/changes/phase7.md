# Phase 7: deploy and people. Live; no real players yet

Live at https://ssb64.guanavibes.com since 4 Nov 2026: a Cloudflare tunnel, set up by the owner's side, forwards to the relay on port 8080 of the owner's desktop.

Checked through the live address, with two browser pages on that same desktop: secure sockets (wss), secure context, lobby, join, start, a real key press moving a fighter, every hash agreeing, about 105 ms round trip. This is not the real internet match: both pages are on one machine.

COOP and COEP headers are not needed: the wasm build uses no threads or SharedArrayBuffer.

Ready:
- One relay binary serves the page, the data and the game traffic.
- Host page with a NEW LOBBY button (`index.html?host=1`).
- `scripts/stage.sh <empty-dir>` checks the build and makes a folder to copy. It restarts nothing.

Needs the owner:
- A host secret. The relay runs with none, so anyone with the address can make lobbies.
- A decision on the data file. It is built from the owner's ROM, and anyone with the address can now download it (open question 1 in the plan).
- Keeping the relay running: it was started by hand from a working session, so it will not survive a reboot.
- People to play, from their own locations, for the proofs of phases 4, 6 and 7.

Worth knowing: the 22 MB data file reaches browsers uncompressed through the tunnel, at about 10 s to reach a lobby on this connection. The relay also serves everything under `web/`, including `web/tests/`, which holds nothing secret. The tunnel adds latency for players on this desktop too.
