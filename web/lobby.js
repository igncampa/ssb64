// The lobby and the online loop. The game's own menus are never used.
import { Engine } from './engine.js';
import { NetClient } from './net.js';
import { BUILD_ID } from './build.js';
import { readPad } from './input.js';
import { Art } from './art.js';

const $ = id => document.getElementById(id);
const FIGHTERS = ['Mario', 'Fox', 'Donkey Kong', 'Samus', 'Luigi', 'Link', 'Yoshi', 'Captain Falcon', 'Kirby', 'Pikachu', 'Jigglypuff', 'Ness'];
const STAGES = ["Peach's Castle", 'Sector Z', 'Kongo Jungle', 'Planet Zebes', 'Hyrule Castle', "Yoshi's Island", 'Dream Land', 'Saffron City', 'Mushroom Kingdom'];
const q = new URLSearchParams(location.search);
const store = { get: k => { try { return localStorage.getItem(k); } catch { return null; } }, set: (k, v) => { try { localStorage.setItem(k, v); } catch {} } };
const embed = window.N64 || null;
let token = embed ? embed.key : store.get('ssb64.token');
if (!token) { token = crypto.randomUUID(); store.set('ssb64.token', token); }
if (embed) document.body.classList.add('n64');

const err = m => { $('err').textContent = m || ''; };
const show = id => { for (const s of ['sJoin', 'sHost', 'sRoom']) $(s).hidden = s !== id; };
const fill = (el, names) => { el.innerHTML = names.map((n, i) => `<option value="${i}">${n}</option>`).join(''); };
fill($('fighter'), FIGHTERS); fill($('stage_'), STAGES);
// Player colours of the game's own HUD, for the little dots on a fighter's tile.
const PCOL = ['var(--p1)', 'var(--p2)', 'var(--p3)', 'var(--p4)', '#b05cf0', '#f07ab8', '#40c8d8', '#f08a3c'];
const COSTUME = [['#d23a2c', '#2d4fd0'], ['#3a8a3c', '#e8e8f0'], ['#f0c030', '#7a3ad0'], ['#30a0e0', '#e05a2c']];
let art = null;
const pics = { fighter: [], stage: [] };
$('name').value = $('hostName').value = (embed ? q.get('name') : store.get('ssb64.name')) || '';
$('code').value = (q.get('room') || '').toUpperCase();
$('secret').value = store.get('ssb64.secret') || '';

let eng = null, net = null, hostId = null, engineReady = false, roster = [];
const pings = new Map();
window.sb = { get net() { return net; }, get eng() { return eng; } };

// Debug harness flags work only away from a real lobby.
if (q.has('netN')) {
  const { startOffline } = await import('./game.js');
  await startOffline(q); throw 0;
}

Engine.load(m => { $('load').textContent = `Loading: ${m}`; net?.json({ t: 'engine', state: m }); }).then(e => {
  eng = e; engineReady = true;
  try {
    art = new Art(e.m.HEAPU8, e.m.dataPtr);
    pics.fighter = FIGHTERS.map((_, i) => art.portrait(i)); pics.stage = STAGES.map((_, i) => art.stage(i));
    buildTiles(); renderRoster();
  } catch (x) { console.warn('[lobby] pictures', x); art = null; } $('load').textContent = 'Game loaded.'; window.eng = e;
  net?.json({ t: 'engine', state: 'ready' }); updateStart();
}).catch(e => { $('load').textContent = ''; err(`The game could not load: ${e.message}. Reload the page to try again.`); });

const wsBase = () => `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}`;

