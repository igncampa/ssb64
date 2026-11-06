// The page's netcode. The relay is the clock and the pad authority; the core is in wasm (port/net/rollback.c).
// No DOM here, so proofs can run the same code without a browser.
const TICK_MS = 1000 / 60;

export function startMatch(m, cfg, go, localSlot, { window: W = 18, lockstep = false } = {}) {
  const arr = v => { const p = m._malloc(v.length * 4); m.HEAP32.set(v, p >> 2); return p; };
  const present = new Set(go.slots.map(s => s.slot));
  const SLOTS = 8, slots8 = (a, d) => Array.from({ length: SLOTS }, (_, i) => a?.[i] ?? d(i));
  const kinds = slots8(null, i => present.has(i) ? 0 : (cfg.cpu?.[i] ? 1 : 2));
  // each racer's own pick wins over the host's defaults
  const fighters = slots8(cfg.fighters, i => i), costumes = slots8(cfg.costumes, () => 0), teams = slots8(cfg.teams, i => i);
  // A pick is an FTKind: 0 to 11, 13 Metal Mario, 14 to 25 the polygons, 26 Giant DK. Anything else
  // (12 is Master Hand) keeps the slot's default. Every page applies the same rule to the same picks.
  const playable = k => Number.isInteger(k) && k >= 0 && k <= 26 && k !== 12;
  for (const s of go.slots) if (s.pick) { fighters[s.slot] = playable(s.pick.fighter) ? s.pick.fighter : fighters[s.slot]; costumes[s.slot] = s.pick.costume ?? costumes[s.slot]; teams[s.slot] = s.pick.team ?? teams[s.slot]; }
  m._NetInit(W, 1 << localSlot, lockstep ? 1 : 0);
  m._StartMatch(arr(kinds), arr(fighters), arr(costumes), arr(slots8(cfg.levels, () => 5)),
    arr(teams), arr(slots8(cfg.handicaps, () => 9)), cfg.stage, cfg.rules ?? 1, cfg.time ?? 3, cfg.stocks ?? 2,
    cfg.damage ?? 100, cfg.team ? 1 : 0, cfg.teamAttack ? 1 : 0, cfg.itemRate ?? 3, (cfg.itemToggles ?? 0xFFFFFFFF) >>> 0, go.seed);
}

