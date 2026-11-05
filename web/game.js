// Debug path: a seeded offline match from URL flags. Refused in a real lobby because a lobby never loads this module.
import { Engine } from './engine.js';
export async function startOffline(q) {
  const status = document.getElementById('strip');
  document.getElementById('lobby').hidden = true; status.hidden = false;
  const eng = await Engine.load(m => { status.textContent = m; });
  window.eng = eng;
  const seed = +q.get('netN') || 1, stage = +(q.get('stage') || 0), modes = q.get('modes') || 'kccc';
  const fighters = (q.get('fighters') || '0,1,2,3,4,5,6,7').split(',').map(Number);
  while (fighters.length < 8) fighters.push(fighters.length);
  eng.startOffline({ seed, stage, modes, fighters });
  eng.runLoop({ stats: status, nodraw: q.has('nodraw') });
  addEventListener('pointerdown', () => eng.startAudio(), { once: true });
  addEventListener('keydown', () => eng.startAudio(), { once: true });
}
