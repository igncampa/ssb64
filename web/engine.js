// Wrapper around the wasm exports. One 60 Hz fixed-step loop.
import { DATA_HASH } from './build.js';
import { readPad } from './input.js';
const FRAME_MS = 1000 / 60;
export class Engine {
  static wide = (() => { try { return localStorage.getItem('ssb64.wide') !== '0'; } catch { return true; } })();
  // idle, loading, ready or failed, for the page: Engine.load sets it.
  static state = 'idle';
  static load(onStatus = () => {}) {
    Engine.state = 'loading';
    return Engine.boot(onStatus).then(e => { Engine.state = 'ready'; return e; }, x => { Engine.state = 'failed'; throw x; });
  }
  static async boot(onStatus) {
    const { default: SSB64 } = await import('./ssb64.js');
    onStatus('engine'); 
    const m = await SSB64({ canvas: document.getElementById('game') });
    onStatus('game data');
    const res = await fetch(`ssb64.data?v=${DATA_HASH}`);
    if (!res.ok) throw new Error(`game data: HTTP ${res.status}`);
    const buf = new Uint8Array(await res.arrayBuffer());
    const hex = [...new Uint8Array(await crypto.subtle.digest('SHA-256', buf))].slice(0, 8).map(b => b.toString(16).padStart(2, '0')).join('');
    if (hex !== DATA_HASH) throw new Error('game data failed its hash check');
    const p = m._malloc(buf.length);
    m.HEAPU8.set(buf, p);
    m._PortBoot(p, buf.length);
    m.dataPtr = p; // the lobby reads its pictures from the same image (art.js)
    onStatus('ready');
    return new Engine(m);
  }
  constructor(m) { this.m = m; this.pads = [0, 0, 0, 0]; this.running = false; this.tail = 0; this.fitCanvas(); }
  // The picture fills the window from 4:3 to 21:9 (proof/widescreen-spec.md 2.7). Render only: every
  // client simulates the same 4:3 game whatever its shape. A wider or taller window is letterboxed.
  // wide = false keeps the N64's 4:3 picture.
  fitCanvas(wide = Engine.wide) {
    const c = document.getElementById('game'), box = document.getElementById('stage');
    if (!c || !box) return;
    const fit = () => {
      const aw = box.clientWidth || 960, ah = box.clientHeight || 720;
      const A = wide ? Math.min(21 / 9, Math.max(4 / 3, aw / ah)) : 4 / 3;
      const cssH = Math.min(ah, aw / A), cssW = cssH * A;
      const dpr = Math.min(window.devicePixelRatio || 1, 1440 / Math.max(1, cssH));
      const w = Math.round(cssW * dpr), h = Math.round(cssH * dpr);
      c.style.width = cssW + 'px'; c.style.height = cssH + 'px';
      if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
      if (this.m._PortSetCanvas) this.m._PortSetCanvas(w, h);
    };
    this.fit = fit;
    if (!this.ro) { this.ro = new ResizeObserver(() => this.fit()); this.ro.observe(box); }
    fit();
  }
  // none, creating, running, or suspended (a browser that wants a press first).
  get audioState() { return !this.ac ? 'none' : !this.node ? 'creating' : this.ac.state === 'running' ? 'running' : 'suspended'; }
  // Safe to call any number of times: makes the context once, and asks it to run every time it is not.
  async startAudio() {
    this.audioReady ??= (async () => {
      this.ac = new AudioContext({ sampleRate: 32000, latencyHint: 'interactive' });
      await this.ac.audioWorklet.addModule('audio-worklet.js');
      this.node = new AudioWorkletNode(this.ac, 'ring', { outputChannelCount: [2] });
      this.node.connect(this.ac.destination);
      this.tail = this.m._AudioHead();
    })();
    await this.audioReady;
    if (this.ac.state !== 'running') await this.ac.resume().catch(() => {});
  }
  pumpAudio() {
    const m = this.m, head = m._AudioHead();
    if (!this.node) { m._AudioConsumed(head); this.tail = head; return; }
    const ring = m._AudioRing() >> 1, size = m._AudioRingFrames();
    let n = head - this.tail;
    if (n <= 0) return;
    if (n > 8192) { this.tail = head - 8192; n = 8192; }
    const pcm = new Float32Array(n * 2);
    for (let i = 0; i < n; i++) {
      const p = ((this.tail + i) & (size - 1)) * 2;
      pcm[i * 2] = m.HEAP16[ring + p] / 32768; pcm[i * 2 + 1] = m.HEAP16[ring + p + 1] / 32768;
    }
    this.tail = head;
    this.node.port.postMessage(pcm, [pcm.buffer]);
    m._AudioConsumed(head - 533);
  }
  arr(a) { const p = this.m._malloc(a.length * 4); this.m.HEAP32.set(a, p >> 2); return p; }
  start(cfg) {
    const m = this.m, f = v => this.arr(v);
    m._StartMatch(f(cfg.kinds), f(cfg.fighters), f(cfg.costumes), f(cfg.levels), f(cfg.teams), f(cfg.handicaps),
      cfg.stage, cfg.rules, cfg.time, cfg.stocks, cfg.damage, cfg.team ? 1 : 0, cfg.teamAttack ? 1 : 0, cfg.itemRate, cfg.itemToggles >>> 0, cfg.seed);
  }
  startOffline({ seed, stage, modes, fighters }) {
    const kinds = [], local = [];
    for (let i = 0; i < 8; i++) {
      const c = modes[i] ?? 'n';
      kinds.push(c === 'c' ? 1 : c === 'n' ? 2 : 0);
      this.m._SetScriptedSlot(i, c === 'a' ? 1 : c === 'h' ? 2 : 0);
      if (c === 'k') local.push(i);
    }
    this.localSlots = local;
    this.start({ kinds, fighters, costumes: Array(8).fill(0), levels: Array(8).fill(5), teams: [0,1,2,3,4,5,6,7], handicaps: Array(8).fill(9), stage, rules: 1, time: 3, stocks: 3, damage: 100, itemRate: 3, itemToggles: 0xFFFFFFFF, seed });
  }
  // Write this frame's pad for every local slot into the ring, then mark it confirmed.
  writePad(slot, frame, buttons, x, y) {
    const m = this.m, ring = m._PadRing() >> 1, len = m._PadRingLen(), i = ring + (slot * len + (frame & (len - 1))) * 2;
    m.HEAP16[i] = buttons << 16 >> 16;
    m.HEAP16[i + 1] = ((x & 0xFF) << 8 | (y & 0xFF)) << 16 >> 16;
    m.HEAP32[(m._Confirmed() >> 2) + slot] = frame;
  }
  feedLocal() {
    if (!this.localSlots) return;
    const f = this.m._SimFrame();
    this.localSlots.forEach((slot, i) => { const p = readPad(i); this.writePad(slot, f, p.buttons, p.x, p.y); });
  }
  runLoop({ stats, nodraw } = {}) {
    let last = performance.now(), acc = 0, frames = 0, tLog = last;
    const tick = t => {
      acc += Math.min(t - last, 250); last = t;
      let n = 0;
      while (acc >= FRAME_MS && n < 4) { acc -= FRAME_MS; n++; }
      for (let i = 0; i < n; i++) {
        this.feedLocal();
        this.alive = this.m._StepFrame(!nodraw && i === n - 1 ? 1 : 0);
        frames++;
      }
      this.pumpAudio();
      if (stats && t - tLog > 500) { stats.textContent = `frame ${this.m._SimFrame()} ${(frames * 1000 / (t - tLog)).toFixed(0)} fps tris ${this.m._port_gfx_tris?.() ?? ''}`; frames = 0; tLog = t; }
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  }
}
