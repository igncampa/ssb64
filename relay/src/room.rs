//! One lobby and its race, as one explicit state machine. No sockets and no clock of its own: every
//! function takes `now`, so the tests drive time by hand. Game-blind: pads are 4 opaque bytes.
//!
//! The relay is the only truth for every frame's pads. Frame f closes when every present racer has
//! sent its pad for f, never before t0 + f/60 s, and at t0 + (f + G)/60 s regardless. A present racer's
//! missing pad repeats its last pad (marked replaced), or is the zero pad once it has been quiet for
//! QUIET. A racer who is away (hidden tab, socket gone, silent for SILENCE, failed to start) gets the
//! zero pad for every frame and holds nobody up; its fighter stands still. It is back when a pad for a
//! frame not yet closed arrives: from then on its pads count again. No fighter ever changes hands.
//!
//!   Idle --start--> Starting --all running, or START_WAIT--> Running
//!   Running --every present racer finished, or END_WAIT after the first--> Idle (`over`)
//!   Running --hash mismatch--> Ending --DESYNC_WAIT--> Idle (`over`, why desync)
//!   any race --no racer online--> Idle
use axum::body::Bytes;
use axum::extract::ws::{Message, Utf8Bytes};
use serde_json::{json, Value};
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::{mpsc, Notify};

pub const SLOTS: usize = 8;
pub const FRAME_US: u64 = 1_000_000 / 60;
const RING: usize = 1024;
/// A closed frame as the history keeps it and as a batch carries it: mask, then 8 pads.
pub const REC: usize = 1 + SLOTS * 4;
/// History kept for a page that comes back: 30 minutes of frames (about 3.6 MB at most per race).
pub const HISTORY: usize = 30 * 60 * 60;
/// Frames per resume batch message.
const BATCH: usize = 512;
const HASHES: usize = 4096; // one every 15 frames: 17 minutes
pub const START_WAIT: Duration = Duration::from_secs(10);
pub const END_WAIT: Duration = Duration::from_secs(10);
pub const DESYNC_WAIT: Duration = Duration::from_secs(2);
const SCAN: Duration = Duration::from_millis(20);
/// Messages a member may have waiting before it is cut off. It reconnects and resumes by itself.
pub const QUEUE: usize = 1024;

#[derive(Clone)]
pub struct Cfg { pub grace: u64, pub silence: Duration, pub quiet: Duration }

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Engine { Loading, Ready, Failed }
impl Engine {
    fn parse(s: &str) -> Engine { match s { "ready" => Engine::Ready, "failed" => Engine::Failed, _ => Engine::Loading } }
    fn name(self) -> &'static str { match self { Engine::Loading => "loading", Engine::Ready => "ready", Engine::Failed => "failed" } }
}

pub type Tx = mpsc::Sender<Message>;

pub struct Member {
    pub id: u64, pub name: String, token: String, pub slot: usize,
    pub tx: Option<Tx>, kill: Option<Arc<Notify>>, pub conn: u64,
    pub engine: Engine, pick: Value,
    // this race
    pub racer: bool, synced: bool, running: bool, finished: bool, failed: bool,
}
impl Member { pub fn online(&self) -> bool { self.tx.is_some() } }

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Phase { Idle, Starting { since: Instant }, Running { t0: Instant }, Ending { at: Instant } }

pub struct Race {
    pub id: u32,
    pub phase: Phase,
    humans: [bool; SLOTS],
    pub away: [bool; SLOTS],
    pub next: u32,
    pads: Vec<[(u32, [u8; 4]); RING]>, // (frame, pad); u32::MAX is none
    newest: [i64; SLOTS],
    last: [[u8; 4]; SLOTS],
    last_recv: [Instant; SLOTS],
    hashes: Vec<(u32, u32)>, // (frame + 1, hash); 0 is none
    history: Vec<u8>,
    hist_base: u32,
    first_finished: Option<Instant>,
    pub replaced: u64,
    go: Option<Utf8Bytes>,
    last_scan: Instant,
    why: &'static str,
}
impl Race {
    fn idle(now: Instant) -> Race {
        Race { id: 0, phase: Phase::Idle, humans: [false; SLOTS], away: [false; SLOTS], next: 0, pads: vec![], newest: [-1; SLOTS], last: [[0; 4]; SLOTS],
            last_recv: [now; SLOTS], hashes: vec![], history: vec![], hist_base: 0, first_finished: None, replaced: 0, go: None, last_scan: now, why: "" }
    }
    pub fn active(&self) -> bool { self.phase != Phase::Idle }
}

pub struct Room {
    pub code: String, pub members: Vec<Member>, config: Value, pub race: Race, pub touched: Instant,
    next_id: u64, conn_seq: u64, race_seq: u32,
    /// Set when a race starts: the socket side spawns the room's clock for it.
    pub clock_wanted: Option<u32>,
    silence: Duration,
}

fn text(v: &Value) -> Message { Message::Text(Utf8Bytes::from(v.to_string())) }

