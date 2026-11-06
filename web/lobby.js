// The lobby and the online loop. The game's own menus are never used.
import { Engine } from './engine.js';
import { NetClient } from './net.js';
import { BUILD_ID } from './build.js';
import { readPad } from './input.js';
import { Art } from './art.js';
import { ITEMS, itemMask, itemsFromMask } from './items.js';

const $ = id => document.getElementById(id);
// Fighters by FTKind (src/ft/ftdef.h): a pick sends the kind, and StartMatch takes it as is.
// pic: the portrait it is drawn from (art.js). 12 is Master Hand and is never offered.
const BASE = ['Mario', 'Fox', 'Donkey Kong', 'Samus', 'Luigi', 'Link', 'Yoshi', 'Captain Falcon', 'Kirby', 'Pikachu', 'Jigglypuff', 'Ness'];
const FIGHTERS = [
  ...BASE.map((name, i) => ({ kind: i, name, pic: i })),
  { kind: 13, name: 'Metal Mario', pic: 0, look: 'metal', extra: true },
  { kind: 26, name: 'Giant DK', pic: 2, look: 'giant', extra: true },
  ...BASE.map((n, i) => ({ kind: 14 + i, name: 'Polygon ' + n, pic: i, look: 'poly', extra: true })),
];
const FIGHTER = new Map(FIGHTERS.map(f => [f.kind, f]));
const nameOf = k => FIGHTER.get(k)?.name ?? '?';
// Metal Mario and the polygons have one look only (dFTParamCostumeIDs in src/ft/ftparam.c).
const oneLook = k => FIGHTER.get(k)?.look === 'metal' || FIGHTER.get(k)?.look === 'poly';
// Stages by GRKind (src/gr/grdef.h): the 9 VS stages, then Meta Crystal, Duel Zone, Final Destination.
const STAGES = [["Peach's Castle", 0], ['Sector Z', 1], ['Kongo Jungle', 2], ['Planet Zebes', 3], ['Hyrule Castle', 4], ["Yoshi's Island", 5], ['Dream Land', 6], ['Saffron City', 7], ['Mushroom Kingdom', 8],
  ['Meta Crystal', 13], ['Duel Zone', 14], ['Final Destination', 16]].map(([name, kind]) => ({ name, kind }));
const q = new URLSearchParams(location.search);
const store = { get: k => { try { return localStorage.getItem(k); } catch { return null; } }, set: (k, v) => { try { localStorage.setItem(k, v); } catch {} } };
const embed = window.N64 || null;
let token = embed ? embed.key : store.get('ssb64.token');
if (!token) { token = crypto.randomUUID(); store.set('ssb64.token', token); }
if (embed) document.body.classList.add('n64');

const err = m => { $('err').textContent = m || ''; };
const fill = (el, list) => { el.innerHTML = list.map(o => `<option value="${o.kind}">${o.name}</option>`).join(''); };
fill($('fighter'), FIGHTERS); fill($('stage_'), STAGES);
$('itemList').innerHTML = ITEMS.map((it, i) => `<label class="it"><input type="checkbox" data-i="${i}" checked><span>${it.name}</span></label>`).join('');
const itemBoxes = () => [...document.querySelectorAll('#itemList input')];
const itemsOn = () => itemBoxes().map(b => b.checked);
// Player colours of the game's own HUD, for the little dots on a fighter's tile.
const PCOL = ['var(--p1)', 'var(--p2)', 'var(--p3)', 'var(--p4)', '#b05cf0', '#f07ab8', '#40c8d8', '#f08a3c'];
const COSTUME = [['#d23a2c', '#2d4fd0'], ['#3a8a3c', '#e8e8f0'], ['#f0c030', '#7a3ad0'], ['#30a0e0', '#e05a2c']];
let art = null;
const pics = { fighter: {}, stage: {} };
$('name').value = $('hostName').value = (embed ? q.get('name') : store.get('ssb64.name')) || '';
$('code').value = (q.get('room') || '').toUpperCase();
$('secret').value = store.get('ssb64.secret') || '';