export class NetClient {
  constructor({ m, url, padFn, lagUp = 0, lagDown = 0, jitter = 0, window: W = 18, lockstep = false, delay = 3, onEvent = () => {} }) {
    Object.assign(this, { m, url, padFn, lagUp, lagDown, jitter, W, lockstep, delay, onEvent });
    this.upQ = { due: 0 }; this.downQ = { due: 0 }; this.rtts = []; this.state = 'lobby'; this.slot = -1; this.sentTop = -1; this.reportedRunning = false; this.lastPing = 0;
    this.scratch = m._malloc(64); this.packBuf = m._malloc(64); this.hOut = m._malloc(8);
    this.desync = null; this.hashes = []; this.gaps = 0; this.finished = false; this.clockSeen = false;
  }
  connect() {
    this.ws = new WebSocket(this.url); this.ws.binaryType = 'arraybuffer';
    this.ws.onmessage = e => this.lagDown || this.jitter ? this.delayed(this.downQ, this.lagDown, () => this.onMessage(e.data)) : this.onMessage(e.data);
    this.ws.onclose = () => {
      this.onEvent({ t: 'closed' });
      if (this.state === 'racing' && !this.finished && !this.gaveUp) setTimeout(() => this.rejoin(), 500);
    };
    return new Promise((res, rej) => { this.ws.onopen = res; this.ws.onerror = () => rej(new Error('relay unreachable')); });
  }
  async rejoin() {
    try { await this.connect(); this.json({ t: 'resume', from: this.m._NetTruthTop() + 1 }); this.onEvent({ t: 'rejoined' }); }
    catch { setTimeout(() => this.rejoin(), 1000); }
  }
  send(data) {
    const go = () => { if (this.ws.readyState === 1) this.ws.send(data); };
    if (this.lagUp || this.jitter) this.delayed(this.upQ, this.lagUp, go); else go();
  }
  // Simulated lag for proofs. TCP never reorders, so neither does this.
  delayed(q, lag, fn) {
    q.due = Math.max(q.due, performance.now() + lag + Math.random() * this.jitter);
    (q.items ??= []).push([q.due, fn]);
    const pump = () => {
      while (q.items.length && q.items[0][0] <= performance.now()) q.items.shift()[1]();
      if (q.items.length) { q.timer = setTimeout(pump, Math.max(0, q.items[0][0] - performance.now())); } else q.timer = null;
    };
    if (!q.timer) q.timer = setTimeout(pump, q.due - performance.now());
  }
  json(o) { this.send(JSON.stringify(o)); }
  onMessage(d) {
    if (typeof d === 'string') {
      const v = JSON.parse(d);
      switch (v.t) {
        case 'welcome': this.slot = v.slot; this.id = v.id; break;
        case 'pong': this.rtts.push(performance.now() - v.c); if (this.rtts.length > 8) this.rtts.shift(); break;
        case 'cpu': this.m._NetCpu(v.slot, v.frame); break;
        case 'desync': this.desync = v.frame; break;
        case 'go': this.begin(v); break;
        case 'over': this.state = 'done'; break;
      }
      this.onEvent(v);
      return;
    }
    const b = new Uint8Array(d);
    if (b[0] === 4 && this.state === 'racing') {
      const frame = (b[1] << 24 | b[2] << 16 | b[3] << 8 | b[4]) >>> 0, n = b[5];
      this.m.HEAPU8.set(b.subarray(7, 7 + n * 4), this.scratch);
      this.m._NetBundle(frame, n, b[6], this.scratch);
      this.clockSeen = true;
    }
  }
  begin(go) {
    this.go = go; this.cfg = go.config; this.sentTop = -1; this.reportedRunning = false; this.finished = false; this.clockSeen = false; this.desync = null;
    startMatch(this.m, go.config, go, this.slot, { window: this.W, lockstep: this.lockstep });
    this.state = 'racing';
    if (this.lockstep) { this.json({ t: 'running' }); this.reportedRunning = true; }
  }
  rtt() { return this.rtts.length ? Math.min(...this.rtts) : 100; }
  sendPads(top, count) {
    const m = this.m, c = m._NetLocalPacked(this.slot, top, count, this.packBuf);
    const o = new Uint8Array(7 + c * 4);
    o[0] = 1; o[1] = this.slot; o[2] = top >>> 24; o[3] = top >>> 16; o[4] = top >>> 8; o[5] = top; o[6] = c;
    o.set(m.HEAPU8.subarray(this.packBuf, this.packBuf + c * 4), 7);
    this.send(o);
  }
  // Once per browser tick. Returns the number of sim frames run.
  tick(draw = 1) {
    const m = this.m, now = performance.now();
    if (now - this.lastPing > 250) { this.lastPing = now; this.json({ t: 'ping', c: now }); }
    if (this.state !== 'racing') return 0;
    if (this.desync !== null) return 0;
    const top = m._NetTop(), truth = m._NetTruthTop();
    const pad = this.padFn(top + 1);
    m._NetSetLocal(this.slot, pad.buttons, pad.x, pad.y);
    let steps = 1;
    if (this.lockstep) {
      // Lockstep: pads apply `delay` frames later, and a frame runs only when its bundle has arrived.
      while (this.sentTop < truth + this.delay) { this.sentTop++; this.sendLockstep(this.sentTop, pad); }
      steps = Math.max(1, truth - m._NetNow());
      steps = Math.min(steps, 4);
    } else if (this.clockSeen) {
      // Pacing: keep a lead over the last bundle that matches this page's latency. Never past C + W (in wasm).
      const target = Math.ceil(this.rtt() / TICK_MS) + 2, lead = top - truth;
      if (lead < target) steps = 2; else if (lead > target) steps = 0;
    }
    const ran = m._NetTick(steps, draw);
    if (!this.lockstep && m._NetTop() > top) { this.sendPads(m._NetTop(), 8); }
    if (!this.reportedRunning && m._NetNow() >= 8) { this.reportedRunning = true; this.json({ t: 'running' }); }
    while (m._NetPollHash(this.hOut, this.hOut + 4)) {
      const f = m.HEAP32[this.hOut >> 2], h = m.HEAPU32[(this.hOut + 4) >> 2];
      this.hashes.push([f, h]);
      const o = new Uint8Array(10); o[0] = 2; o[1] = this.slot;
      o[2] = f >>> 24; o[3] = f >>> 16; o[4] = f >>> 8; o[5] = f; o[6] = h >>> 24; o[7] = h >>> 16; o[8] = h >>> 8; o[9] = h;
      this.send(o);
    }
    if (m._NetEnded() && !this.finished) { this.finished = true; this.json({ t: 'finished' }); this.onEvent({ t: 'ended', frame: m._NetNow() }); }
    return ran;
  }
  sendLockstep(frame, pad) {
    const o = new Uint8Array(11);
    o[0] = 1; o[1] = this.slot; o[2] = frame >>> 24; o[3] = frame >>> 16; o[4] = frame >>> 8; o[5] = frame; o[6] = 1;
    o[7] = pad.buttons >> 8; o[8] = pad.buttons & 255; o[9] = pad.x & 255; o[10] = pad.y & 255;
    this.send(o);
  }
  stats() {
    const m = this.m, s = i => m._NetStat(i);
    return { now: m._NetNow(), confirmed: m._NetConfirmed(), rollbacks: s(0), resim: s(1), maxDepth: s(2), replacedBundles: s(3), stalls: s(4), rtt: Math.round(this.rtt()), snapshot: m._NetSnapshotSize(), corrections: Array.from({ length: 32 }, (_, d) => m._NetDepthHist(d)) };
  }
}