/// Queue one message. A member whose queue is full cannot keep up: it is cut off, and comes back.
fn push(m: &mut Member, msg: Message) {
    if let Some(tx) = &m.tx {
        if tx.try_send(msg).is_err() { m.tx = None; if let Some(k) = m.kill.take() { k.notify_one(); } }
    }
}

impl Room {
    pub fn new(code: String, now: Instant) -> Room {
        Room { code, members: vec![], config: json!({}), race: Race::idle(now), touched: now, next_id: 1, conn_seq: 0, race_seq: 0, clock_wanted: None, silence: Duration::from_secs(3) }
    }
    fn broadcast(&mut self, msg: Message) { for m in self.members.iter_mut() { push(m, msg.clone()); } }
    fn broadcast_json(&mut self, v: Value) { self.broadcast(text(&v)); }
    fn host(&self) -> Option<u64> { self.members.iter().filter(|m| m.online()).map(|m| m.id).min() }
    pub fn roster(&mut self) {
        let host = self.host();
        let v: Vec<Value> = self.members.iter().map(|m| json!({"id": m.id, "name": m.name, "slot": m.slot, "engine": m.engine.name(), "online": m.online(), "pick": m.pick})).collect();
        let racing = self.race.active();
        self.broadcast_json(json!({"t": "roster", "members": v, "host": host, "config": self.config, "racing": racing}));
    }
    fn member(&mut self, id: u64) -> Option<&mut Member> { self.members.iter_mut().find(|m| m.id == id) }
    /// Is this socket still the member's current one? A newer socket for the same token replaces it.
    pub fn is_conn(&self, id: u64, conn: u64) -> bool { self.members.iter().any(|m| m.id == id && m.conn == conn && m.tx.is_some()) }

