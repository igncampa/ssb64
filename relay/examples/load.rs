//! Load test for the relay: R rooms of 8 bots, each sending a pad every frame like a page and a ping
//! 4 times a second. Measures how late each bundle arrives against its ideal time (t0 + f/60) and the
//! relay's CPU time. Works against the old relay too (json pings with --json-ping).
//!   cargo run --release --example load -- <port> <rooms> <seconds> <relay pid> [--json-ping]
use futures_util::{SinkExt, StreamExt};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tokio_tungstenite::tungstenite::Message;

fn cpu(pid: &str) -> f64 {
    let s = std::fs::read_to_string(format!("/proc/{pid}/stat")).unwrap();
    let f: Vec<&str> = s.rsplit(')').next().unwrap().split_whitespace().collect();
    (f[11].parse::<f64>().unwrap() + f[12].parse::<f64>().unwrap()) / 100.0
}
fn rss(pid: &str) -> String { std::fs::read_to_string(format!("/proc/{pid}/status")).unwrap().lines().find(|l| l.starts_with("VmRSS")).unwrap_or("").to_string() }

#[tokio::main(flavor = "multi_thread")]
async fn main() {
    let a: Vec<String> = std::env::args().collect();
    let (port, rooms, secs, pid) = (a[1].clone(), a[2].parse::<usize>().unwrap(), a[3].parse::<u64>().unwrap(), a[4].clone());
    let json_ping = a.iter().any(|x| x == "--json-ping");
    let arrivals: Arc<Mutex<Vec<Vec<(u32, Instant)>>>> = Arc::new(Mutex::new(vec![]));
    let http = format!("http://127.0.0.1:{port}");
    let mut tasks = vec![];
    for ri in 0..rooms {
        let code = {
            let src = format!("127.0.0.{}", 2 + ri / 9); // the relay allows 10 new lobbies a minute per address
            let out = std::process::Command::new("curl").args(["-s", "--interface", &src, "-X", "POST", "-H", "content-type: application/json", "-d", "{}", &format!("{http}/api/lobby")]).output().unwrap();
            let s = String::from_utf8(out.stdout).unwrap();
            s.split('"').nth(3).unwrap().to_string()
        };
        for k in 0..8 {
            let url = format!("ws://127.0.0.1:{port}/ws/{code}?name=b{k}&token=t{code}{k}&build=");
            let arrivals = arrivals.clone();
            tasks.push(tokio::spawn(async move {
                let (ws, _) = tokio_tungstenite::connect_async(url).await.unwrap();
                let (mut wr, mut rd) = ws.split();
                wr.send(Message::Text(r#"{"t":"engine","state":"ready"}"#.into())).await.unwrap();
                let (txo, mut rxo) = tokio::sync::mpsc::unbounded_channel::<Message>();
                tokio::spawn(async move { while let Some(m) = rxo.recv().await { if wr.send(m).await.is_err() { break; } } });
                                let mut slot = 0u8; let mut mine = vec![]; let end = Instant::now() + Duration::from_secs(secs + 5);
                let mut started = false; let mut sender: Option<tokio::task::JoinHandle<()>> = None;
                while let Ok(Some(Ok(m))) = tokio::time::timeout_at(end.into(), rd.next()).await {
                    match m {
                        Message::Text(t) => {
                            if t.contains("\"welcome\"") { slot = t.split("\"slot\":").nth(1).unwrap().split(|c: char| !c.is_ascii_digit()).next().unwrap().parse().unwrap(); }
                            if t.contains("\"roster\"") && k == 0 && t.matches("\"engine\":\"ready\"").count() == 8 && sender.is_none() && !started { started = true; txo.send(Message::Text(r#"{"t":"start","config":{}}"#.into())).unwrap(); }
                            if t.contains("\"go\"") && sender.is_none() {
                                let tx = txo.clone();
                                tx.send(Message::Text(r#"{"t":"running"}"#.into())).unwrap();
                                let s = slot;
                                sender = Some(tokio::spawn(async move {
                                    let t0 = Instant::now();
                                    let mut iv = tokio::time::interval(Duration::from_micros(16_667));
                                    let mut f: u32 = 0; let mut n = 0u64;
                                    loop {
                                        iv.tick().await;
                                        let mut b = vec![1u8, s]; b.extend_from_slice(&(f + 3).to_be_bytes()); b.push(8);
                                        for i in 0..8u32 { b.extend_from_slice(&[0, (f + 3 - 7 + i) as u8, 0, 0]); }
                                        if tx.send(Message::Binary(b.into())).is_err() { return; }
                                        n += 1;
                                        if n % 15 == 0 {
                                            let c = t0.elapsed().as_secs_f64() * 1000.0;
                                            let m = if json_ping { Message::Text(format!(r#"{{"t":"ping","c":{c}}}"#).into()) } else { let mut p = vec![3u8, 0]; p.extend_from_slice(&c.to_be_bytes()); Message::Binary(p.into()) };
                                            if tx.send(m).is_err() { return; }
                                        }
                                        f += 1;
                                    }
                                }));
                            }
                        }
                        Message::Binary(b) if b[0] == 4 => mine.push((u32::from_be_bytes([b[1], b[2], b[3], b[4]]), Instant::now())),
                        _ => {}
                    }
                }
                if let Some(s) = sender { s.abort(); }
                arrivals.lock().unwrap().push(mine);
            }));
        }
    }
    tokio::time::sleep(Duration::from_millis(3000)).await;
    let c0 = cpu(&pid); let w0 = Instant::now();
    tokio::time::sleep(Duration::from_secs(secs)).await;
    let c1 = cpu(&pid); let wall = w0.elapsed().as_secs_f64();
    let mem = rss(&pid);
    for t in tasks { let _ = t.await; }
    let all = arrivals.lock().unwrap();
    let mut late: Vec<f64> = vec![]; let mut frames = 0usize;
    for v in all.iter() {
        if v.len() < 120 { continue; }
        frames += v.len();
        // t0 as this socket sees it: the earliest arrival less its frame time
        let base = v.iter().map(|(f, t)| *t - Duration::from_micros(*f as u64 * 16_667)).min().unwrap();
        for (f, t) in v.iter().skip(60) { late.push((*t - (base + Duration::from_micros(*f as u64 * 16_667))).as_secs_f64() * 1000.0); }
    }
    late.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let q = |p: f64| late[((late.len() as f64 - 1.0) * p) as usize];
    println!("rooms {rooms} sockets {} bundles {frames} | lateness ms p50 {:.3} p90 {:.3} p99 {:.3} p99.9 {:.3} max {:.3} | relay cpu {:.1}% of a core | {mem}",
        all.len(), q(0.5), q(0.9), q(0.99), q(0.999), late[late.len() - 1], (c1 - c0) / wall * 100.0);
}
