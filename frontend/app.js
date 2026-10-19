import { BleTransport, bleProblem } from './ble.js';
import { DemoTransport } from './demo.js';

const $ = (id) => document.getElementById(id);
const ZONES = ['Rest', 'Light', 'Moderate', 'Vigorous'];
const STATUS_TEXT = {
  connecting: 'Connecting…',
  connected: 'Connected',
  reconnecting: 'Reconnecting…',
  demo: 'Demo',
  disconnected: 'Not connected',
};
const STORE_KEY = 'sockpod.sessions.v1';

let transport = null;
let latest = null;          // most recent live message
let checkedLast = null;     // `last` value we already tried to sync
let sending = false;

// ---------- formatting ----------
const pad = (n) => String(n).padStart(2, '0');
function duration(totalSeconds) {
  const s = Math.max(0, Math.round(totalSeconds || 0));
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  return h ? `${h}:${pad(m)}:${pad(sec)}` : `${m}:${pad(sec)}`;
}
const num = (n) => Number(n || 0).toLocaleString();
const oneDp = (n) => Number(n || 0).toFixed(1);

// ---------- saved sessions ----------
function loadHistory() {
  try {
    const list = JSON.parse(localStorage.getItem(STORE_KEY));
    return Array.isArray(list) ? list : [];
  } catch {
    return [];
  }
}
let history = loadHistory();

function saveHistory() {
  try {
    localStorage.setItem(STORE_KEY, JSON.stringify(history));
  } catch {
    // Storage full or blocked: sessions stay on screen until the page closes
  }
}

const sameSession = (a, source, id) => a.source === source && a.id === id;

function addSession(summary, source) {
  if (!summary || !summary.id || history.some((h) => sameSession(h, source, summary.id))) return;
  history.unshift({ ...summary, source, syncedAt: Date.now() });
  saveHistory();
  renderHistory();
}

// The pod keeps only its most recent finished session, so fetch it as soon as one ends.
async function syncSession(last) {
  checkedLast = last;
  try {
    const summary = await transport.readSession();
    if (summary.id === last) addSession(summary, transport.source);
  } catch (err) {
    console.warn('Could not read the session summary', err);
    checkedLast = null;   // try again on the next update
  }
}

// ---------- live view ----------
function setStatus(state) {
  const pill = $('conn');
  pill.dataset.state = state;
  pill.textContent = STATUS_TEXT[state] ?? state;
  const online = state === 'connected' || state === 'demo' || state === 'reconnecting';
  $('live').hidden = !online;
  $('connect-card').hidden = online;
  $('btn-session').disabled = state !== 'connected' && state !== 'demo';
  if (state === 'disconnected') {
    transport = null;
    latest = null;
    checkedLast = null;
  }
}

function onLive(msg) {
  latest = msg;
  const rec = msg.rec === 1;

  $('rec-state').dataset.rec = rec ? '1' : '0';
  $('rec-label').textContent = rec ? 'Recording' : msg.t > 0 ? 'Last session' : 'Ready';
  $('timer').textContent = duration(msg.t);
  $('active').textContent = duration(msg.act);
  $('rest').textContent = duration(msg.rest);

  if (!sending) {
    const btn = $('btn-session');
    btn.textContent = rec ? 'Stop session' : 'Start session';
    btn.classList.toggle('btn-stop', rec);
    btn.classList.toggle('btn-primary', !rec);
  }

  $('steps').textContent = num(msg.steps);
  $('steps-sub').textContent = rec ? 'This session' : msg.t > 0 ? 'Last session' : ' ';
  $('cad').textContent = num(msg.cad);
  $('cad-sub').textContent = msg.cad === 0 ? 'Not moving'
    : msg.cad >= 180 ? 'Running fast' : msg.cad >= 140 ? 'Running' : 'Walking';

  const zone = Math.min(Math.max(msg.zone | 0, 0), 3);
  $('zone').textContent = ZONES[zone];
  $('zone-meter').dataset.zone = String(zone);
  $('enmo-sub').textContent = `${Number(msg.enmo || 0).toFixed(2)} g of movement`;

  $('jumps').textContent = num(msg.jumps);
  $('jumps-sub').textContent = msg.jumps > 0 ? `Last jump ${oneDp(msg.jlast)} cm` : 'No jumps yet';
  $('sprints').textContent = num(msg.spr);

  const elevCard = $('elev-value').closest('.stat');
  if (msg.baro === 0) {
    elevCard.classList.add('stat-off');
    $('elev-value').innerHTML = '<span>Off</span>';
    $('elev-sub').textContent = 'Pressure sensor not found';
  } else {
    elevCard.classList.remove('stat-off');
    $('elev-value').innerHTML = `<span id="elev-up">${oneDp(msg.up)}</span><small>m up</small>`;
    $('elev-sub').textContent = `${oneDp(msg.dn)} m down · now ${msg.alt >= 0 ? '+' : ''}${oneDp(msg.alt)} m`;
  }

  if (!rec && msg.last > 0 && msg.last !== checkedLast &&
      !history.some((h) => sameSession(h, transport?.source, msg.last))) {
    syncSession(msg.last);
  }
}

// ---------- history view ----------
function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') node.className = v;
    else if (k === 'style') node.style.cssText = v;
    else node.setAttribute(k, v);
  }
  for (const c of children) node.append(c);
  return node;
}

function chip(label, value) {
  return el('span', {}, el('b', {}, value), ` ${label}`);
}