    /// A socket arrives. A known token takes its member back (the same slot, in the race if it raced).
    pub fn join(&mut self, name: &str, token: &str, tx: Tx, kill: Arc<Notify>, now: Instant) -> Result<(u64, u64), &'static str> {
        self.touched = now;
        self.conn_seq += 1;
        let conn = self.conn_seq;
        let id = if let Some(m) = self.members.iter_mut().find(|m| !token.is_empty() && m.token == token) {
            if let Some(k) = m.kill.replace(kill) { k.notify_one(); }
            m.tx = Some(tx); m.conn = conn; m.synced = false;
            m.id
        } else {
            let mut used = [false; SLOTS];
            for m in &self.members { used[m.slot] = true; }
            // a slot still racing for someone who left stays theirs until the race ends
            for s in 0..SLOTS { if self.race.active() && self.race.humans[s] { used[s] = true; } }
            let Some(slot) = (0..SLOTS).find(|&i| !used[i]) else { return Err("lobby full") };
            let id = self.next_id; self.next_id += 1;
            self.members.push(Member { id, name: name.to_string(), token: token.to_string(), slot, tx: Some(tx), kill: Some(kill), conn,
                engine: Engine::Loading, pick: Value::Null, racer: false, synced: false, running: false, finished: false, failed: false });
            id
        };
        let race = if self.race.active() { self.race.id } else { 0 };
        let code = self.code.clone();
        let m = self.member(id).unwrap();
        let w = json!({"t": "welcome", "id": id, "slot": m.slot, "code": code, "race": race, "racer": m.racer});
        push(m, text(&w));
        self.roster();
        Ok((id, conn))
    }

    /// The socket is gone (closed, cut off, or replaced: only the current one counts).
    pub fn leave(&mut self, id: u64, conn: u64, now: Instant) {
        let Some(i) = self.members.iter().position(|m| m.id == id && m.conn == conn) else { return };
        self.members[i].tx = None; self.members[i].kill = None;
        if !self.race.active() || !self.members[i].racer { self.members.remove(i); }
        self.after_change(now);
    }

    /// A racer who leaves for good: their fighter stands for the rest of the race, their slot stays theirs.
    fn quit(&mut self, id: u64, now: Instant) {
        let Some(i) = self.members.iter().position(|m| m.id == id) else { return };
        if let Some(k) = self.members[i].kill.take() { k.notify_one(); }
        self.members.remove(i);
        self.after_change(now);
    }

    fn after_change(&mut self, now: Instant) {
        if self.race.active() {
            if !self.members.iter().any(|m| m.racer && m.online()) { self.end_race("abandoned"); }
            else { self.scan(now); self.advance(now, None); }
        }
        self.roster();
    }

    pub fn on_text(&mut self, id: u64, v: &Value, cfg: &Cfg, now: Instant) {
        match v["t"].as_str().unwrap_or("") {
            "engine" => {
                let e = Engine::parse(v["state"].as_str().unwrap_or(""));
                if let Some(m) = self.member(id) { m.engine = e; }
                self.roster();
            }
            "pick" => { if let Some(m) = self.member(id) { m.pick = v["pick"].clone(); } self.roster(); }
            "config" if self.host() == Some(id) => { self.config = v["config"].clone(); self.roster(); }
            "start" if self.host() == Some(id) && !self.race.active() => {
                // everyone online must have finished loading; a page whose game failed to load sits out
                if self.members.iter().any(|m| m.online() && m.engine == Engine::Loading) { return; }
                let c = if v["config"].is_null() { self.config.clone() } else { v["config"].clone() };
                self.start_race(c, now, true);
            }
            "resume" => self.resume(id, v["from"].as_i64().unwrap_or(0), v["race"].as_u64().unwrap_or(0) as u32),
            "running" => { if let Some(m) = self.member(id) { m.running = true; } self.advance(now, Some(cfg)); }
            "finished" => {
                if let Some(m) = self.member(id) { if m.racer { m.finished = true; } }
                if self.race.active() { self.race.first_finished.get_or_insert(now); }
                self.advance(now, Some(cfg));
            }
            "failed" => {
                // the page could not start the match: it sits this race out and holds nobody up
                if let Some(m) = self.member(id) { if m.racer { m.failed = true; } }
                self.scan(now); self.advance(now, Some(cfg));
            }
            "away" => {
                // the page went to the background: its fighter stands from the next frame
                let slot = self.members.iter().find(|m| m.id == id && m.racer).map(|m| m.slot);
                if let (Some(s), Phase::Running { .. }) = (slot, self.race.phase) { self.set_away(s, true); }
            }
            "leave" => { if self.race.active() { self.quit(id, now); } else { let c = self.members.iter().find(|m| m.id == id).map(|m| m.conn).unwrap_or(0); self.leave(id, c, now); } }
            _ => {}
        }
    }

    pub fn on_bin(&mut self, id: u64, b: Bytes, cfg: &Cfg, now: Instant) {
        let Some(i) = self.members.iter().position(|m| m.id == id) else { return };
        match b.first() {
            // ping: echoed as it came, no parsing
            Some(3) => { push(&mut self.members[i], Message::Binary(b)); }
            Some(1) if b.len() >= 7 && self.members[i].racer && matches!(self.race.phase, Phase::Starting { .. } | Phase::Running { .. }) => {
                let slot = self.members[i].slot;
                let frame = u32::from_be_bytes([b[2], b[3], b[4], b[5]]) as i64;
                let count = b[6] as usize;
                if b.len() < 7 + count * 4 { return; }
                let r = &mut self.race;
                let away = r.away[slot];
                let mut back = false;
                for k in 0..count {
                    let f = frame - (count - 1 - k) as i64;
                    if f < 0 || f > u32::MAX as i64 - 1 { continue; }
                    let p = [b[7 + k * 4], b[8 + k * 4], b[9 + k * 4], b[10 + k * 4]];
                    if f > r.newest[slot] {
                        r.newest[slot] = f;
                        r.pads[slot][f as usize % RING] = (f as u32, p);
                        // a present racer's pad after its close is kept as its last pad, so a late player still drives
                        if (f as u32) < r.next { if !away { r.last[slot] = p; } } else { back = true; }
                    }
                }
                if !away { r.last_recv[slot] = now; }
                else if back && !self.members[i].failed {
                    // caught up: its pads count again from the next frame to close
                    r.last_recv[slot] = now; r.last[slot] = [0; 4];
                    self.set_away(slot, false);
                }
                self.advance(now, Some(cfg));
            }
            Some(2) if b.len() >= 10 && matches!(self.race.phase, Phase::Running { .. }) => {
                let frame = u32::from_be_bytes([b[2], b[3], b[4], b[5]]);
                let hash = u32::from_be_bytes([b[6], b[7], b[8], b[9]]);
                let r = &mut self.race;
                let e = &mut r.hashes[(frame / 15) as usize % HASHES];
                if e.0 != frame + 1 { *e = (frame + 1, hash); }
                else if e.1 != hash {
                    self.broadcast_json(json!({"t": "desync", "frame": frame}));
                    self.race.phase = Phase::Ending { at: now + DESYNC_WAIT };
                    self.race.why = "desync";
                }
            }
            _ => {}
        }
    }

    fn set_away(&mut self, s: usize, away: bool) {
        if self.race.away[s] == away { return; }
        self.race.away[s] = away;
        let frame = self.race.next;
        self.broadcast_json(json!({"t": if away { "away" } else { "back" }, "slot": s, "frame": frame}));
    }

    pub fn start_race(&mut self, config: Value, now: Instant, need_ready: bool) {
        self.race_seq += 1;
        let mut r = Race::idle(now);
        r.id = self.race_seq;
        r.phase = Phase::Starting { since: now };
        r.pads = vec![[(u32::MAX, [0; 4]); RING]; SLOTS];
        r.hashes = vec![(0, 0); HASHES];
        r.history = Vec::with_capacity(REC * 60 * 60);
        for m in self.members.iter_mut() {
            m.racer = m.online() && !(need_ready && m.engine == Engine::Failed);
            m.synced = m.racer; m.running = false; m.finished = false; m.failed = false;
            if m.racer { r.humans[m.slot] = true; }
        }
        let seed: u32 = rand::Rng::gen_range(&mut rand::thread_rng(), 1..0x7FFF_FFFF);
        let slots: Vec<Value> = self.members.iter().filter(|m| m.racer).map(|m| json!({"id": m.id, "slot": m.slot, "name": m.name, "pick": m.pick})).collect();
        let go = json!({"t": "go", "race": r.id, "seed": seed, "config": config, "slots": slots, "n": SLOTS});
        let go = Utf8Bytes::from(go.to_string());
        r.go = Some(go.clone());
        self.race = r;
        for m in self.members.iter_mut() { if m.racer { push(m, Message::Text(go.clone())); } }
        self.clock_wanted = Some(self.race.id);
        self.roster();
    }

    /// A page asks for the frames it missed. From another race (a reload mid-match): the match from its start.
    fn resume(&mut self, id: u64, from: i64, race: u32) {
        let Some(i) = self.members.iter().position(|m| m.id == id) else { return };
        if !self.members[i].racer || !matches!(self.race.phase, Phase::Starting { .. } | Phase::Running { .. }) { return; }
        let mut from = from.max(0) as u32;
        if race != self.race.id { if let Some(go) = self.race.go.clone() { push(&mut self.members[i], Message::Text(go)); } from = 0; }
        let r = &self.race;
        let (base, to) = (r.hist_base, r.next);
        if from < base {
            push(&mut self.members[i], text(&json!({"t": "resumed", "ok": false, "oldest": base})));
            return;
        }
        push(&mut self.members[i], text(&json!({"t": "resumed", "ok": true, "from": from, "to": to})));
        let mut f = from;
        while f < to {
            let n = ((to - f) as usize).min(BATCH);
            let at = (f - base) as usize * REC;
            let mut msg = Vec::with_capacity(8 + n * REC);
            msg.push(5u8); msg.extend_from_slice(&f.to_be_bytes()); msg.extend_from_slice(&(n as u16).to_be_bytes()); msg.push(SLOTS as u8);
            msg.extend_from_slice(&self.race.history[at..at + n * REC]);
            push(&mut self.members[i], Message::Binary(Bytes::from(msg)));
            f += n as u32;
        }
        self.members[i].synced = true;
    }

    fn end_race(&mut self, why: &'static str) {
        let why = if self.race.why.is_empty() { why } else { self.race.why };
        self.broadcast_json(json!({"t": "over", "why": why}));
        self.race = Race::idle(self.race.last_scan);
        self.members.retain(|m| m.online());
        for m in self.members.iter_mut() { m.racer = false; m.synced = false; }
    }

    /// Away detection, every SCAN at most: offline, failed, or silent too long.
    fn scan(&mut self, now: Instant) {
        if !matches!(self.race.phase, Phase::Running { .. }) { return; }
        // the relay itself was stalled (scans come every 50 ms at most): nobody's silence is their fault
        if now.duration_since(self.race.last_scan) > Duration::from_secs(1) { self.race.last_recv = [now; SLOTS]; }
        self.race.last_scan = now;
        for s in 0..SLOTS {
            if !self.race.humans[s] || self.race.away[s] { continue; }
            let here = self.members.iter().any(|m| m.slot == s && m.racer && m.online() && !m.failed);
            if !here || now.duration_since(self.race.last_recv[s]) > self.silence { self.set_away(s, true); }
        }
    }

    /// Everything time moves: start the clock, close due frames, see who is away, end the race.
    /// `cfg` is None only from paths that cannot close frames.
    pub fn advance(&mut self, now: Instant, cfg: Option<&Cfg>) {
        if let Some(c) = cfg { self.silence = c.silence; }
        match self.race.phase {
            Phase::Idle => {}
            Phase::Starting { since } => {
                let all = self.members.iter().filter(|m| m.racer && m.online() && !m.failed).all(|m| m.running);
                if all || now.duration_since(since) > START_WAIT {
                    self.race.phase = Phase::Running { t0: now };
                    self.race.last_recv = [now; SLOTS];
                    self.broadcast_json(json!({"t": "clock"}));
                }
            }
            Phase::Running { t0 } => {
                if let Some(c) = cfg { self.close_frames(t0, now, c); }
                if now.duration_since(self.race.last_scan) >= SCAN { self.scan(now); }
                if let Some(ff) = self.race.first_finished {
                    let present = |m: &Member| m.racer && m.online() && !m.failed && !self.race.away[m.slot];
                    let all = self.members.iter().filter(|m| present(m)).all(|m| m.finished);
                    if all || now.duration_since(ff) > END_WAIT { self.end_race("finished"); self.roster(); }
                }
            }
            Phase::Ending { at } => { if now >= at { self.end_race("desync"); self.roster(); } }
        }
    }

    fn close_frames(&mut self, t0: Instant, now: Instant, cfg: &Cfg) {
        let now_us = now.saturating_duration_since(t0).as_micros() as u64;
        loop {
            let r = &mut self.race;
            let f = r.next as u64;
            if now_us < f * FRAME_US { break; }
            let mut all = true;
            for s in 0..SLOTS { if r.humans[s] && !r.away[s] && r.newest[s] < f as i64 { all = false; } }
            if !all && now_us < (f + cfg.grace) * FRAME_US { break; }
            let mut msg = Vec::with_capacity(7 + REC);
            msg.push(4u8); msg.extend_from_slice(&(f as u32).to_be_bytes()); msg.push(SLOTS as u8); msg.push(0);
            let mut mask = 0u8;
            for s in 0..SLOTS {
                let pad = if r.humans[s] && !r.away[s] {
                    let (pf, p) = r.pads[s][f as usize % RING];
                    if pf == f as u32 { r.last[s] = p; p } else {
                        mask |= 1 << s;
                        // quiet for a while: the fighter stands rather than repeat an old input
                        if now.duration_since(r.last_recv[s]) > cfg.quiet { [0; 4] } else { r.last[s] }
                    }
                } else { [0; 4] };
                msg.extend_from_slice(&pad);
            }
            msg[6] = mask;
            if mask != 0 { r.replaced += 1; }
            r.next += 1;
            if r.history.len() >= HISTORY * REC {
                // keep the newest HISTORY frames; drop the oldest tenth in one go
                let drop = HISTORY / 10;
                r.history.drain(..drop * REC);
                r.hist_base += drop as u32;
            }
            r.history.extend_from_slice(&msg[6..]);
            let b = Message::Binary(Bytes::from(msg));
            for m in self.members.iter_mut() { if m.synced { push(m, b.clone()); } }
        }
    }

    /// When the room's clock must look again.
    pub fn next_wake(&self, now: Instant, cfg: &Cfg) -> Option<Instant> {
        let soon = now + Duration::from_millis(50);
        match self.race.phase {
            Phase::Idle => None,
            Phase::Starting { since } => Some((since + START_WAIT).min(soon)),
            Phase::Ending { at } => Some(at.min(soon)),
            Phase::Running { t0 } => {
                let r = &self.race;
                let f = r.next as u64;
                let all = (0..SLOTS).all(|s| !r.humans[s] || r.away[s] || r.newest[s] >= f as i64);
                let due = t0 + Duration::from_micros(if all { f } else { f + cfg.grace } * FRAME_US);
                Some(due.min(soon).max(now))
            }
        }
    }
}


