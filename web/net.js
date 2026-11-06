// The page's netcode. The relay is the clock and the pad authority; the core is in wasm (port/net/rollback.c).
// No DOM here, so proofs can run the same code without a browser.
//
// One NetClient per lobby visit. Its phase is the only "is a match on" on the page:
//   lobby   in the room (also before the first welcome)
//   racing  a match runs (also while the socket is down and coming back)
//   ended   this page's match is over; waiting for the relay's `over`
//   closed  gave up: left, refused by the relay, or the connection did not come back
// The socket has its own small life: up, or down and retrying with back-off (one timer, never two).
const TICK_MS = 1000 / 60;
const CATCH_UP = 30;      // frames behind the relay before running flat out
const CATCH_MS = 10;      // per browser tick, at most, when catching up
const RETRY_FOR = 30000;  // a socket that does not come back in this long is lost

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
    this.upQ = { due: 0 }; this.downQ = { due: 0 }; this.rtts = []; this.phase = 'lobby'; this.slot = -1; this.id = 0; this.sentTop = -1; this.reportedRunning = false;
    this.lastPing = 0; this.lastPong = 0; this.link = 'down'; this.tries = 0; this.lostAt = 0; this.retryT = 0; this.gaveUp = false;
    this.scratch = m._malloc(64); this.packBuf = m._malloc(64); this.hOut = m._malloc(8);
    this.desync = null; this.hashes = []; this.finished = false; this.clockSeen = false; this.clock = false;
    this.race = 0; this.inbox = []; this.inHead = 0; this.catching = false; this.away = new Set();
  }
  // Read by the lobby, the portal report and the proofs: 'racing' while a match is on, else 'lobby'.
  get state() { return this.phase === 'racing' || this.phase === 'ended' ? 'racing' : 'lobby'; }
  // Resolves once the socket is open; rejects if it closes first. A close after that is `lost()`.
  connect() {
    const ws = new WebSocket(this.url); ws.binaryType = 'arraybuffer'; this.ws = ws;
    return new Promise((res, rej) => {
      let open = false;
      ws.onopen = () => { open = true; this.link = 'up'; res(); };
      ws.onmessage = e => { if (ws !== this.ws) return; this.lagDown || this.jitter ? this.delayed(this.downQ, this.lagDown, () => this.onMessage(e.data)) : this.onMessage(e.data); };
      ws.onclose = () => { if (ws !== this.ws) return; this.link = 'down'; if (!open) rej(new Error('relay unreachable')); else this.lost(); };
    });
  }
  lost() {
    this.onEvent({ t: 'closed', phase: this.phase });
    if (!this.gaveUp) this.retry();
  }
  // The one reconnect timer: 0.5, 1, 2, 4, 4 ... s, for RETRY_FOR at most.
  retry() {
    if (this.retryT || this.gaveUp) return;
    if (!this.lostAt) this.lostAt = performance.now();
    if (performance.now() - this.lostAt > RETRY_FOR) { this.close('lost'); return; }
    const d = Math.min(4000, 500 * 2 ** this.tries++);
    this.retryT = setTimeout(async () => {
      this.retryT = 0;
      try { await this.connect(); this.tries = 0; this.lostAt = 0; } catch { this.retry(); }
    }, d);
  }
  // Done with this client: left, refused, or lost. The lobby hears why.
  close(why) {
    if (this.phase === 'closed') return;
    const was = this.phase;
    this.gaveUp = true; this.phase = 'closed';
    clearTimeout(this.retryT); this.retryT = 0;
    if (this.ws && this.ws.readyState <= 1) { if (why === 'left') this.json({ t: 'leave' }); this.ws.close(); }
    this.onEvent({ t: 'gone', why, was });
  }
  send(data) {
    const go = () => { if (this.ws?.readyState === 1) this.ws.send(data); };
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
        case 'welcome': this.welcome(v); break;
        case 'error': this.gaveUp = true; break;
        case 'clock': this.clock = true; break;
        case 'away': this.away.add(v.slot); v.me = v.slot === this.slot; break;
        case 'back': this.away.delete(v.slot); v.me = v.slot === this.slot; break;
        case 'resumed': if (!v.ok && this.phase === 'racing') { this.phase = 'lobby'; v.me = true; } break;
        case 'desync': if (this.phase === 'racing') this.desync = v.frame; break;
        case 'go': if (!this.begin(v)) return; break;
        case 'over': if (this.phase === 'racing' || this.phase === 'ended') this.phase = 'lobby'; break;
      }
      this.onEvent(v);
      return;
    }
    const b = new Uint8Array(d);
    if (b[0] === 3) { const c = new DataView(d).getFloat64(2); this.lastPong = performance.now(); this.rtts.push(this.lastPong - c); if (this.rtts.length > 8) this.rtts.shift(); return; }
    if (this.phase !== 'racing') return;
    if (b[0] === 4) { this.lastBundle = performance.now(); this.inbox.push(b.subarray(1)); this.feed(); }
    else if (b[0] === 5) {
      this.lastBundle = performance.now();
      // a batch of missed frames: first frame, count, n, then count x (mask, n pads)
      const f0 = (b[1] << 24 | b[2] << 16 | b[3] << 8 | b[4]) >>> 0, count = b[5] << 8 | b[6], n = b[7], rec = 1 + n * 4;
      for (let k = 0; k < count; k++) {
        const o = new Uint8Array(6 + n * 4), f = f0 + k, at = 8 + k * rec;
        o[0] = f >>> 24; o[1] = f >>> 16; o[2] = f >>> 8; o[3] = f; o[4] = n; o[5] = b[at]; o.set(b.subarray(at + 1, at + rec), 6);
        this.inbox.push(o);
      }
      this.feed();
    }
  }
  // A (re)connect. Same race and still a racer: ask for what was missed. Otherwise the match went on without us.
  welcome(v) {
    const back = this.phase === 'racing' || this.phase === 'ended';
    this.slot = v.slot; this.id = v.id;
    if (back) {
      if (v.race !== this.race || !v.racer) { this.phase = 'lobby'; this.onEvent({ t: 'over', why: 'gone' }); return; }
      if (this.phase === 'racing') this.json({ t: 'resume', from: this.queuedTop() + 1, race: this.race });
      else this.json({ t: 'finished' });
      this.onEvent({ t: 'rejoined' });
    } else if (v.race && v.racer) {
      // this page was reloaded in the middle of its match: the relay sends the match from its start
      this.json({ t: 'resume', from: 0, race: 0 });
    }
  }
  // The newest frame this page holds, in the core or waiting to go in.
  queuedTop() {
    if (this.inHead < this.inbox.length) { const e = this.inbox[this.inbox.length - 1]; return (e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3]) >>> 0; }
    return this.m._NetTruthTop();
  }
  // Bundles go into the core in order, as far as its ring has room. The rest waits here.
  feed() {
    const m = this.m;
    while (this.inHead < this.inbox.length) {
      const e = this.inbox[this.inHead], frame = (e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3]) >>> 0, n = e[4];
      m.HEAPU8.set(e.subarray(6, 6 + n * 4), this.scratch);
      const r = m._NetBundle(frame, n, e[5], this.scratch);
      if (r === 2) break;
      if (r === 3) {
        // a hole (cannot happen on one ordered socket; if it does, ask again from what we have)
        this.inbox = []; this.inHead = 0;
        this.json({ t: 'resume', from: m._NetTruthTop() + 1, race: this.race });
        return;
      }
      this.inHead++;
      this.clockSeen = true;
    }
    if (this.inHead > 4096 && this.inHead * 2 > this.inbox.length) { this.inbox = this.inbox.slice(this.inHead); this.inHead = 0; }
  }
  begin(go) {
    this.go = go; this.cfg = go.config; this.race = go.race; this.sentTop = -1; this.reportedRunning = false; this.finished = false; this.clockSeen = false; this.clock = false; this.desync = null;
    this.hashes = []; this.inbox = []; this.inHead = 0; this.away = new Set(); this.catching = false; this.begunAt = performance.now(); this.lastBundle = 0;
    try { startMatch(this.m, go.config, go, this.slot, { window: this.W, lockstep: this.lockstep }); }
    catch (e) {
      // this page sits the match out; the relay does not wait for it
      try { this.m._Park(); } catch {}
      this.json({ t: 'failed' });
      this.onEvent({ t: 'failed', reason: String(e?.message ?? e) });
      return false;
    }
    this.phase = 'racing';
    if (this.lockstep) { this.json({ t: 'running' }); this.reportedRunning = true; }
    return true;
  }
  // The page went to the background: browsers stop its frames, so its fighter should stand at once.
  hidden(h) { if (h && this.phase === 'racing') this.json({ t: 'away' }); }
  rtt() { return this.rtts.length ? Math.min(...this.rtts) : 100; }
  sendPads(top, count) {
    const m = this.m, c = m._NetLocalPacked(this.slot, top, count, this.packBuf);
    const o = new Uint8Array(7 + c * 4);
    o[0] = 1; o[1] = this.slot; o[2] = top >>> 24; o[3] = top >>> 16; o[4] = top >>> 8; o[5] = top; o[6] = c;
    o.set(m.HEAPU8.subarray(this.packBuf, this.packBuf + c * 4), 7);
    this.send(o);
  }
  ping(now) {
    const o = new ArrayBuffer(10), v = new DataView(o); v.setUint8(0, 3); v.setFloat64(2, now);
    this.lastPing = now; this.send(o);
  }
  pollHashes() {
    const m = this.m;
    while (m._NetPollHash(this.hOut, this.hOut + 4)) {
      const f = m.HEAP32[this.hOut >> 2], h = m.HEAPU32[(this.hOut + 4) >> 2];
      this.hashes.push([f, h]);
      const o = new Uint8Array(10); o[0] = 2; o[1] = this.slot;
      o[2] = f >>> 24; o[3] = f >>> 16; o[4] = f >>> 8; o[5] = f; o[6] = h >>> 24; o[7] = h >>> 16; o[8] = h >>> 8; o[9] = h;
      this.send(o);
    }
  }
  // How far behind the relay this page is, in frames (0 when level or ahead).
  behind() { return Math.max(0, this.queuedTop() - this.m._NetNow()); }
  // Once per browser tick. Returns the number of sim frames run.
  tick(draw = 1) {
    const m = this.m, now = performance.now();
    if (this.link === 'up' && now - this.lastPing > 250) this.ping(now);
    if (this.phase !== 'racing' || this.desync !== null) return 0;
    this.feed();
    let ran = 0;
    // Far behind (a hidden tab, a reconnect, a reload): run the frames the relay already closed, flat out,
    // with no picture, a few milliseconds per tick. The sim does exactly what it would have done.
    if (!this.lockstep && m._NetTruthTop() - m._NetNow() > CATCH_UP) {
      const until = now + CATCH_MS;
      this.catching = true;
      do { const r = m._NetCatchUp(60); ran += r; this.pollHashes(); this.feed(); if (r === 0) break; }
      while (performance.now() < until && m._NetTruthTop() - m._NetNow() > 2 && !m._NetEnded());
      m._NetQuietSfx();
    } else this.catching = false;
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
    ran += m._NetTick(steps, draw);
    if (!this.lockstep && m._NetTop() > top) { this.sendPads(m._NetTop(), 8); }
    if (!this.reportedRunning && m._NetNow() >= 8) { this.reportedRunning = true; this.json({ t: 'running' }); }
    this.pollHashes();
    if (m._NetEnded() && !this.finished) { this.finished = true; this.phase = 'ended'; this.json({ t: 'finished' }); this.onEvent({ t: 'ended', frame: m._NetNow() }); }
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
