# Phase 1: headless port. Done, with two items dropped

Ran `scripts/proof_phase1.sh`, in wasm under node: 54 double runs and 89 checks in all, 0 fails (`proof/phase1.log`).

- Same seed twice identical on all 9 stages: 4 scripted, 2 plus 2 CPU, 8 scripted, 8 human-like, 4 plus 4 CPU.
- Different seed differs at the first hash.
- Three matches in one process each equal a standalone run.
- Two memory fill patterns identical. Draw on and off identical. Audio on and off identical.
- Flipped button at frame 900 first shows in the hash at frame 900.
- Largest arena use in these runs: 3,077,008 bytes of 8 MB. Data file: 21,931,872 bytes.

Changed from the plan, recorded in section 6:
- Pools are not preallocated. The heap pointer is in the snapshot, so the malloc counter is not a proof here.
- Tools are Rust (`tools/port`), by the owner's language rule, not Python.

Not done:
- Native Linux build and the native vs wasm comparison. Marked informational in the plan; only wasm ships.