#[cfg(test)]
mod tests {
    use super::*;
    const CFG: Cfg = Cfg { grace: 6, silence: Duration::from_secs(3), quiet: Duration::from_millis(500) };
    struct P { id: u64, conn: u64, rx: mpsc::Receiver<Message> }
    fn ms(t0: Instant, n: u64) -> Instant { t0 + Duration::from_millis(n) }
    fn join(r: &mut Room, name: &str, now: Instant) -> P {
        let (tx, rx) = mpsc::channel(QUEUE);
        let (id, conn) = r.join(name, name, tx, Arc::new(Notify::new()), now).unwrap();
        P { id, conn, rx }
    }
    fn rejoin(r: &mut Room, name: &str, now: Instant, cap: usize) -> P {
        let (tx, rx) = mpsc::channel(cap);
        let (id, conn) = r.join(name, name, tx, Arc::new(Notify::new()), now).unwrap();
        P { id, conn, rx }
    }
    /// Everything queued for a page: text as JSON, bundles as (frame, mask, pads).
    fn drain(p: &mut P) -> (Vec<Value>, Vec<(u32, u8, Vec<u8>)>) {
        let (mut t, mut b) = (vec![], vec![]);
        while let Ok(m) = p.rx.try_recv() {
            match m {
                Message::Text(s) => t.push(serde_json::from_str(s.as_str()).unwrap()),
                Message::Binary(x) if x[0] == 4 => b.push((u32::from_be_bytes([x[1], x[2], x[3], x[4]]), x[6], x[7..].to_vec())),
                Message::Binary(x) if x[0] == 5 => {
                    let f0 = u32::from_be_bytes([x[1], x[2], x[3], x[4]]);
                    let n = u16::from_be_bytes([x[5], x[6]]) as usize;
                    for k in 0..n { let r = &x[8 + k * REC..8 + (k + 1) * REC]; b.push((f0 + k as u32, r[0], r[1..].to_vec())); }
                }
                _ => {}
            }
        }
        (t, b)
    }
    fn pad(r: &mut Room, p: &P, f: u32, v: u8, now: Instant) {
        let mut b = vec![1u8, 0]; b.extend_from_slice(&f.to_be_bytes()); b.push(1); b.extend_from_slice(&[v, v, v, v]);
        r.on_bin(p.id, Bytes::from(b), &CFG, now);
    }
    fn hash(r: &mut Room, p: &P, f: u32, h: u32, now: Instant) {
        let mut b = vec![2u8, 0]; b.extend_from_slice(&f.to_be_bytes()); b.extend_from_slice(&h.to_be_bytes());
        r.on_bin(p.id, Bytes::from(b), &CFG, now);
    }
    fn t(r: &mut Room, p: &P, v: Value, now: Instant) { r.on_text(p.id, &v, &CFG, now); }
    fn has(t: &[Value], k: &str) -> bool { t.iter().any(|v| v["t"] == k) }
    /// Two racers, the clock running at `t0`.
    fn racing() -> (Room, P, P, Instant) {
        let t0 = Instant::now();
        let mut r = Room::new("ABCDE".into(), t0);
        let (mut a, mut b) = (join(&mut r, "a", t0), join(&mut r, "b", t0));
        for p in [&a, &b] { t(&mut r, p, json!({"t": "engine", "state": "ready"}), t0); }
        t(&mut r, &a, json!({"t": "start", "config": {}}), t0);
        assert!(r.clock_wanted.take().is_some());
        for p in [&a, &b] { t(&mut r, p, json!({"t": "running"}), t0); }
        assert!(matches!(r.race.phase, Phase::Running { .. }));
        drain(&mut a); drain(&mut b);
        (r, a, b, t0)
    }
    /// Both racers send a pad per frame, ahead of the clock, until `to` (frames).
    fn play(r: &mut Room, ps: &[&P], from: u32, to: u32, t0: Instant) {
        for f in from..to { let now = t0 + Duration::from_micros(f as u64 * FRAME_US); for p in ps { pad(r, p, f, 1 + p.id as u8, now); } r.advance(now, Some(&CFG)); }
    }