async function join(code) {
  err('');
  const name = ($('sHost').hidden ? $('name') : $('hostName')).value.trim() || 'player';
  if (!embed) store.set('ssb64.name', name);
  if (!engineReady) { err('Still loading the game. Your place is saved once it finishes.'); }
  while (!eng) await new Promise(r => setTimeout(r, 100));
  if (embed) err('');
  net = new NetClient({
    m: eng.m, url: `${wsBase()}${location.pathname.replace(/\/[^/]*$/, '')}/ws/${code}?name=${encodeURIComponent(name)}&token=${token}&build=${BUILD_ID}${embed?.hostKey ? `&hk=${embed.hostKey}` : ''}`,
    padFn: () => readPad(0), lagUp: +(q.get('lagms') || 0), lagDown: +(q.get('lagdown') || 0), jitter: +(q.get('jitter') || 0),
    onEvent: onEvent,
  });
  try { await net.connect(); } catch { err('Could not reach the server. Check your connection and try again.'); if (embed) setTimeout(() => join(code), 2000); return; }
  $('roomCode').textContent = code;
  if (!embed) history.replaceState(null, '', `?room=${code}`);
  net.json({ t: 'engine', state: 'ready' });
  sendPick();
  show('sRoom');
  lobbySound(true); cue('choose');
}

function sendPick() { net?.json({ t: 'pick', pick: { fighter: +$('fighter').value, costume: +$('costume').value } }); }
function config() {
  return { stage: +$('stage_').value, rules: +$('rules').value, time: +$('time').value, stocks: +$('stocks').value, itemRate: +$('items').value,
    itemToggles: +$('items').value ? 0xFFFFFFFF : 0, cpu: [0, 1, 2, 3, 4, 5, 6, 7].map(i => i < +$('cpu').value ? 1 : 0), levels: Array(8).fill(+$('level').value), fighters: [0, 1, 2, 3, 4, 5, 6, 7], cpuFill: +$('cpu').value };
}
const isHost = () => net && net.id === hostId;

// ---- lobby sound: the game's select-screen music and menu cues, through its own audio thread ----
const CUE = { scroll: 0, select: 1, denied: 2, fighter: 3, choose: 4, stage: 6 };
let soundOn = store.get('ssb64.sound') !== '0', duck = false, lobbyAudio = false, audioT = 0, audioAcc = 0;
const cue = (k, arg = 0) => { if (soundOn && lobbyAudio && eng?.m._LobbyCue) eng.m._LobbyCue(CUE[k], arg); };
function musicLevel() { return !soundOn ? 0 : duck ? 0x1C00 : 0x5800; }
function lobbySound(on) {
  if (!eng?.m._LobbyMusic) return;
  if (on && !lobbyAudio) {
    lobbyAudio = true; audioT = performance.now(); audioAcc = 0;
    eng.startAudio().catch(() => {});
    eng.m._LobbyMusic(soundOn ? 1 : 0, musicLevel());
    requestAnimationFrame(lobbyFrame);
  } else if (!on && lobbyAudio) { lobbyAudio = false; eng.m._LobbyQuiet(); }
}
function lobbyFrame(t) {
  if (!lobbyAudio) return;
  audioAcc += Math.min(t - audioT, 200); audioT = t;
  while (audioAcc >= 1000 / 60) { audioAcc -= 1000 / 60; eng.m._LobbyAudioTick(); }
  eng.pumpAudio();
  requestAnimationFrame(lobbyFrame);
}
function syncSound() {
  $('sound').textContent = soundOn ? 'Sound on' : 'Sound off';
  $('sound').classList.toggle('on', soundOn);
  if (lobbyAudio) eng.m._LobbyMusic(soundOn ? 1 : 0, musicLevel());
}
// A browser starts sound only after a press. The first press in the room starts the music.
const unlock = () => { if (net && net.state !== 'racing' && !$('sRoom').hidden) { eng?.startAudio().catch(() => {}); } };
addEventListener('pointerdown', unlock, true); addEventListener('keydown', unlock, true);

