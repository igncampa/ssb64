# Phase 3: harness. Done

- Pads driven through the ring as the relay would, identical twice; flipped button seen at its frame (phase 1 log).
- RNG trace names the source line of each draw, for example `src/ft/ftcomputer.c:3685` (`RNGDUMP=<frame>` on the runner).
- Build id stamped in `web/build.js`; the relay refuses a different build; the page checks the data file's hash.
- Debug URL flags work only away from a real lobby. The wasm exports themselves are not locked.
