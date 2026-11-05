//! Game-blind relay. Knows rooms, slots, a seed, a clock and opaque pads. Never parses game state.
//! Frame f closes when every racer has sent its pad for f, never before t0 + f/60 s, and at
//! t0 + (f + G)/60 s regardless. A missing pad repeats that racer's last pad and is marked replaced.
use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use axum::extract::{ConnectInfo, Path, Query, State};
use axum::http::{header, HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use axum::{Json, Router};
use futures_util::{SinkExt, StreamExt};
use rand::Rng;
use serde_json::{json, Value};
use std::collections::{BTreeMap, HashMap};
use std::net::SocketAddr;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tokio::sync::mpsc;

const SLOTS: usize = 8;
const ALPHABET: &[u8] = b"ABCDEFGHJKMNPQRSTUVWXYZ23456789";
const FRAME_US: u64 = 1_000_000 / 60;
const RING: usize = 1024;

#[derive(Clone)]
struct Cfg { secret: String, build_id: String, site: String, grace: u64, silence_ms: u64 }

struct Member { id: u64, name: String, token: String, slot: usize, tx: Option<mpsc::UnboundedSender<Message>>, engine: String, running: bool, finished: bool, pick: Value }

#[derive(Default)]
struct Race {
    active: bool,
    humans: [bool; SLOTS],
    n: usize,
    t0: Option<Instant>,
    next: u32,
    pads: Vec<[Option<[u8; 4]>; RING]>,
    newest: [i64; SLOTS],
    last: [[u8; 4]; SLOTS],
    last_recv: Vec<Instant>,
    dropped: [bool; SLOTS],
    hashes: BTreeMap<u32, (u32, u32)>,
    started_at: Option<Instant>,
    replaced: u64,
    sent: std::collections::VecDeque<(u32, Vec<u8>)>,
}

struct Room { code: String, members: Vec<Member>, config: Value, race: Race, touched: Instant, next_id: u64 }
type Rooms = Arc<Mutex<HashMap<String, Room>>>;

#[derive(Clone)]
struct App { rooms: Rooms, cfg: Cfg, creates: Arc<Mutex<HashMap<std::net::IpAddr, Vec<Instant>>>> }

fn new_code(rooms: &HashMap<String, Room>) -> String {
    let mut r = rand::thread_rng();
    loop {
        let c: String = (0..5).map(|_| ALPHABET[r.gen_range(0..ALPHABET.len())] as char).collect();
        if !rooms.contains_key(&c) { return c; }
    }
}

async fn create_lobby(State(app): State<App>, ConnectInfo(addr): ConnectInfo<SocketAddr>, Json(body): Json<Value>) -> Response {
    if !app.cfg.secret.is_empty() && body["secret"].as_str() != Some(app.cfg.secret.as_str()) {
        return (StatusCode::FORBIDDEN, Json(json!({"error": "wrong host secret"}))).into_response();
    }
    {
        let mut c = app.creates.lock().unwrap();
        let v = c.entry(addr.ip()).or_default();
        v.retain(|t| t.elapsed() < Duration::from_secs(60));
        if v.len() >= 10 { return (StatusCode::TOO_MANY_REQUESTS, Json(json!({"error": "too many new lobbies, wait a minute"}))).into_response(); }
        v.push(Instant::now());
    }
    let mut rooms = app.rooms.lock().unwrap();
    let code = new_code(&rooms);
    rooms.insert(code.clone(), Room { code: code.clone(), members: vec![], config: json!({}), race: Race::default(), touched: Instant::now(), next_id: 1 });
    Json(json!({ "code": code })).into_response()
}

async fn ws_upgrade(ws: WebSocketUpgrade, Path(code): Path<String>, Query(q): Query<HashMap<String, String>>, State(app): State<App>) -> Response {
    ws.on_upgrade(move |s| member_task(s, app, code.to_uppercase(), q))
}

fn roster(room: &Room) -> Value {
    let v: Vec<Value> = room.members.iter().map(|m| json!({"id": m.id, "name": m.name, "slot": m.slot, "engine": m.engine, "online": m.tx.is_some(), "pick": m.pick})).collect();
    json!({"t": "roster", "members": v, "host": room.members.iter().min_by_key(|m| m.id).map(|m| m.id), "config": room.config})
}
fn broadcast(room: &Room, msg: &Value) {
    let s = msg.to_string();
    for m in &room.members { if let Some(tx) = &m.tx { let _ = tx.send(Message::Text(s.clone().into())); } }
}
fn broadcast_bin(room: &Room, b: &[u8]) {
    for m in &room.members { if let Some(tx) = &m.tx { let _ = tx.send(Message::Binary(b.to_vec().into())); } }
}

async fn member_task(sock: WebSocket, app: App, code: String, q: HashMap<String, String>) {
    let (mut wr, mut rd) = sock.split();
    let (tx, mut rx) = mpsc::unbounded_channel::<Message>();
    let fail = |msg: &str| Message::Text(json!({"t": "error", "reason": msg}).to_string().into());
    let build = q.get("build").cloned().unwrap_or_default();
    if !app.cfg.build_id.is_empty() && build != app.cfg.build_id {
        let _ = wr.send(fail("build mismatch: reload the page")).await; return;
    }
    let name: String = q.get("name").cloned().unwrap_or_else(|| "player".into()).chars().take(16).collect();
    let token = q.get("token").cloned().unwrap_or_default();
    let joined: Result<u64, &str> = (|| {
        let mut rooms = app.rooms.lock().unwrap();
        let Some(room) = rooms.get_mut(&code) else { return Err("lobby not found") };
        room.touched = Instant::now();
        let id;
        if let Some(m) = room.members.iter_mut().find(|m| !token.is_empty() && m.token == token) {
            m.tx = Some(tx.clone()); id = m.id;
        } else {
            let mut used = [false; SLOTS];
            for m in &room.members { used[m.slot] = true; }
            let Some(slot) = (0..SLOTS).find(|&i| !used[i]) else { return Err("lobby full") };
            if room.race.active { return Err("match in progress") }
            id = room.next_id; room.next_id += 1;
            room.members.push(Member { id, name: name.clone(), token: token.clone(), slot, tx: Some(tx.clone()), engine: "booting".into(), running: false, finished: false, pick: Value::Null });
        }
        let slot = room.members.iter().find(|m| m.id == id).unwrap().slot;
        let _ = tx.send(Message::Text(json!({"t": "welcome", "id": id, "slot": slot, "code": room.code}).to_string().into()));
        broadcast(room, &roster(room));
        Ok(id)
    })();
    let id = match joined { Ok(i) => i, Err(e) => { let _ = wr.send(fail(e)).await; return; } };
    let w = tokio::spawn(async move { while let Some(m) = rx.recv().await { if wr.send(m).await.is_err() { break; } } });
    while let Some(Ok(m)) = rd.next().await {
        let mut rooms = app.rooms.lock().unwrap();
        let Some(room) = rooms.get_mut(&code) else { break };
        room.touched = Instant::now();
        match m {
            Message::Text(t) => { if let Ok(v) = serde_json::from_str::<Value>(&t) { on_text(room, id, v, &app.cfg); } }
            Message::Binary(b) => on_bin(room, id, &b),
            Message::Close(_) => break,
            _ => {}
        }
    }
    w.abort();
    let mut rooms = app.rooms.lock().unwrap();
    if let Some(room) = rooms.get_mut(&code) {
        if let Some(m) = room.members.iter_mut().find(|m| m.id == id) { m.tx = None; }
        if !room.race.active { room.members.retain(|m| m.tx.is_some()); }
        broadcast(room, &roster(room));
    }
}

fn is_host(room: &Room, id: u64) -> bool { room.members.iter().filter(|m| m.tx.is_some()).map(|m| m.id).min() == Some(id) }

fn on_text(room: &mut Room, id: u64, v: Value, cfg: &Cfg) {
    match v["t"].as_str().unwrap_or("") {
        "ping" => { if let Some(m) = room.members.iter().find(|m| m.id == id) { if let Some(tx) = &m.tx { let _ = tx.send(Message::Text(json!({"t": "pong", "c": v["c"]}).to_string().into())); } } }
        "engine" => { if let Some(m) = room.members.iter_mut().find(|m| m.id == id) { m.engine = v["state"].as_str().unwrap_or("").chars().take(16).collect(); } broadcast(room, &roster(room)); }
        "pick" => { if let Some(m) = room.members.iter_mut().find(|m| m.id == id) { m.pick = v["pick"].clone(); } broadcast(room, &roster(room)); }
        "resume" => {
            // a socket dropped and came back: send what it missed
            let from = v["from"].as_i64().unwrap_or(0).max(0) as u32;
            if let Some(tx) = room.members.iter().find(|m| m.id == id).and_then(|m| m.tx.clone()) {
                for (f, b) in room.race.sent.iter() { if *f >= from { let _ = tx.send(Message::Binary(b.clone().into())); } }
            }
        }
        "config" if is_host(room, id) => { room.config = v["config"].clone(); broadcast(room, &roster(room)); }
        "start" if is_host(room, id) && !room.race.active => start_race(room, v, cfg),
        "kick" if is_host(room, id) => {
            let target = v["id"].as_u64().unwrap_or(0);
            if let Some(m) = room.members.iter_mut().find(|m| m.id == target) { if let Some(tx) = m.tx.take() { let _ = tx.send(Message::Text(json!({"t": "error", "reason": "removed by host"}).to_string().into())); } }
            if !room.race.active { room.members.retain(|m| m.id != target); }
            broadcast(room, &roster(room));
        }
        "running" => {
            if let Some(m) = room.members.iter_mut().find(|m| m.id == id) { m.running = true; }
            let online = room.members.iter().filter(|m| m.tx.is_some()).all(|m| m.running);
            if room.race.active && room.race.t0.is_none() && online {
                room.race.t0 = Some(Instant::now());
                let now = Instant::now(); for t in room.race.last_recv.iter_mut() { *t = now; }
            }
        }
        "finished" => {
            if let Some(m) = room.members.iter_mut().find(|m| m.id == id) { m.finished = true; }
            if room.members.iter().filter(|m| m.tx.is_some()).all(|m| m.finished) {
                room.race.active = false;
                room.members.retain(|m| m.tx.is_some());
                broadcast(room, &json!({"t": "over"}));
                broadcast(room, &roster(room));
            }
        }
        "leave" => {
            if let Some(m) = room.members.iter_mut().find(|m| m.id == id) { m.tx = None; }
        }
        _ => {}
    }
}

fn start_race(room: &mut Room, v: Value, cfg: &Cfg) {
    let _ = cfg;
    let mut r = Race { active: true, n: SLOTS, ..Default::default() };
    r.pads = (0..SLOTS).map(|_| [None; RING]).collect();
    r.newest = [-1; SLOTS];
    r.last_recv = vec![Instant::now(); SLOTS];
    r.started_at = Some(Instant::now());
    for m in room.members.iter_mut() { m.running = false; m.finished = false; if m.tx.is_some() { r.humans[m.slot] = true; } }
    let seed: u32 = rand::thread_rng().gen_range(1..0x7FFF_FFFF);
    let slots: Vec<Value> = room.members.iter().filter(|m| m.tx.is_some()).map(|m| json!({"id": m.id, "slot": m.slot, "name": m.name, "pick": m.pick})).collect();
    room.race = r;
    broadcast(room, &json!({"t": "go", "seed": seed, "config": if v["config"].is_null() { room.config.clone() } else { v["config"].clone() }, "slots": slots, "n": SLOTS}));
}

fn on_bin(room: &mut Room, id: u64, b: &[u8]) {
    let Some(slot) = room.members.iter().find(|m| m.id == id).map(|m| m.slot) else { return };
    match b.first() {
        Some(1) if b.len() >= 7 && room.race.active => {
            let frame = u32::from_be_bytes([b[2], b[3], b[4], b[5]]) as i64;
            let count = b[6] as usize;
            if b.len() < 7 + count * 4 { return; }
            let r = &mut room.race;
            r.last_recv[slot] = Instant::now();
            for i in 0..count {
                let f = frame - (count - 1 - i) as i64;
                if f < 0 { continue; }
                let p = [b[7 + i * 4], b[8 + i * 4], b[9 + i * 4], b[10 + i * 4]];
                if f > r.newest[slot] {
                    r.newest[slot] = f;
                    r.pads[slot][f as usize % RING] = Some(p);
                    // a pad after its close is kept as the slot's last pad, so a late player still drives
                    if (f as u32) < r.next { r.last[slot] = p; }
                }
            }
        }
        Some(2) if b.len() >= 10 && room.race.active => {
            let frame = u32::from_be_bytes([b[2], b[3], b[4], b[5]]);
            let hash = u32::from_be_bytes([b[6], b[7], b[8], b[9]]);
            let e = room.race.hashes.entry(frame).or_insert((hash, 0));
            e.1 += 1;
            if e.0 != hash {
                broadcast(room, &json!({"t": "desync", "frame": frame}));
            }
            while room.race.hashes.len() > 256 { let k = *room.race.hashes.keys().next().unwrap(); room.race.hashes.remove(&k); }
        }
        _ => {}
    }
}

/// Close every frame that is due. Called every 2 ms for each racing room.
fn clock(room: &mut Room, cfg: &Cfg) {
    let Some(t0) = room.race.t0 else {
        // a page that never reports running must not hold the race forever
        if room.race.active && room.race.started_at.map(|s| s.elapsed() > Duration::from_secs(30)).unwrap_or(false) {
            room.race.t0 = Some(Instant::now());
        }
        return;
    };
    let now = t0.elapsed().as_micros() as u64;
    let mut out: Vec<Value> = vec![];
    loop {
        let r = &mut room.race;
        let f = r.next as u64;
        if now < f * FRAME_US { break; }
        let mut all = true;
        for s in 0..SLOTS { if r.humans[s] && !r.dropped[s] && r.newest[s] < f as i64 { all = false; } }
        if !all && now < (f + cfg.grace) * FRAME_US { break; }
        let mut msg = Vec::with_capacity(7 + r.n * 4);
        let mut mask = 0u8;
        msg.push(4u8);
        msg.extend_from_slice(&(f as u32).to_be_bytes());
        msg.push(r.n as u8);
        let at = msg.len(); msg.push(0);
        for s in 0..r.n {
            let pad = if r.humans[s] && !r.dropped[s] {
                if r.newest[s] >= f as i64 && r.newest[s] - (f as i64) < RING as i64 {
                    if let Some(p) = r.pads[s][f as usize % RING] { r.last[s] = p; p } else { mask |= 1 << s; r.last[s] }
                } else { mask |= 1 << s; r.last[s] }
            } else { [0; 4] };
            msg.extend_from_slice(&pad);
        }
        msg[at] = mask;
        if mask != 0 { r.replaced += 1; }
        r.next += 1;
        broadcast_bin(room, &msg);
        let fr = room.race.next - 1;
        room.race.sent.push_back((fr, msg));
        if room.race.sent.len() > 900 { room.race.sent.pop_front(); }
    }
    // a racer with no pads goes CPU on the relay's timer, ordered with the bundles on one socket
    let closed = room.race.next as i64 - 1;
    for s in 0..SLOTS {
        let r = &mut room.race;
        if !r.humans[s] || r.dropped[s] { continue; }
        let online = room.members.iter().any(|m| m.slot == s && m.tx.is_some());
        if !online || r.last_recv[s].elapsed() > Duration::from_millis(cfg.silence_ms) {
            r.dropped[s] = true;
            out.push(json!({"t": "cpu", "slot": s, "frame": closed}));
        }
    }
    for m in out { broadcast(room, &m); }
}

// Test hook, only when RELAY_TEST=1: start a race without a host socket.
async fn test_start(State(app): State<App>, Path(code): Path<String>, Json(body): Json<Value>) -> StatusCode {
    if std::env::var("RELAY_TEST").is_err() { return StatusCode::NOT_FOUND; }
    let mut rooms = app.rooms.lock().unwrap();
    let Some(room) = rooms.get_mut(&code) else { return StatusCode::NOT_FOUND };
    room.members.retain(|m| m.name != "ctl");
    start_race(room, body, &app.cfg);
    StatusCode::OK
}

async fn stat(State(app): State<App>) -> Json<Value> {
    let rooms = app.rooms.lock().unwrap();
    let v: Vec<Value> = rooms.values().map(|r| json!({"code": r.code, "members": r.members.len(), "racing": r.race.active, "frame": r.race.next, "replaced": r.race.replaced})).collect();
    Json(json!({"build": app.cfg.build_id, "rooms": v}))
}

async fn file(State(app): State<App>, Path(p): Path<String>) -> Response { serve(&app.cfg.site, &p).await }
async fn index(State(app): State<App>) -> Response { serve(&app.cfg.site, "index.html").await }
async fn serve(site: &str, p: &str) -> Response {
    if p.contains("..") { return StatusCode::BAD_REQUEST.into_response(); }
    let ty = match p.rsplit('.').next().unwrap_or("") { "html" => "text/html; charset=utf-8", "js" | "mjs" => "text/javascript", "wasm" => "application/wasm", "css" => "text/css", _ => "application/octet-stream" };
    match tokio::fs::read(format!("{site}/{p}")).await {
        Ok(b) => { let mut h = HeaderMap::new(); h.insert(header::CONTENT_TYPE, ty.parse().unwrap()); h.insert(header::CACHE_CONTROL, "no-cache".parse().unwrap()); (h, b).into_response() }
        Err(_) => StatusCode::NOT_FOUND.into_response(),
    }
}

#[tokio::main]
async fn main() {
    let e = |k: &str, d: &str| std::env::var(k).unwrap_or_else(|_| d.to_string());
    let port = e("PORT", "8080");
    let cfg = Cfg {
        secret: e("RELAY_SECRET", ""),
        build_id: e("BUILD_ID", ""),
        site: e("SITE", "web"),
        grace: e("GRACE_FRAMES", "6").parse().unwrap(),
        silence_ms: e("SILENCE_MS", "3000").parse().unwrap(),
    };
    let rooms: Rooms = Arc::new(Mutex::new(HashMap::new()));
    let app = App { rooms: rooms.clone(), cfg: cfg.clone(), creates: Arc::new(Mutex::new(HashMap::new())) };
    {
        let (rooms, cfg) = (rooms.clone(), cfg.clone());
        tokio::spawn(async move {
            let mut iv = tokio::time::interval(Duration::from_millis(2));
            iv.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
            let mut last_gc = Instant::now();
            loop {
                iv.tick().await;
                let mut rs = rooms.lock().unwrap();
                for r in rs.values_mut() { if r.race.active { clock(r, &cfg); } }
                if last_gc.elapsed() > Duration::from_secs(30) {
                    last_gc = Instant::now();
                    rs.retain(|_, r| {
                        let idle = r.touched.elapsed();
                        let empty = r.members.iter().all(|m| m.tx.is_none());
                        !(idle > Duration::from_secs(1800) || (empty && idle > Duration::from_secs(120)))
                    });
                }
            }
        });
    }
    let router = Router::new()
        .route("/api/lobby", post(create_lobby))
        .route("/api/stat", get(stat))
        .route("/api/start/{code}", post(test_start))
        .route("/ws/{code}", get(ws_upgrade))
        .route("/", get(index))
        .route("/{*p}", get(file))
        .with_state(app);
    let listener = tokio::net::TcpListener::bind(format!("0.0.0.0:{port}")).await.unwrap();
    use axum::serve::ListenerExt;
    let listener = listener.tap_io(|s| { let _ = s.set_nodelay(true); });
    println!("relay on :{port} build {}", cfg.build_id);
    axum::serve(listener, router.into_make_service_with_connect_info::<SocketAddr>()).await.unwrap();
}