// ---- the pictures: the game's own portraits and stage previews (art.js) ----
function buildTiles() {
  if (!art) return;
  $('fighters').innerHTML = '';
  FIGHTERS.forEach((n, i) => {
    const b = document.createElement('button');
    b.className = 'ft'; b.dataset.f = i; b.title = n; b.setAttribute('aria-label', n);
    b.innerHTML = `<img class="px" alt="" src="${pics.fighter[i]}"><div class="tags"></div>`;
    b.onclick = () => { if (+$('fighter').value === i) return; $('fighter').value = i; cue('fighter', i); sendPick(); renderPicks(); };
    $('fighters').append(b);
  });
  $('costumes').querySelectorAll('button').forEach(b => b.remove());
  COSTUME.forEach((c, i) => {
    const b = document.createElement('button');
    b.dataset.c = i; b.title = 'Colour ' + (i + 1); b.style.background = `linear-gradient(135deg,${c[0]} 50%,${c[1]} 50%)`;
    b.onclick = () => { $('costume').value = i; cue('select'); sendPick(); renderPicks(); };
    $('costumes').append(b);
  });
  $('stages').innerHTML = '';
  STAGES.forEach((n, i) => {
    const b = document.createElement('button');
    b.className = 'sg'; b.dataset.s = i;
    b.innerHTML = `<img class="px" alt="" src="${pics.stage[i]}"><span>${n}</span>`;
    b.onclick = () => { if (!isHost()) { cue('denied'); return; } if (+$('stage_').value === i) return; $('stage_').value = i; cue('stage'); net.json({ t: 'config', config: config() }); renderPicks(); };
    $('stages').append(b);
  });
  renderPicks();
}
function renderPicks() {
  const me = +$('fighter').value, col = +$('costume').value, st = +$('stage_').value;
  document.querySelectorAll('.ft').forEach(b => {
    const f = +b.dataset.f;
    b.classList.toggle('on', f === me);
    const tags = b.querySelector('.tags'); tags.innerHTML = '';
    roster.forEach((m, k) => { if (m.pick?.fighter === f) { const d = document.createElement('span'); d.style.background = PCOL[k % 8]; d.title = m.name; tags.append(d); } });
  });
  document.querySelectorAll('.costumes button').forEach(b => b.classList.toggle('on', +b.dataset.c === col));
  document.querySelectorAll('.sg').forEach(b => { b.classList.toggle('on', +b.dataset.s === st); b.disabled = !isHost(); });
}
function updateStart() {
  // In the portal, a friend whose game still loads has no socket here yet: the room knows them.
  const others = roster.filter(m => m.engine !== 'ready').concat((embed?.notReady ?? []).filter(n => !roster.some(m => m.name === n)).map(name => ({ name })));
  $('start').disabled = !isHost() || others.length > 0;
  $('hostOnly').textContent = isHost() ? '' : '(the host chooses)';
  for (const id of ['stage_', 'rules', 'time', 'stocks', 'items', 'cpu', 'level']) $(id).disabled = !isHost();
  $('start').hidden = !isHost();
  $('startNote').textContent = !isHost() ? 'Waiting for the host to start.' : others.length ? `Waiting for ${others.map(m => m.name).join(', ')} to load.` : '';
}
function renderRoster() {
  $('roster').innerHTML = '';
  roster.forEach((m, k) => {
    const tr = document.createElement('tr');
    tr.className = (m.id === net?.id ? 'me' : '') + (m.online ? '' : ' off');
    tr.style.borderColor = m.id === net?.id ? '' : PCOL[k % 8] + '66';
    const pic = m.pick && pics.fighter[m.pick.fighter] ? `<img class="px" alt="" src="${pics.fighter[m.pick.fighter]}">` : '?';
    const state = !m.online ? 'disconnected' : m.engine === 'ready' ? (m.id === net?.id ? `${Math.round(net.rtt())} ms` : 'ready') : 'loading';
    const name = document.createElement('div'); name.className = 'nm'; name.textContent = m.name;
    tr.innerHTML = `<td class="pic" style="box-shadow:inset 0 -3px 0 ${PCOL[k % 8]}">${pic}</td><td class="who"></td>`;
    const who = tr.querySelector('.who'); who.append(name);
    if (m.id === hostId) { const h = document.createElement('span'); h.className = 'host'; h.textContent = 'HOST'; name.append(h); }
    const sub = document.createElement('div'); sub.className = 'st'; sub.textContent = (m.pick ? FIGHTERS[m.pick.fighter] + ' · ' : '') + state; who.append(sub);
    $('roster').append(tr);
  });
  if (art) renderPicks();
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
      { const st = +$('stage_').value; renderRoster(); updateStart(); if (!isHost() && lobbySt !== null && st !== lobbySt) cue('scroll'); lobbySt = st; }
      break;
    case 'error':
      if (embed) {
        if (net) net.gaveUp = true;
        // A match is on: this person joins the next one, by itself.
        if (v.reason === 'match in progress') { err('A match is on. You join when it ends.'); setTimeout(() => join(q.get('room')), 3000); break; }
        embed.fatal(v.reason === 'build mismatch: reload the page' ? 'The game was updated. Reload the page.' : v.reason);
        break;
      }
      err(v.reason === 'build mismatch: reload the page' ? 'The game was updated. Reload the page to join.' : v.reason === 'lobby not found' ? 'That lobby does not exist any more. Ask the host for a new link.' : v.reason === 'lobby full' ? 'That lobby is full.' : v.reason === 'match in progress' ? 'A match is in progress. Try again when it ends.' : v.reason); if (net) net.gaveUp = true; show(q.has('host') ? 'sHost' : 'sJoin'); break;
    case 'go': lobbySound(false); $('lobby').hidden = true; $('strip').hidden = !q.has('stats'); eng.startAudio().catch(() => {}); startLoop(); break;
    case 'desync': banner(`Games went out of sync at frame ${v.frame}. Back to the lobby.`); setTimeout(endRace, 3000); break;
    case 'cpu': { const m = roster.find(r => r.slot === v.slot); banner(`${m?.name ?? 'A player'} dropped. A CPU takes over.`, 4000); break; }
    case 'closed': if (net?.state === 'racing') banner('Connection lost. Reconnecting…'); else if (!net?.gaveUp) { err('Connection lost.'); if (embed) setTimeout(() => join(q.get('room')), 1000); } break;
    case 'rejoined': banner('Reconnected.', 2000); break;
    case 'ended': banner('Match over.', 1500); break;
    case 'over': endRace(); break;
  }
}