    #[test]
    fn frames_close_on_time_with_every_pad() {
        let (mut r, mut a, _b, t0) = racing();
        play(&mut r, &[&a, &_b], 0, 60, t0);
        let (_, bs) = drain(&mut a);
        assert_eq!(bs.len(), 60);
        assert!(bs.iter().enumerate().all(|(i, x)| x.0 == i as u32 && x.1 == 0 && x.2[0] == 2 && x.2[4] == 3));
    }

    #[test]
    fn a_missing_pad_repeats_then_stands_then_away() {
        let (mut r, mut a, b, t0) = racing();
        play(&mut r, &[&a, &b], 0, 10, t0);
        // b stops: its slot repeats its last pad, after QUIET the zero pad, after SILENCE it is away
        for f in 10..400u32 { let now = t0 + Duration::from_micros(f as u64 * FRAME_US); pad(&mut r, &a, f, 2, now); r.advance(now, Some(&CFG)); }
        r.advance(ms(t0, 400 * 17), Some(&CFG));
        let (tx, bs) = drain(&mut a);
        let at = |f: u32| bs.iter().find(|x| x.0 == f).unwrap().clone();
        assert_eq!(at(12).2[4], 3, "repeats the last pad at first");
        assert_eq!(at(12).1 & 2, 2, "marked replaced");
        assert_eq!(at(60).2[4], 0, "zero after quiet");
        assert!(has(&tx, "away"));
        assert!(!has(&tx, "cpu"));
        assert_eq!(r.race.away, [false, true, false, false, false, false, false, false]);
    }