let eng = null, net = null, hostId = null, roster = [], roomRacing = false, waiting = false;
window.sb = { get net() { return net; }, get eng() { return eng; }, get mode() { return mode; } };

// Debug harness flags work only away from a real lobby.
if (q.has('netN')) {
  const { startOffline } = await import('./game.js');
  await startOffline(q); throw 0;
}

// ---- the page's one mode, and the only writer of what is shown ----
//   join        the Join or Host screen (the portal: the room panel, before a socket)
//   connecting  a Join was pressed: the socket opens (and the game may still be loading)
//   room        in the lobby
//   racing      a match runs: the lobby is hidden, the loop and the match's own sound run
// The lobby music plays in `room` only; a match is parked whenever `racing` ends, whatever ended it.
let mode = 'join';
const homeScreen = () => embed ? 'sRoom' : (location.pathname.endsWith('/host') || q.has('host')) ? 'sHost' : 'sJoin';
function setMode(next, msg) {
  const was = mode; mode = next;
  if (msg !== undefined) err(msg);
  const sec = next === 'room' || next === 'racing' ? 'sRoom' : homeScreen();
  for (const s of ['sJoin', 'sHost', 'sRoom']) $(s).hidden = s !== sec;
  $('lobby').hidden = next === 'racing';
  $('strip').hidden = next !== 'racing' || !q.has('stats');
  $('join').disabled = $('newLobby').disabled = next === 'connecting' || Engine.state === 'failed';
  if (next === 'racing') { eng.startAudio().catch(() => {}); startLoop(); }
  else if (was === 'racing') { loopOn = false; try { eng.m._Park(); } catch {} banner.kind = ''; drawBanner(); }
  lobbySound(next === 'room');
  if (next !== 'room' && next !== 'racing') { roster = []; hostId = null; }
  updateStart();
}

const engineP = Engine.load(m => { $('load').textContent = `Loading: ${m}`; });
engineP.then(e => {
  eng = e;
  try {
    art = new Art(e.m.HEAPU8, e.m.dataPtr);
    for (const f of FIGHTERS) pics.fighter[f.kind] = art.portrait(f.pic, f.look);
    for (const g of STAGES) pics.stage[g.kind] = art.stage(g.kind);
    buildTiles(); renderRoster();
  } catch (x) { console.warn('[lobby] pictures', x); art = null; } $('load').textContent = 'Game loaded.'; window.eng = e;
  net?.json({ t: 'engine', state: 'ready' }); updateStart();
}, e => {
  $('load').textContent = '';
  net?.json({ t: 'engine', state: 'failed' });
  setMode(mode === 'room' ? 'room' : 'join', `The game could not load: ${e.message}. Reload the page to try again.`);
});

const wsBase = () => `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}`;

// The portal's only retry: one pending timer, 1, 2, 4 ... 30 s.
let retryT = 0, retryN = 0;
function retryJoin(code) { clearTimeout(retryT); retryT = setTimeout(() => { retryT = 0; join(code); }, Math.min(30000, 1000 * 2 ** retryN++)); }

async function join(code) {
  if (mode === 'connecting' || mode === 'room' || mode === 'racing') return;
  err('');
  const name = ($('sHost').hidden ? $('name') : $('hostName')).value.trim() || 'player';
  if (!embed) store.set('ssb64.name', name);
  setMode('connecting');
  if (Engine.state !== 'ready') {
    err('Still loading the game. You join as soon as it finishes.');
    try { await engineP; } catch { return; }
    err('');
  }
  const client = new NetClient({
    m: eng.m, url: `${wsBase()}${location.pathname.replace(/\/[^/]*$/, '')}/ws/${code}?name=${encodeURIComponent(name)}&token=${token}&build=${BUILD_ID}${embed?.hostKey ? `&hk=${embed.hostKey}` : ''}`,
    padFn: () => readPad(0), lagUp: +(q.get('lagms') || 0), lagDown: +(q.get('lagdown') || 0), jitter: +(q.get('jitter') || 0),
    onEvent: e => { if (client === net) onEvent(e); },
  });
  net = client;
  try { await client.connect(); }
  catch {
    if (client !== net) return;
    net = null;
    setMode('join', embed ? '' : 'Could not reach the server. Check your connection and try again.');
    if (embed) retryJoin(code);
    return;
  }
  if (client !== net || client.gaveUp) return;
  retryN = 0;
  $('roomCode').textContent = code;
  if (!embed) history.replaceState(null, '', `?room=${code}`);
  client.json({ t: 'engine', state: 'ready' });
  sendPick();
  if (mode === 'connecting') { setMode('room'); cue('choose'); }
}

