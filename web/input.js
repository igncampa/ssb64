// Keyboard and Gamepad API to one N64 pad. Stick range is the N64's, about +/-80.
const A = 0x8000, B = 0x4000, Z = 0x2000, START = 0x1000, L = 0x0020, R = 0x0010, CU = 0x0008, CD = 0x0004, CL = 0x0002, CR = 0x0001;
const held = new Set();
addEventListener('keydown', e => { held.add(e.code); if (e.code.startsWith('Arrow') || e.code === 'Space') e.preventDefault(); });
addEventListener('keyup', e => held.delete(e.code));
addEventListener('blur', () => held.clear());
export const KEYMAP = { KeyZ: A, KeyX: B, KeyC: Z, Enter: START, KeyQ: L, KeyE: R, KeyI: CU, KeyK: CD, KeyJ: CL, KeyL: CR };
const axis = v => { const a = Math.abs(v) < 0.15 ? 0 : v; return Math.max(-80, Math.min(80, Math.round(a * 80))); };
export function readPad(index = 0) {
  let buttons = 0, x = 0, y = 0;
  const gp = navigator.getGamepads?.()[index];
  if (gp) {
    const pressed = i => gp.buttons[i]?.pressed;
    if (pressed(0)) buttons |= A; if (pressed(2)) buttons |= B;
    if (pressed(6) || pressed(4)) buttons |= Z; if (pressed(5) || pressed(7)) buttons |= R;
    if (pressed(9)) buttons |= START; if (pressed(1)) buttons |= CD; if (pressed(3)) buttons |= CU;
    x = axis(gp.axes[0] ?? 0); y = -axis(gp.axes[1] ?? 0);
    const cx = gp.axes[2] ?? 0, cy = gp.axes[3] ?? 0;
    if (cx > 0.5) buttons |= CR; if (cx < -0.5) buttons |= CL; if (cy > 0.5) buttons |= CD; if (cy < -0.5) buttons |= CU;
  }
  if (index === 0) {
    for (const k in KEYMAP) if (held.has(k)) buttons |= KEYMAP[k];
    const tilt = held.has('ShiftLeft') ? 40 : 80;
    if (held.has('ArrowLeft') || held.has('KeyA')) x = -tilt;
    if (held.has('ArrowRight') || held.has('KeyD')) x = tilt;
    if (held.has('ArrowUp') || held.has('KeyW')) y = tilt;
    if (held.has('ArrowDown') || held.has('KeyS')) y = -tilt;
  }
  return { buttons, x, y };
}