    #[test]
    fn an_away_racer_holds_nobody_and_comes_back() {
        let (mut r, mut a, mut b, t0) = racing();
        play(&mut r, &[&a, &b], 0, 10, t0);
        t(&mut r, &b, json!({"t": "away"}), ms(t0, 170));
        // a alone: frames close as soon as a's pad is in, without waiting for the grace
        for f in 10..100u32 { let now = t0 + Duration::from_micros(f as u64 * FRAME_US); pad(&mut r, &a, f, 2, now); }
        let (_, bs) = drain(&mut a);
        assert_eq!(bs.last().unwrap().0, 99);
        assert!(bs.iter().filter(|x| x.0 >= 10).all(|x| x.2[4] == 0 && x.1 == 0), "zero pads, not replaced");
        // b catches up: pads for closed frames do not bring it back, a pad for an open frame does
        let now = t0 + Duration::from_micros(99 * FRAME_US);
        pad(&mut r, &b, 50, 9, now);
        assert!(r.race.away[1]);
        pad(&mut r, &b, 100, 9, now);
        assert!(!r.race.away[1]);
        let (tx, _) = drain(&mut b);
        assert!(tx.iter().any(|v| v["t"] == "back" && v["frame"] == 100));
        let now = t0 + Duration::from_micros(100 * FRAME_US);
        pad(&mut r, &a, 100, 2, now);
        let (_, bs) = drain(&mut a);
        assert_eq!(bs.last().unwrap(), &(100, 0, vec![2, 2, 2, 2, 9, 9, 9, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]));
    }

