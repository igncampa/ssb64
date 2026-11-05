// The lobby and the online loop. The game's own menus are never used.
import { Engine } from './engine.js';
import { NetClient } from './net.js';
import { BUILD_ID } from './build.js';
import { readPad } from './input.js';

const $ = id => document.getElementById(id);
const FIGHTERS = ['Mario', 'Fox', 'Donkey Kong', 'Samus', 'Luigi', 'Link', 'Yoshi', 'Captain Falcon', 'Kirby', 'Pikachu', 'Jigglypuff', 'Ness'];
const STAGES = ["Peach's Castle", 'Sector Z', 'Kongo Jungle', 'Planet Zebes', 'Hyrule Castle', "Yoshi's Island", 'Dream Land', 'Saffron City', 'Mushroom Kingdom'];
const q = new URLSearchParams(location.search);
const store = { get: k => { try { return localStorage.getItem(k); } catch { return null; } }, set: (k, v) => { try { localStorage.setItem(k, v); } catch {} } };
let token = store.get('sb64.token');
if (!token) { token = crypto.randomUUID(); store.set('sb64.token', token); }

const err = m => { $('err').textContent = m || ''; };
const show = id => { for (const s of ['sJoin', 'sHost', 'sRoom']) $(s).hidden = s !== id; };
const fill = (el, names) => { el.innerHTML = names.map((n, i) => `<option value="${i}">${n}</option>`).join(''); };
fill($('fighter'), FIGHTERS); fill($('stage_'), STAGES);
$('name').value = $('hostName').value = store.get('sb64.name') || '';
$('code').value = (q.get('room') || '').toUpperCase();
$('secret').value = store.get('sb64.secret') || '';

let eng = null, net = null, hostId = null, engineReady = false, roster = [];
const pings = new Map();
window.sb = { get net() { return net; }, get eng() { return eng; } };

// Debug harness flags work only away from a real lobby.
if (q.has('netN')) {
  const { startOffline } = await import('./game.js');
  await startOffline(q); throw 0;
}

Engine.load(m => { $('load').textContent = `Loading: ${m}`; net?.json({ t: 'engine', state: m }); }).then(e => {
  eng = e; engineReady = true; $('load').textContent = 'Game loaded.'; window.eng = e;
  net?.json({ t: 'engine', state: 'ready' }); updateStart();
}).catch(e => { $('load').textContent = ''; err(`The game could not load: ${e.message}. Reload the page to try again.`); });

const wsBase = () => `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}`;

async function join(code) {
  err('');
  const name = ($('sHost').hidden ? $('name') : $('hostName')).value.trim() || 'player';
  store.set('sb64.name', name);
  if (!engineReady) { err('Still loading the game. Your place is saved once it finishes.'); }
  while (!eng) await new Promise(r => setTimeout(r, 100));
  net = new NetClient({
    m: eng.m, url: `${wsBase()}/ws/${code}?name=${encodeURIComponent(name)}&token=${token}&build=${BUILD_ID}`,
    padFn: () => readPad(0), lagUp: +(q.get('lagms') || 0), lagDown: +(q.get('lagdown') || 0), jitter: +(q.get('jitter') || 0),
    onEvent: onEvent,
  });
  try { await net.connect(); } catch { err('Could not reach the server. Check your connection and try again.'); return; }
  $('roomCode').textContent = code;
  history.replaceState(null, '', `?room=${code}`);
  net.json({ t: 'engine', state: 'ready' });
  sendPick();
  show('sRoom');
}

function sendPick() { net?.json({ t: 'pick', pick: { fighter: +$('fighter').value, costume: +$('costume').value } }); }
function config() {
  return { stage: +$('stage_').value, rules: +$('rules').value, time: +$('time').value, stocks: +$('stocks').value, itemRate: +$('items').value,
    itemToggles: +$('items').value ? 0xFFFFFFFF : 0, cpu: [0, 1, 2, 3, 4, 5, 6, 7].map(i => i < +$('cpu').value ? 1 : 0), levels: Array(8).fill(+$('level').value), fighters: [0, 1, 2, 3, 4, 5, 6, 7], cpuFill: +$('cpu').value };
}
const isHost = () => net && net.id === hostId;
function updateStart() {
  const others = roster.filter(m => m.engine !== 'ready');
  $('start').disabled = !isHost() || others.length > 0;
  $('hostOnly').textContent = isHost() ? '' : '(the host chooses)';
  for (const id of ['stage_', 'rules', 'time', 'stocks', 'items', 'cpu', 'level']) $(id).disabled = !isHost();
  $('start').hidden = !isHost();
  $('startNote').textContent = !isHost() ? 'Waiting for the host to start.' : others.length ? `Waiting for ${others.map(m => m.name).join(', ')} to load.` : '';
}
function renderRoster() {
  $('roster').innerHTML = '';
  for (const m of roster) {
    const tr = document.createElement('tr');
    const cells = [m.name + (m.id === hostId ? ' (host)' : ''), m.pick ? FIGHTERS[m.pick.fighter] : '-', m.id === net?.id ? `${Math.round(net.rtt())} ms` : '', m.online ? m.engine : 'disconnected'];
    for (const c of cells) { const td = document.createElement('td'); td.textContent = c; tr.append(td); }
    $('roster').append(tr);
  }
}
const banner = (m, ms = 0) => { $('banner').textContent = m; $('banner').hidden = !m; if (ms) setTimeout(() => { $('banner').hidden = true; }, ms); };

