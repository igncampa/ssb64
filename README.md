# ssb64

Super Smash Bros. 64 in the browser: online rollback matches for up to 8 human players, built from the decompilation below. Live at https://ssb64.guanavibes.com. You build the game data from your own ROM; the ROM is not in this repository.

- What it is and how it was built: `PLAN.md`, `CHANGES.md`.
- What is still open: `PENDING.md`.
- The state machines of page, client, relay room and rollback core: `spec/state-machines.md`, `relay/src/room.rs`, `proof/state-machines/`.
- Every spec and analysis, and what was implemented or not from each: `SPECS.md`.
- Build: `source scripts/env.sh`, then `tools/port/target/release/packdata .`, then `make -f Makefile.port -j16 all`.
- Run: `PORT=8080 SITE=web BUILD_ID=<id from web/build.js> relay/target/release/ssb64-relay`.
- Prove: `scripts/proof_all.sh`. Package for a server: `scripts/stage.sh <empty-folder>`.

The original N64 build is untouched: all port changes sit behind `#ifdef PORT`.

---

[![Code](https://decomp.dev/VetriTheRetri/ssb-decomp-re/us.svg?mode=shield&measure=code&label=Code)](https://decomp.dev/VetriTheRetri/ssb-decomp-re)
[![Data](https://decomp.dev/VetriTheRetri/ssb-decomp-re/us.svg?mode=shield&measure=data&label=Data)](https://decomp.dev/VetriTheRetri/ssb-decomp-re)

# Super Smash Bros. 64 Decompilation

Want to help? Here's a list of remaining functions: https://VetriTheRetri.github.io/ssb-decomp-re/

## Setup
- SHA1: `e2929e10fccc0aa84e5776227e798abc07cedabf`
- MD5: `f7c52568a31aadf26e14dc2b6416b2ed`

To set up and build the rom, see below

## Clone this branch

`git clone --single-branch --branch main https://github.com/VetriTheRetri/ssb-decomp-re.git`

## Install dependencies

`bash ./installDependencies.sh`

This script will also tell you where to place the ROM in case it's not found in the right location (`<repo root>/baserom.us.z64`). Having the ROM there is required to be able to run `make extract` which extracts assembly and assets from the ROM.

## Extract files, and run the initial build

`make init` or `make clean && make extract && make`

Extracting is required every time the splat yaml (`smashbrothers.us.yaml` of `smashbrothers.jp.yaml`) file is modified or anything inside the `symbols` folder (which is used by splat). So it's recommended to run `make init` every time you pull changes.

## Build the ROM

`make`

or with parallel jobs for faster build: `make -j$(nproc)`

### MacOS specific

- MIPS binutils can be obtained from [macports](https://ports.macports.org/port/mips-elf-binutils/)
- GNU make can be obtained from [macports](https://ports.macports.org/port/gmake/)

## Debugging
There's a few handy scripts to help with debugging non matches after making changes.

| Tool           | Details                                                                |
|----------------|------------------------------------------------------------------------|
|`first_diff.py` | Finds the location, and cause of most diffs from the baserom           |
|`livediff.sh`   | Run it with the function you're looking to do some decomp work on<br />and it will set up a live diffing session where all changes saved will<br />automatically build and udpate the session. |
|`mips_to_c.sh`  | Pass a file, and function to this and have it generate all required<br />context, and a file in m2cfiles folder with the m2c output to start<br  />working with for matching purposes.         |