let loopOn = false, lobbySt = null;
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
function endRace() { loopOn = false; if (net) net.state = 'lobby'; $('lobby').hidden = false; $('strip').hidden = true; $('banner').hidden = true; eng.m._Park(); show('sRoom'); lobbySound(true); }

$('join').onclick = () => { const c = $('code').value.trim().toUpperCase(); if (c.length !== 5) { err('A lobby code has 5 characters.'); return; } join(c); };
$('newLobby').onclick = async () => {
  err(''); store.set('ssb64.secret', $('secret').value);
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

if (embed) {
  // The portal already asked for a name and a click. This page is one cart.
  show('sRoom');
  embed.onRoom(m => {
    if (m.notReady) { embed.notReady = m.notReady; updateStart(); }
    // A mic is on in the room: the music goes under the voices, as in the kart lobby.
    if (typeof m.duck === 'boolean' && m.duck !== duck) { duck = m.duck; syncSound(); }
  });
  setInterval(() => embed.report({ engine: engineReady ? 'ready' : 'booting', playing: net?.state === 'racing' }), 500);
  join(q.get('room'));
} else if (location.pathname.endsWith('/host') || q.has('host')) show('sHost');
else if (q.get('room')) { show('sJoin'); if ($('name').value) join(q.get('room').toUpperCase()); }
else show('sJoin');

$('sound').onclick = () => { soundOn = !soundOn; store.set('ssb64.sound', soundOn ? '1' : '0'); syncSound(); if (soundOn) cue('select'); };
syncSound();
const syncWide = () => { $('wide').classList.toggle('on', Engine.wide); $('wide').textContent = Engine.wide ? 'Wide screen' : '4:3 screen'; };
$('wide').onclick = () => { Engine.wide = !Engine.wide; store.set('ssb64.wide', Engine.wide ? '1' : '0'); syncWide(); eng?.fitCanvas(); cue('select'); };
syncWide();
if (embed) document.body.classList.add('n64');
