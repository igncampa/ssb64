//! Game-blind relay. Knows rooms, slots, a seed, a clock and opaque pads. Never parses game state.
//! The rules of a room and its race are in room.rs; this file is sockets, timers and HTTP.
//! Each room has its own lock; one clock thread wakes each racing room when its next frame
//! is due. Every send is a bounded, non-blocking queue push of a shared buffer; nothing awaits under a lock.
mod room;

use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use axum::extract::{ConnectInfo, Path, Query, State};
use axum::http::{header, HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use axum::{Json, Router};
use futures_util::{SinkExt, StreamExt};
use rand::Rng;
use room::{Cfg, Room};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tokio::sync::{mpsc, Notify};

const ALPHABET: &[u8] = b"ABCDEFGHJKMNPQRSTUVWXYZ23456789";

type RoomRef = Arc<Mutex<Room>>;
type Rooms = Arc<Mutex<HashMap<String, RoomRef>>>;

#[derive(Clone)]
struct App { rooms: Rooms, cfg: Arc<Cfg>, clocks: Arc<Mutex<Clocks>>, secret: Arc<String>, build_id: Arc<String>, site: Arc<String>, creates: Arc<Mutex<HashMap<std::net::IpAddr, Vec<Instant>>>> }

fn new_code(rooms: &HashMap<String, RoomRef>) -> String {
    let mut r = rand::thread_rng();
    loop {
        let c: String = (0..5).map(|_| ALPHABET[r.gen_range(0..ALPHABET.len())] as char).collect();
        if !rooms.contains_key(&c) { return c; }
    }
}

async fn create_lobby(State(app): State<App>, ConnectInfo(addr): ConnectInfo<SocketAddr>, Json(body): Json<Value>) -> Response {
    if !app.secret.is_empty() && body["secret"].as_str() != Some(app.secret.as_str()) {
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
    rooms.insert(code.clone(), Arc::new(Mutex::new(Room::new(code.clone(), Instant::now()))));
    Json(json!({ "code": code })).into_response()
}

async fn ws_upgrade(ws: WebSocketUpgrade, Path(code): Path<String>, Query(q): Query<HashMap<String, String>>, State(app): State<App>) -> Response {
    // pages send small messages; nothing legitimate comes near this
    ws.max_message_size(64 * 1024).max_frame_size(64 * 1024).on_upgrade(move |s| member_task(s, app, code.to_uppercase(), q))
}

/// The clocks of every racing room, on one OS thread with a timer heap. A thread sleep wakes within
/// tens of microseconds of a frame's due time, where the async timer rounds up to the next millisecond.
/// Locks one room at a time, sends with non-blocking queue pushes only.
struct Wake { at: Instant, seq: u64, room: RoomRef, race: u32 }
impl PartialEq for Wake { fn eq(&self, o: &Self) -> bool { (self.at, self.seq) == (o.at, o.seq) } }
impl Eq for Wake {}
impl PartialOrd for Wake { fn partial_cmp(&self, o: &Self) -> Option<std::cmp::Ordering> { Some(self.cmp(o)) } }
impl Ord for Wake { fn cmp(&self, o: &Self) -> std::cmp::Ordering { (o.at, o.seq).cmp(&(self.at, self.seq)) } }
type Clocks = std::sync::mpsc::Sender<(RoomRef, u32)>;
fn clock_thread(cfg: Arc<Cfg>) -> Clocks {
    let (tx, rx) = std::sync::mpsc::channel::<(RoomRef, u32)>();
    std::thread::Builder::new().name("clocks".into()).spawn(move || {
        let mut heap = std::collections::BinaryHeap::<Wake>::new();
        let mut seq = 0u64;
        loop {
            let got = match heap.peek() {
                None => match rx.recv() { Ok(x) => Some(x), Err(_) => return },
                Some(w) => match rx.recv_timeout(w.at.saturating_duration_since(Instant::now())) {
                    Ok(x) => Some(x), Err(std::sync::mpsc::RecvTimeoutError::Timeout) => None, Err(_) => return,
                },
            };
            if let Some((room, race)) = got { seq += 1; heap.push(Wake { at: Instant::now(), seq, room, race }); }
            let now = Instant::now();
            while heap.peek().is_some_and(|w| w.at <= now) {
                let w = heap.pop().unwrap();
                let next = {
                    let mut r = w.room.lock().unwrap();
                    if r.race.id != w.race { continue; }
                    let now = Instant::now();
                    r.advance(now, Some(&cfg));
                    if r.race.id != w.race { None } else { r.next_wake(now, &cfg) }
                };
                if let Some(at) = next { seq += 1; heap.push(Wake { at, seq, room: w.room, race: w.race }); }
            }
        }
    }).unwrap();
    tx
}
fn clock_if_wanted(r: &mut Room, room: &RoomRef, clocks: &Mutex<Clocks>) {
    if let Some(id) = r.clock_wanted.take() { let _ = clocks.lock().unwrap().send((room.clone(), id)); }
}

async fn member_task(sock: WebSocket, app: App, code: String, q: HashMap<String, String>) {
    let (mut wr, mut rd) = sock.split();
    let fail = |msg: &str| Message::Text(json!({"t": "error", "reason": msg}).to_string().into());
    let build = q.get("build").cloned().unwrap_or_default();
    if !app.build_id.is_empty() && build != *app.build_id {
        let _ = wr.send(fail("build mismatch: reload the page")).await; return;
    }
    let name: String = q.get("name").cloned().unwrap_or_else(|| "player".into()).chars().take(16).collect();
    let token = q.get("token").cloned().unwrap_or_default();
    let Some(room) = app.rooms.lock().unwrap().get(&code).cloned() else { let _ = wr.send(fail("lobby not found")).await; return };
    let (tx, mut rx) = mpsc::channel::<Message>(room::QUEUE);
    let kill = Arc::new(Notify::new());
    let joined = room.lock().unwrap().join(&name, &token, tx, kill.clone(), Instant::now());
    let (id, conn) = match joined { Ok(x) => x, Err(e) => { let _ = wr.send(fail(e)).await; return; } };
    // one writer per socket: everything queued goes out in one flush
    let w = tokio::spawn(async move {
        while let Some(m) = rx.recv().await {
            if wr.feed(m).await.is_err() { return; }
            while let Ok(m) = rx.try_recv() { if wr.feed(m).await.is_err() { return; } }
            if wr.flush().await.is_err() { return; }
        }
        let _ = wr.close().await;
    });
    loop {
        let m = tokio::select! {
            _ = kill.notified() => break,
            m = rd.next() => m,
        };
        let Some(Ok(m)) = m else { break };
        let now = Instant::now();
        let mut r = room.lock().unwrap();
        if !r.is_conn(id, conn) { break; }
        r.touched = now;
        match m {
            Message::Text(t) => { if let Ok(v) = serde_json::from_str::<Value>(t.as_str()) { r.on_text(id, &v, &app.cfg, now); } }
            Message::Binary(b) => r.on_bin(id, b, &app.cfg, now),
            Message::Close(_) => break,
            _ => {}
        }
        clock_if_wanted(&mut r, &room, &app.clocks);
    }
    w.abort();
    room.lock().unwrap().leave(id, conn, Instant::now());
}

// Test hook, only when RELAY_TEST=1: start a race without a host socket.
async fn test_start(State(app): State<App>, Path(code): Path<String>, Json(body): Json<Value>) -> StatusCode {
    if std::env::var("RELAY_TEST").is_err() { return StatusCode::NOT_FOUND; }
    let Some(room) = app.rooms.lock().unwrap().get(&code).cloned() else { return StatusCode::NOT_FOUND };
    let mut r = room.lock().unwrap();
    r.members.retain(|m| m.name != "ctl");
    r.start_race(body["config"].clone(), Instant::now(), false);
    clock_if_wanted(&mut r, &room, &app.clocks);
    StatusCode::OK
}

async fn stat(State(app): State<App>) -> Json<Value> {
    let list: Vec<RoomRef> = app.rooms.lock().unwrap().values().cloned().collect();
    let v: Vec<Value> = list.iter().map(|r| { let r = r.lock().unwrap(); json!({"code": r.code, "members": r.members.len(), "racing": r.race.active(), "frame": r.race.next, "replaced": r.race.replaced}) }).collect();
    Json(json!({"build": *app.build_id, "rooms": v}))
}

async fn file(State(app): State<App>, Path(p): Path<String>) -> Response { serve(&app.site, &p).await }
async fn index(State(app): State<App>) -> Response { serve(&app.site, "index.html").await }
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
    let cfg = Arc::new(Cfg {
        grace: e("GRACE_FRAMES", "6").parse().unwrap(),
        silence: Duration::from_millis(e("SILENCE_MS", "3000").parse().unwrap()),
        quiet: Duration::from_millis(e("QUIET_MS", "500").parse().unwrap()),
    });
    let rooms: Rooms = Arc::new(Mutex::new(HashMap::new()));
    let clocks = Arc::new(Mutex::new(clock_thread(cfg.clone())));
    let app = App { rooms: rooms.clone(), cfg, clocks, secret: Arc::new(e("RELAY_SECRET", "")), build_id: Arc::new(e("BUILD_ID", "")), site: Arc::new(e("SITE", "web")), creates: Arc::new(Mutex::new(HashMap::new())) };
    {
        let rooms = rooms.clone();
        tokio::spawn(async move {
            let mut iv = tokio::time::interval(Duration::from_secs(30));
            loop {
                iv.tick().await;
                rooms.lock().unwrap().retain(|_, r| {
                    let r = r.lock().unwrap();
                    let idle = r.touched.elapsed();
                    let empty = r.members.iter().all(|m| !m.online());
                    !(idle > Duration::from_secs(1800) || (empty && idle > Duration::from_secs(120)))
                });
            }
        });
    }
    let build = app.build_id.clone();
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
    println!("relay on :{port} build {}", build);
    axum::serve(listener, router.into_make_service_with_connect_info::<SocketAddr>()).await.unwrap();
}