function sendPick() { const f = +$('fighter').value; net?.json({ t: 'pick', pick: { fighter: f, costume: oneLook(f) ? 0 : +$('costume').value } }); }
function config() {
  return { stage: +$('stage_').value, rules: +$('rules').value, time: +$('time').value, stocks: +$('stocks').value, itemRate: +$('items').value,
    itemToggles: itemMask(itemsOn()), cpu: [0, 1, 2, 3, 4, 5, 6, 7].map(i => i < +$('cpu').value ? 1 : 0), levels: Array(8).fill(+$('level').value), fighters: [0, 1, 2, 3, 4, 5, 6, 7], cpuFill: +$('cpu').value };
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
    eng.startAudio().then(syncSound, () => {});
    eng.m._LobbyMusic(soundOn ? 1 : 0, musicLevel());
    requestAnimationFrame(lobbyFrame);
  } else if (!on && lobbyAudio) { lobbyAudio = false; eng.m._LobbyQuiet(); }
  syncSound();
}
function lobbyFrame(t) {
  if (!lobbyAudio) return;
  audioAcc += Math.min(t - audioT, 200); audioT = t;
  while (audioAcc >= 1000 / 60) { audioAcc -= 1000 / 60; eng.m._LobbyAudioTick(); }
  eng.pumpAudio();
  requestAnimationFrame(lobbyFrame);
}
function syncSound() {
  // a browser that wants a press before any sound (an auto-join from a link) gets asked for one
  const waitsForPress = lobbyAudio && soundOn && eng?.audioState === 'suspended';
  $('sound').textContent = waitsForPress ? 'Click for sound' : soundOn ? 'Sound on' : 'Sound off';
  $('sound').classList.toggle('on', soundOn);
  if (lobbyAudio) eng.m._LobbyMusic(soundOn ? 1 : 0, musicLevel());
}
// A browser starts sound only after a press. Every press asks again until the sound runs.
const unlock = () => { if (eng && eng.audioState !== 'running' && (mode === 'room' || mode === 'racing')) eng.startAudio().then(syncSound, () => {}); };
addEventListener('pointerdown', unlock, true); addEventListener('keydown', unlock, true);
// A hidden tab gets no frames: tell the relay at once, so the fighter stands instead of repeating a key.
document.addEventListener('visibilitychange', () => net?.hidden(document.hidden));