function split(s) {
  const parts = [
    ['Rest', s.rest, 'var(--zone-0)'],
    ['Light', s.light, 'var(--zone-1)'],
    ['Moderate', s.mod, 'var(--zone-2)'],
    ['Vigorous', s.vig, 'var(--zone-3)'],
  ];
  const total = parts.reduce((t, p) => t + (p[1] || 0), 0) || 1;
  const bar = el('div', { class: 'split', role: 'img',
    'aria-label': parts.map(([n, v]) => `${n} ${duration(v)}`).join(', ') });
  const legend = el('div', { class: 'legend' });
  for (const [name, secs, color] of parts) {
    if (secs) bar.append(el('span', { style: `width:${(secs / total) * 100}%;background:${color}` }));
    legend.append(el('span', {}, el('i', { style: `background:${color}` }), `${name} ${duration(secs)}`));
  }
  return el('div', {}, bar, legend);
}

function sessionItem(s) {
  const when = new Date(s.syncedAt);
  const title = el('div', { class: 'session-title' },
    el('span', {}, `Session ${s.id}`, ...(s.source === 'demo' ? [el('span', { class: 'badge' }, 'Demo')] : [])),
    el('time', { datetime: when.toISOString() },
      when.toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' })));
  const plural = (n, word) => (n === 1 ? word : `${word}s`);
  const chips = el('div', { class: 'chips' },
    chip('', duration(s.dur)), chip(plural(s.steps, 'step'), num(s.steps)),
    chip(plural(s.jumps, 'jump'), num(s.jumps)), chip(plural(s.sprints, 'sprint'), num(s.sprints)));

  const facts = [
    ['Active', duration(s.act)],
    ['Rest', duration(s.rest)],
    ['Average cadence', s.cad_avg ? `${num(s.cad_avg)} steps/min` : '—'],
    ['Top cadence', s.cad_max ? `${num(s.cad_max)} steps/min` : '—'],
    ['Highest jump', s.jumps ? `${oneDp(s.jump_max)} cm` : '—'],
    ['Average jump', s.jumps ? `${oneDp(s.jump_avg)} cm` : '—'],
    ['Sprint time', s.sprints ? duration(s.sprint_s) : '—'],
    ['Longest sprint', s.sprints ? duration(s.sprint_max) : '—'],
    ['Climbed', s.baro ? `${oneDp(s.up)} m` : 'Not measured'],
    ['Descended', s.baro ? `${oneDp(s.dn)} m` : 'Not measured'],
  ];
  const dl = el('dl');
  for (const [k, v] of facts) dl.append(el('div', {}, el('dt', {}, k), el('dd', {}, v)));

  return el('li', {}, el('details', {},
    el('summary', {}, title, chips),
    el('div', { class: 'detail' }, split(s), dl)));
}

function renderHistory() {
  const list = $('history-list');
  list.replaceChildren(...history.map(sessionItem));
  $('history-empty').hidden = history.length > 0;
  $('btn-export').disabled = history.length === 0;
  $('btn-clear').disabled = history.length === 0;
}

// ---------- actions ----------
function showNote(text, kind = 'info') {
  const note = $('connect-note');
  note.textContent = text;
  note.dataset.kind = kind;
  note.hidden = !text;
}

async function connect(Transport) {
  showNote('');
  // The demo numbers its sessions after the demo sessions already saved
  const firstId = Math.max(0, ...history.filter((h) => h.source === 'demo').map((h) => h.id));
  transport = new Transport({ onLive, onStatus: setStatus, firstId });
  try {
    await transport.connect();
  } catch (err) {
    transport = null;
    setStatus('disconnected');
    // Closing the device picker isn't an error worth shouting about
    if (err?.name === 'NotFoundError') showNote('No pod selected.');
    else showNote(`Couldn't connect: ${err?.message || err}`, 'error');
  }
}

$('btn-connect').addEventListener('click', () => connect(BleTransport));
$('btn-demo').addEventListener('click', () => connect(DemoTransport));
$('btn-disconnect').addEventListener('click', () => transport?.disconnect());

$('btn-session').addEventListener('click', async () => {
  if (!transport || !latest || sending) return;
  const stopping = latest.rec === 1;
  const btn = $('btn-session');
  sending = true;
  btn.disabled = true;
  btn.textContent = stopping ? 'Stopping…' : 'Starting…';
  try {
    await transport.send(stopping ? 'stop' : 'start');
  } catch (err) {
    alert(`The pod didn't respond: ${err?.message || err}`);
  }
  // The pod confirms with its next live update (within a second)
  setTimeout(() => {
    sending = false;
    btn.disabled = !['connected', 'demo'].includes($('conn').dataset.state);
    if (latest) onLive(latest);
  }, 1200);
});

$('btn-export').addEventListener('click', () => {
  const blob = new Blob([JSON.stringify(history, null, 2)], { type: 'application/json' });
  const link = el('a', { href: URL.createObjectURL(blob), download: 'sockpod-sessions.json' });
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(link.href), 1000);
});

$('btn-clear').addEventListener('click', () => {
  if (!confirm('Delete all saved sessions from this browser?')) return;
  history = [];
  saveHistory();
  renderHistory();
});

// ---------- start ----------
const problem = bleProblem();
if (problem) {
  $('btn-connect').disabled = true;
  showNote(`${problem} You can still try the demo.`);
}
setStatus('disconnected');
renderHistory();
