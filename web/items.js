// The lobby's item checklist. No DOM here, so proofs use the same code.
// The VS Item Switch (src/mn/mnvsmode/mnvsitemswitch.c): bit = 1 << ITKind (src/it/itdef.h).
// The shell is one switch for both shells, as in the game.
export const ITEMS = [['Beam Sword', 7], ['Home-Run Bat', 8], ['Hammer', 13], ['Fan', 9], ['Motion-Sensor Bomb', 14], ['Bob-omb', 15], ['Bumper', 16],
  ['Shell', 17, 18], ['Poké Ball', 19], ['Ray Gun', 11], ['Fire Flower', 12], ['Star Rod', 10], ['Maxim Tomato', 4], ['Heart', 5], ['Star Man', 6]]
  .map(([name, ...kinds]) => ({ name, bits: kinds.reduce((m, k) => m | (1 << k), 0) }));
export const CONTAINERS = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3); // crate, barrel, capsule, egg
// As mnVSItemSwitchSetItemToggles does it: from the game's default mask (every bit on), clear what is
// switched off; containers stay on while any item is on; every switch off is 0.
export function itemMask(on) {
  if (!on.some(Boolean)) return 0;
  let m = 0xFFFFFFFF;
  ITEMS.forEach((it, i) => { if (!on[i]) m &= ~it.bits; });
  return (m | CONTAINERS) >>> 0;
}
export const itemsFromMask = mask => ITEMS.map(it => ((mask >>> 0) & it.bits) === it.bits);