// ---- the pictures: the game's own portraits and stage previews (art.js) ----
function buildTiles() {
  if (!art) return;
  $('fighters').innerHTML = ''; $('extras').innerHTML = '';
  FIGHTERS.forEach(f => {
    const i = f.kind, b = document.createElement('button');
    b.className = 'ft' + (f.look ? ' ' + f.look : ''); b.dataset.f = i; b.title = f.name; b.setAttribute('aria-label', f.name);
    b.innerHTML = `<img class="px" alt="" src="${pics.fighter[i]}"><div class="tags"></div>${f.extra ? `<div class="lbl">${f.look === 'poly' ? 'Polygon' : f.name}</div>` : ''}`;
    b.onclick = () => { if (+$('fighter').value === i) return; $('fighter').value = i; cue('fighter', i); sendPick(); renderPicks(); };
    $(f.extra ? 'extras' : 'fighters').append(b);
  });
  $('costumes').querySelectorAll('button').forEach(b => b.remove());
  COSTUME.forEach((c, i) => {
    const b = document.createElement('button');
    b.dataset.c = i; b.title = 'Colour ' + (i + 1); b.style.background = `linear-gradient(135deg,${c[0]} 50%,${c[1]} 50%)`;
    b.onclick = () => { $('costume').value = i; cue('select'); sendPick(); renderPicks(); };
    $('costumes').append(b);
  });
  $('stages').innerHTML = '';
  STAGES.forEach(({ name: n, kind: i }) => {
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
  document.querySelectorAll('.costumes button').forEach(b => { b.classList.toggle('on', !oneLook(me) && +b.dataset.c === col); b.disabled = oneLook(me); });
  document.querySelectorAll('.sg').forEach(b => { b.classList.toggle('on', +b.dataset.s === st); b.disabled = !isHost(); });
}
// The relay keeps the first 16 characters of a name; the portal's list is matched by the same rule.
const relayName = n => [...String(n).trim()].slice(0, 16).join('');
function updateStart() {
  // In the portal, a friend whose game still loads has no socket here yet: the room knows them.
  const loading = roster.filter(m => m.online && m.engine === 'loading');
  const others = loading.concat((embed?.notReady ?? []).map(relayName).filter(n => !roster.some(m => m.name === n)).map(name => ({ name })));
  const host = isHost();
  $('start').disabled = !host || others.length > 0 || roomRacing;
  $('hostOnly').textContent = host ? '' : '(the host chooses)';
  for (const id of ['stage_', 'rules', 'time', 'stocks', 'items', 'cpu', 'level', 'itemsAll', 'itemsNone']) $(id).disabled = !host;
  for (const b of itemBoxes()) b.disabled = !host;
  $('start').hidden = !host;
  $('startNote').textContent = roomRacing ? 'A match is on. You play in the next one.' : !host ? 'Waiting for the host to start.' : others.length ? `Waiting for ${others.map(m => m.name).join(', ')} to load.` : '';
}
function renderRoster() {
  $('roster').innerHTML = '';
  roster.forEach((m, k) => {
    const tr = document.createElement('tr');
    tr.className = (m.id === net?.id ? 'me' : '') + (m.online ? '' : ' off');
    tr.style.borderColor = m.id === net?.id ? '' : PCOL[k % 8] + '66';
    const pic = m.pick && pics.fighter[m.pick.fighter] ? `<img class="px" alt="" src="${pics.fighter[m.pick.fighter]}">` : '?';
    const state = !m.online ? 'disconnected' : m.engine === 'ready' ? (m.id === net?.id ? `${Math.round(net.rtt())} ms` : 'ready') : m.engine === 'failed' ? 'could not load' : 'loading';
    const name = document.createElement('div'); name.className = 'nm'; name.textContent = m.name;
    tr.innerHTML = `<td class="pic" style="box-shadow:inset 0 -3px 0 ${PCOL[k % 8]}">${pic}</td><td class="who"></td>`;
    const who = tr.querySelector('.who'); who.append(name);
    if (m.id === hostId) { const h = document.createElement('span'); h.className = 'host'; h.textContent = 'HOST'; name.append(h); }
    const sub = document.createElement('div'); sub.className = 'st'; sub.textContent = (m.pick ? nameOf(m.pick.fighter) + ' · ' : '') + state; who.append(sub);
    $('roster').append(tr);
  });
  if (art) renderPicks();
}

// ---- the match banner: one element, one owner. A higher kind is never replaced by a lower one;
// stall and desync stay until cleared, info goes by itself. Drawn every frame of the loop. ----
const RANK = { '': 0, info: 1, stall: 2, desync: 3 };
const banner = { kind: '', text: '', until: 0 };
function say(kind, text, ms = 0) {
  if (banner.kind && RANK[banner.kind] > RANK[kind]) return;
  banner.kind = kind; banner.text = text; banner.until = ms ? performance.now() + ms : 0; drawBanner();
}
function unsay(kind) { if (banner.kind === kind) { banner.kind = ''; drawBanner(); } }
function drawBanner() {
  if (banner.until && performance.now() > banner.until) banner.kind = '';
  if ($('banner').textContent !== banner.text) $('banner').textContent = banner.text;
  $('banner').hidden = !banner.kind;
}
const who = slot => roster.find(r => r.slot === slot)?.name ?? 'A player';
const REFUSED = { 'build mismatch: reload the page': 'The game was updated. Reload the page to join.', 'lobby not found': 'That lobby does not exist any more. Ask the host for a new link.', 'lobby full': 'That lobby is full.' };
let desyncT = 0;

function onEvent(v) {
  switch (v.t) {
    case 'welcome': waiting = !!v.race && !v.racer; break;
    case 'roster':
      roster = v.members; hostId = v.host; roomRacing = !!v.racing && mode !== 'racing';
      if (!isHost() && v.config?.stage !== undefined) {
        $('stage_').value = v.config.stage; $('rules').value = v.config.rules; $('time').value = v.config.time; $('stocks').value = v.config.stocks; $('items').value = v.config.itemRate;
        $('cpu').value = v.config.cpuFill ?? 0; $('level').value = v.config.levels?.[0] ?? 5;
        itemsFromMask(v.config.itemToggles ?? 0xFFFFFFFF).forEach((on, i) => { itemBoxes()[i].checked = on; });
      }
      { const st = +$('stage_').value; renderRoster(); updateStart(); if (!isHost() && lobbySt !== null && st !== lobbySt) cue('scroll'); lobbySt = st; }
      break;
    case 'error': {
      // the relay refused this page; whatever was on (a match on a rejoin after an update) stops here
      const text = REFUSED[v.reason] ?? v.reason;
      if (embed) { embed.fatal(v.reason === 'build mismatch: reload the page' ? 'The game was updated. Reload the page.' : text); }
      net = null; setMode('join', text);
      break;
    }
    case 'go': waiting = false; setMode('racing'); break;
    case 'failed': setMode('room', 'This page could not start the match. You can play in the next one.'); break;
    case 'desync':
      say('desync', `Games went out of sync at frame ${v.frame}. Back to the lobby.`);
      clearTimeout(desyncT); desyncT = setTimeout(() => { if (mode === 'racing') setMode('room', 'The last match went out of sync and was stopped.'); }, 6000);
      break;
    case 'away': if (mode === 'racing') say('info', v.me ? 'You were away: your fighter stood still. Catching up…' : `${who(v.slot)} is away. Their fighter stands still.`, 4000); break;
    case 'back': if (mode === 'racing') say('info', v.me ? 'You are back in.' : `${who(v.slot)} is back.`, 2500); break;
    case 'resumed': if (!v.ok && mode === 'racing') setMode('room', 'You were away too long to catch up. You play in the next match.'); break;
    case 'closed': if (mode === 'racing') say('stall', 'Connection lost. Reconnecting…'); else if (mode === 'room') err('Connection lost. Reconnecting…'); break;
    case 'rejoined': unsay('stall'); say('info', 'Reconnected.', 2000); break;
    case 'gone':
      if (v.why === 'left') break;
      net = null;
      if (embed) { setMode('join', ''); retryJoin(q.get('room')); }
      else setMode('join', 'Connection lost. Check your connection and join again.');
      break;
    case 'ended': say('info', 'Match over.', 1500); break;
    case 'over':
      clearTimeout(desyncT);
      if (mode === 'racing') setMode('room', v.why === 'gone' ? 'The match ended while you were away.' : v.why === 'desync' ? 'The last match went out of sync and was stopped for everyone.' : '');
      else if (mode === 'room') { err(''); updateStart(); }
      break;
  }
  if (v.t === 'welcome' || v.t === 'rejoined') { if (mode === 'room') err(''); }
}

let loopOn = false, lobbySt = null;
function startLoop() {
  if (loopOn) return; loopOn = true;
  let last = performance.now(), acc = 0, tLog = last, stalledSince = 0;
  const frame = t => {
    if (!loopOn || !net) return;
    acc += Math.min(t - last, 200); last = t;
    let n = 0;
    while (acc >= 1000 / 60 && n < 4) { acc -= 1000 / 60; n++; }
    for (let i = 0; i < n; i++) net.tick(i === n - 1 ? 1 : 0);
    eng.pumpAudio();
    // the stall banner, with the real reason
    // starving: the relay's frames stopped coming (the picture holds still while this page waits for them)
    const starving = net.state === 'racing' && !net.finished && (net.clockSeen ? performance.now() - net.lastBundle > 250 : performance.now() - net.begunAt > 1500);
    if (net.catching) say('stall', `Catching up… ${Math.ceil(net.behind() / 60)} s behind`);
    else if (starving) {
      stalledSince ||= t;
      if (t - stalledSince > 150) say('stall', net.link !== 'up' ? 'Connection lost. Reconnecting…' : performance.now() - net.lastPong > 1500 ? 'Your connection stopped.' : !net.clock ? 'Waiting for everyone to load…' : 'Waiting for the server…');
    } else { stalledSince = 0; unsay('stall'); }
    drawBanner();
    if (!$('strip').hidden && t - tLog > 500) { tLog = t; const s = net.stats(); $('strip').textContent = `frame ${s.now}  confirmed ${s.confirmed}  rtt ${s.rtt} ms  rollbacks ${s.rollbacks}  deepest ${s.maxDepth}  replaced ${s.replacedBundles}  stalls ${s.stalls}`; }
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
}

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
$('leave').onclick = () => { const c = net; net = null; c?.close('left'); history.replaceState(null, '', location.pathname); setMode('join', ''); };
$('copy').onclick = async () => { const u = `${location.origin}${location.pathname}?room=${$('roomCode').textContent}`; try { await navigator.clipboard.writeText(u); $('copy').textContent = 'Copied'; } catch { prompt('Copy this link', u); } };
$('fighter').onchange = $('costume').onchange = sendPick;
const sendConfig = () => { if (isHost()) net.json({ t: 'config', config: config() }); };
for (const id of ['stage_', 'rules', 'time', 'stocks', 'items', 'cpu', 'level']) $(id).onchange = sendConfig;
for (const b of itemBoxes()) b.onchange = () => { cue('select'); sendConfig(); };
$('itemsAll').onclick = () => { for (const b of itemBoxes()) b.checked = true; cue('select'); sendConfig(); };
$('itemsNone').onclick = () => { for (const b of itemBoxes()) b.checked = false; cue('select'); sendConfig(); };

setMode('join');
if (embed) {
  // The portal already asked for a name and a click. This page is one cart.
  embed.onRoom(m => {
    if (m.notReady) { embed.notReady = m.notReady; updateStart(); }
    // A mic is on in the room: the music goes under the voices, as in the kart lobby.
    if (typeof m.duck === 'boolean' && m.duck !== duck) { duck = m.duck; syncSound(); }
  });
  setInterval(() => embed.report({ engine: Engine.state === 'ready' ? 'ready' : 'booting', playing: mode === 'racing' }), 500);
  join(q.get('room'));
} else if (q.get('room') && $('name').value) join(q.get('room').toUpperCase());

$('sound').onclick = () => { soundOn = !soundOn; store.set('ssb64.sound', soundOn ? '1' : '0'); syncSound(); if (soundOn) cue('select'); };
syncSound();
const syncWide = () => { $('wide').classList.toggle('on', Engine.wide); $('wide').textContent = Engine.wide ? 'Wide screen' : '4:3 screen'; };
$('wide').onclick = () => { Engine.wide = !Engine.wide; store.set('ssb64.wide', Engine.wide ? '1' : '0'); syncWide(); eng?.fitCanvas(); cue('select'); };
syncWide();