    #[test]
    fn a_stale_ring_entry_is_never_used() {
        let (mut r, mut a, b, t0) = racing();
        play(&mut r, &[&a, &b], 0, 5, t0);
        // b's pad for frame 5 + 1024 lands in the same ring cell as frame 5's would
        pad(&mut r, &b, 5 + 1024, 7, ms(t0, 80));
        for f in 5..7u32 { pad(&mut r, &a, f, 2, ms(t0, 80)); }
        r.advance(t0 + Duration::from_micros(12 * FRAME_US), Some(&CFG));
        let (_, bs) = drain(&mut a);
        assert_ne!(bs.iter().find(|x| x.0 == 5).unwrap().2[4], 7);
    }

    #[test]
    fn match_end_waits_for_present_racers_only() {
        let (mut r, mut a, b, t0) = racing();
        play(&mut r, &[&a, &b], 0, 10, t0);
        t(&mut r, &b, json!({"t": "away"}), ms(t0, 170));
        t(&mut r, &a, json!({"t": "finished"}), ms(t0, 180));
        let (tx, _) = drain(&mut a);
        assert!(tx.iter().any(|v| v["t"] == "over" && v["why"] == "finished"));
        assert!(!r.race.active());
    }

    #[test]
    fn match_end_times_out_for_a_stuck_present_racer() {
        let (mut r, mut a, b, t0) = racing();
        play(&mut r, &[&a, &b], 0, 10, t0);
        t(&mut r, &a, json!({"t": "finished"}), ms(t0, 170));
        assert!(r.race.active());
        // b keeps sending pads (present) but never finishes
        for k in 1..12u64 { let now = ms(t0, 170 + k * 1000); pad(&mut r, &b, 10 + k as u32 * 60, 3, now); pad(&mut r, &a, 10 + k as u32 * 60, 2, now); r.advance(now, Some(&CFG)); }
        assert!(!r.race.active());
        assert!(has(&drain(&mut a).0, "over"));
    }

    #[test]
    fn a_desync_ends_the_race_and_a_rematch_starts() {
        let (mut r, mut a, b, t0) = racing();
        play(&mut r, &[&a, &b], 0, 20, t0);
        hash(&mut r, &a, 15, 111, ms(t0, 300));
        hash(&mut r, &b, 15, 222, ms(t0, 300));
        let (tx, _) = drain(&mut a);
        assert!(has(&tx, "desync"));
        r.advance(ms(t0, 300) + DESYNC_WAIT + Duration::from_millis(1), Some(&CFG));
        let (tx, _) = drain(&mut a);
        assert!(tx.iter().any(|v| v["t"] == "over" && v["why"] == "desync"));
        t(&mut r, &a, json!({"t": "start", "config": {}}), ms(t0, 3000));
        assert!(r.race.active() && r.race.id == 2);
    }

    #[test]
    fn equal_hashes_are_fine() {
        let (mut r, mut a, b, t0) = racing();
        for f in (0..3000).step_by(15) { hash(&mut r, &a, f, f * 7, ms(t0, 1)); hash(&mut r, &b, f, f * 7, ms(t0, 1)); }
        assert!(!has(&drain(&mut a).0, "desync"));
    }

    #[test]
    fn a_racer_who_reconnects_gets_every_frame_it_missed() {
        let (mut r, mut a, b, t0) = racing();
        play(&mut r, &[&a, &b], 0, 50, t0);
        r.leave(b.id, b.conn, ms(t0, 840));
        assert!(r.race.away[1]);
        let mut live = vec![];
        for k in 0..6 { play(&mut r, &[&a], 50 + k * 492, 50 + (k + 1) * 492, t0); live.extend(drain(&mut a).1); }
        let mut b2 = rejoin(&mut r, "b", ms(t0, 50_000), QUEUE);
        let (tx, bs) = drain(&mut b2);
        assert!(tx.iter().any(|v| v["t"] == "welcome" && v["racer"] == true && v["race"] == 1));
        assert!(bs.is_empty(), "nothing live before it asks");
        t(&mut r, &b2, json!({"t": "resume", "from": 40, "race": 1}), ms(t0, 50_000));
        let (tx, bs) = drain(&mut b2);
        assert!(tx.iter().any(|v| v["t"] == "resumed" && v["ok"] == true));
        assert_eq!(bs.len(), 3002 - 40);
        assert!(bs.iter().enumerate().all(|(i, x)| x.0 == 40 + i as u32));
        assert_eq!(&live.iter().find(|x| x.0 == 1234).unwrap().2, &bs.iter().find(|x| x.0 == 1234).unwrap().2);
    }

    #[test]
    fn a_reloaded_page_gets_the_match_from_its_start() {
        let (mut r, _a, b, t0) = racing();
        play(&mut r, &[&_a, &b], 0, 100, t0);
        r.leave(b.id, b.conn, ms(t0, 1700));
        let mut b2 = rejoin(&mut r, "b", ms(t0, 1800), QUEUE);
        t(&mut r, &b2, json!({"t": "resume", "from": 0, "race": 0}), ms(t0, 1800));
        let (tx, bs) = drain(&mut b2);
        assert!(tx.iter().any(|v| v["t"] == "go" && v["race"] == 1));
        assert_eq!(bs.len(), 100);
    }