function onEvent(v) {
  switch (v.t) {
    case 'roster':
      roster = v.members; hostId = v.host;
      if (!isHost() && v.config?.stage !== undefined) {
        $('stage_').value = v.config.stage; $('rules').value = v.config.rules; $('time').value = v.config.time; $('stocks').value = v.config.stocks; $('items').value = v.config.itemRate;
        $('cpu').value = v.config.cpuFill ?? 0; $('level').value = v.config.levels?.[0] ?? 5;
      }
      renderRoster(); updateStart();
      break;
    case 'error': err(v.reason === 'build mismatch: reload the page' ? 'The game was updated. Reload the page to join.' : v.reason === 'lobby not found' ? 'That lobby does not exist any more. Ask the host for a new link.' : v.reason === 'lobby full' ? 'That lobby is full.' : v.reason === 'match in progress' ? 'A match is in progress. Try again when it ends.' : v.reason); if (net) net.gaveUp = true; show(q.has('host') ? 'sHost' : 'sJoin'); break;
    case 'go': $('lobby').hidden = true; $('strip').hidden = !q.has('stats'); eng.startAudio().catch(() => {}); startLoop(); break;
    case 'desync': banner(`Games went out of sync at frame ${v.frame}. Back to the lobby.`); setTimeout(endRace, 3000); break;
    case 'cpu': { const m = roster.find(r => r.slot === v.slot); banner(`${m?.name ?? 'A player'} dropped. A CPU takes over.`, 4000); break; }
    case 'closed': if (net?.state === 'racing') banner('Connection lost. Reconnecting…'); else if (!net?.gaveUp) err('Connection lost.'); break;
    case 'rejoined': banner('Reconnected.', 2000); break;
    case 'ended': banner('Match over.', 1500); break;
    case 'over': endRace(); break;
  }
}

let loopOn = false;
function startLoop() {
  if (loopOn) return; loopOn = true;
  let last = performance.now(), acc = 0, tLog = last, stalledSince = 0;
  const frame = t => {
    if (!loopOn) return;
    acc += Math.min(t - last, 200); last = t;
    let n = 0;
    while (acc >= 1000 / 60 && n < 4) { acc -= 1000 / 60; n++; }
    for (let i = 0; i < n; i++) net.tick(i === n - 1 ? 1 : 0);
    eng.pumpAudio();
    // stall banner with a reason
    const m = eng.m, starving = m._NetNow() - m._NetConfirmed() >= net.W;
    if (starving) { stalledSince ||= t; if (t - stalledSince > 400) banner(net.rtts.length && performance.now() - net.lastPing < 1000 ? 'Waiting for the server…' : 'Your connection stopped.'); }
    else if (stalledSince) { stalledSince = 0; if (!net.desync) $('banner').hidden = true; }
    if (!$('strip').hidden && t - tLog > 500) { tLog = t; const s = net.stats(); $('strip').textContent = `frame ${s.now}  confirmed ${s.confirmed}  rtt ${s.rtt} ms  rollbacks ${s.rollbacks}  deepest ${s.maxDepth}  replaced ${s.replacedBundles}  stalls ${s.stalls}`; }
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
}
function endRace() { loopOn = false; if (net) net.state = 'lobby'; $('lobby').hidden = false; $('strip').hidden = true; $('banner').hidden = true; eng.m._Park(); show('sRoom'); }

$('join').onclick = () => { const c = $('code').value.trim().toUpperCase(); if (c.length !== 5) { err('A lobby code has 5 characters.'); return; } join(c); };
$('newLobby').onclick = async () => {
  err(''); store.set('sb64.secret', $('secret').value);
  try {
    const r = await fetch('/api/lobby', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ secret: $('secret').value }) });
    const j = await r.json();
    if (!r.ok) { err(j.error || 'Could not create a lobby.'); return; }
    join(j.code);
  } catch { err('Could not reach the server.'); }
};
$('start').onclick = () => { net.json({ t: 'config', config: config() }); net.json({ t: 'start', config: config() }); };
$('leave').onclick = () => { net.gaveUp = true; net.json({ t: 'leave' }); net.ws.close(); history.replaceState(null, '', location.pathname); show(q.has('host') ? 'sHost' : 'sJoin'); };
$('copy').onclick = async () => { const u = `${location.origin}${location.pathname}?room=${$('roomCode').textContent}`; try { await navigator.clipboard.writeText(u); $('copy').textContent = 'Copied'; } catch { prompt('Copy this link', u); } };
$('fighter').onchange = $('costume').onchange = sendPick;
for (const id of ['stage_', 'rules', 'time', 'stocks', 'items', 'cpu', 'level']) $(id).onchange = () => { if (isHost()) net.json({ t: 'config', config: config() }); };

if (location.pathname.endsWith('/host') || q.has('host')) show('sHost');
else if (q.get('room')) { show('sJoin'); if ($('name').value) join(q.get('room').toUpperCase()); }
else show('sJoin');
