// Plays stereo PCM posted from the main thread. Drops the oldest audio if it builds up.
class Ring extends AudioWorkletProcessor {
  constructor() {
    super(); this.q = []; this.off = 0; this.queued = 0;
    this.port.onmessage = e => { this.q.push(e.data); this.queued += e.data.length / 2; while (this.queued > 4800) { this.queued -= (this.q[0].length / 2 - this.off); this.q.shift(); this.off = 0; } };
  }
  process(_, outs) {
    const [l, r] = outs[0];
    for (let i = 0; i < l.length; i++) {
      if (!this.q.length) { l[i] = r[i] = 0; continue; }
      const b = this.q[0];
      l[i] = b[this.off * 2]; r[i] = b[this.off * 2 + 1];
      this.queued--;
      if (++this.off * 2 >= b.length) { this.q.shift(); this.off = 0; }
    }
    return true;
  }
}
registerProcessor('ring', Ring);