    #[test]
    fn a_slow_page_is_cut_off_not_buffered() {
        let (mut r, _a, b, t0) = racing();
        let b2 = rejoin(&mut r, "b", t0, 4);
        let _ = b;
        t(&mut r, &b2, json!({"t": "resume", "from": 0, "race": 1}), t0);
        play(&mut r, &[&_a], 0, 50, t0);
        assert!(!r.members.iter().find(|m| m.id == b2.id).unwrap().online());
    }

    #[test]
    fn everyone_gone_ends_the_race() {
        let (mut r, a, b, t0) = racing();
        r.leave(a.id, a.conn, t0); r.leave(b.id, b.conn, t0);
        assert!(!r.race.active());
        assert!(r.members.is_empty());
    }

    #[test]
    fn the_host_is_the_lowest_online_member_everywhere() {
        let (mut r, a, mut b, t0) = racing();
        r.leave(a.id, a.conn, t0);
        let (tx, _) = drain(&mut b);
        let ro = tx.iter().rev().find(|v| v["t"] == "roster").unwrap();
        assert_eq!(ro["host"], b.id);
    }

    #[test]
    fn a_newcomer_waits_in_the_room_during_a_race() {
        let (mut r, mut a, b, t0) = racing();
        let mut c = join(&mut r, "c", t0);
        let (tx, _) = drain(&mut c);
        assert!(tx.iter().any(|v| v["t"] == "welcome" && v["racer"] == false && v["race"] == 1));
        play(&mut r, &[&a, &b], 0, 5, t0);
        pad(&mut r, &c, 5, 9, t0);
        assert!(drain(&mut c).1.is_empty(), "no bundles for a newcomer");
        let _ = drain(&mut a);
        t(&mut r, &a, json!({"t": "finished"}), ms(t0, 100)); t(&mut r, &b, json!({"t": "finished"}), ms(t0, 100));
        assert!(!r.race.active());
        assert_eq!(r.members.len(), 3);
    }

    #[test]
    fn start_waits_for_every_engine() {
        let t0 = Instant::now();
        let mut r = Room::new("X".into(), t0);
        let (a, b) = (join(&mut r, "a", t0), join(&mut r, "b", t0));
        t(&mut r, &a, json!({"t": "engine", "state": "ready"}), t0);
        t(&mut r, &a, json!({"t": "start"}), t0);
        assert!(!r.race.active());
        t(&mut r, &b, json!({"t": "engine", "state": "failed"}), t0);
        t(&mut r, &a, json!({"t": "start"}), t0);
        assert!(r.race.active());
        assert!(!r.members.iter().find(|m| m.id == b.id).unwrap().racer, "a page whose game failed to load sits out");
    }

    #[test]
    fn a_failed_start_holds_nobody() {
        let t0 = Instant::now();
        let mut r = Room::new("X".into(), t0);
        let (a, b) = (join(&mut r, "a", t0), join(&mut r, "b", t0));
        for p in [&a, &b] { t(&mut r, p, json!({"t": "engine", "state": "ready"}), t0); }
        t(&mut r, &a, json!({"t": "start"}), t0);
        t(&mut r, &b, json!({"t": "failed"}), t0);
        t(&mut r, &a, json!({"t": "running"}), t0);
        assert!(matches!(r.race.phase, Phase::Running { .. }), "the clock starts without the failed page");
        r.advance(ms(t0, 30), Some(&CFG));
        assert!(r.race.away[1]);
        t(&mut r, &a, json!({"t": "finished"}), ms(t0, 100));
        assert!(!r.race.active());
    }

    #[test]
    fn history_is_bounded() {
        let (mut r, a, _b, t0) = racing();
        r.race.away[1] = true;
        let n = HISTORY as u32 + HISTORY as u32 / 5;
        for f in 0..n { if f % 8 == 0 { pad(&mut r, &a, f + 7, 1, t0); } }
        let mut q = a; while q.rx.try_recv().is_ok() {}
        r.advance(t0 + Duration::from_micros(n as u64 * FRAME_US), Some(&CFG));
        assert!(r.race.history.len() <= HISTORY * REC);
        assert!(r.race.hist_base > 0);
    }

    #[test]
    fn a_stalled_relay_does_not_mark_everyone_away() {
        let (mut r, mut a, b, t0) = racing();
        play(&mut r, &[&a, &b], 0, 10, t0);
        // the relay process was stopped for 4 s; then the pads that waited in the sockets arrive
        let now = ms(t0, 4200);
        r.advance(now, Some(&CFG));
        assert_eq!(r.race.away, [false; SLOTS]);
        assert!(!has(&drain(&mut a).0, "away"));
    }
}
