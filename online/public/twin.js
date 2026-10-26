// ── Jumeau d'écran : la carte Sunton, dans le navigateur ─────────────
// 1. charge vn-twin.wasm (cœur + UI du firmware) ;
// 2. reprend l'état du serveur (les fichiers de la carte + l'état vivant), comme une carte qui se
//    resynchronise au démarrage ;
// 3. rejoue chaque appel du serveur au moteur, au même instant : les chiffres affichés sont ceux
//    du serveur, au bit près ; l'interface, elle, est celle de la carte, telle quelle.
import { loadEngine, WIFI } from './vn-engine.js';

const $ = (id) => document.getElementById(id);
const canvas = $('screen'), bezel = $('bezel'), veil = $('veil'), veilTxt = $('veilTxt');
const ctx = canvas.getContext('2d');
const back = document.createElement('canvas');           // image 480×272 de la carte
back.width = 480; back.height = 272;
const bctx = back.getContext('2d');
const img = bctx.createImageData(480, 272);

const store = {
  get(k) { try { return localStorage.getItem(k); } catch { return null; } },
  set(k, v) { try { localStorage.setItem(k, v); } catch { /* stockage indisponible : sans conséquence */ } },
};
let token = store.get('vn.token') || new URLSearchParams(location.search).get('token') || '';
const authHeaders = () => (token ? { Authorization: `Bearer ${token}` } : {});
const api = async (path, opts = {}) => {
  const r = await fetch(path, { ...opts, headers: { ...authHeaders(), ...(opts.headers || {}) } });
  if (r.status === 401) { $('tokenCard').hidden = false; throw Object.assign(new Error('jeton requis'), { code: 401 }); }
  return r;
};

// ── Mise en forme (comme la carte : fr-FR) ──
const nbsp = ' ';
const fmtInt = (v) => String(Math.round(v)).replace(/\B(?=(\d{3})+(?!\d))/g, nbsp);
const ago = (t) => {
  if (!t) return 'jamais';
  const s = Math.max(0, Date.now() / 1000 + skew - t);
  if (s < 45) return 'à l\'instant';
  if (s < 3600) return `il y a ${Math.round(s / 60)} min`;
  if (s < 86400) return `il y a ${Math.round(s / 3600)} h`;
  return `il y a ${Math.floor(s / 86400)} j`;
};
const dateFr = (t) => new Date(t * 1000).toLocaleString('fr-FR', { timeZone: 'Europe/Luxembourg', day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' });
const b64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

// ── État ──
let wasmBytes = null, engine = null, seq = 0, queuedSeq = 0, skew = 0, info = null;
let netState = WIFI.CONNECTING, lastEventAt = 0, booted = false, demo = null;
const pending = [];                                       // évènements reçus, pas encore rejoués
const mono = () => 1000 + performance.now();
const epoch = () => Date.now() / 1000 + skew;

function setVeil(text) { if (text) { veilTxt.textContent = text; veil.hidden = false; } else veil.hidden = true; }
function setPill(kind, text) {
  const p = $('pill');
  p.className = `pill ${kind ? 'is-' + kind : ''}`;
  $('pillTxt').textContent = text;
}

// ── Moteur et UI ──
async function startEngine(snap) {
  engine = await loadEngine(wasmBytes, { onLog: (l) => console.debug('[vn]', l) });
  engine.clock(snap.t, mono());
  const set = store.get('vn.settings');                  // Réglages du jumeau (propres à ce navigateur)
  if (set) engine.putFile('/settings.bin', b64(set));
  engine.init();
  engine.settings({ refreshSec: snap.refreshS });
  for (const [name, data] of Object.entries(snap.files)) engine.putFile('/' + name, b64(data));
  const r = engine.load(true);
  engine.setFetching(!!snap.fetching);
  seq = queuedSeq = snap.seq;
  engine.clock(epoch(), mono());
  engine.uiNet(WIFI.CONNECTED, -48, snap.name || 'serveur en ligne', 'Disponibilités en temps réel…');
  engine.uiInit();
  if (demo) engine.uiSync('aperçu autonome (sans serveur)', 2, snap.t, 'Aperçu : instantané réel du serveur, rejoué à son heure');
  else engine.uiSync(new URL('.', location.href).href.replace(/\/$/, ''), 2, snap.t,
    `Jumeau : état du serveur repris (évènement ${snap.seq}), puis rejoué en continu`);
  netState = WIFI.CONNECTED;
  return r;
}

let resyncing = false;
function replay(ev) {
  if (ev.k === 'reset') {                                // le serveur a rechargé son état (carte adoptée)
    pending.length = 0;
    if (!resyncing) resync('le serveur a repris l\'apprentissage d\'une carte');
    return false;
  }
  engine.clock(ev.t, mono());
  switch (ev.k) {
    case 'ingest': engine.ingest(ev.feed, ev.got, ev.code, ev.body, ev.dur, ev.cyc); break;
    case 'cycle': engine.cycleEnd(ev.ok); break;
    case 'fetching': engine.setFetching(ev.on); break;
  }
  seq = ev.seq;
  return true;
}

// ── Toucher : chaque état est tenu au moins 40 ms (LVGL lit le pointeur toutes les 16 ms) ──
const touchQ = [];
let touchHeldSince = 0, touchDown = false, lastXY = [0, 0];
function toScreen(e) {
  const r = canvas.getBoundingClientRect();
  return [Math.round((e.clientX - r.left) * 480 / r.width), Math.round((e.clientY - r.top) * 272 / r.height)];
}
canvas.addEventListener('pointerdown', (e) => { canvas.setPointerCapture(e.pointerId); touchQ.push([...toScreen(e), true]); e.preventDefault(); });
canvas.addEventListener('pointermove', (e) => {
  if (!touchDown && !touchQ.some((q) => q[2])) return;
  const xy = toScreen(e);
  const last = touchQ[touchQ.length - 1];
  if (last && last[2]) { last[0] = xy[0]; last[1] = xy[1]; } else touchQ.push([...xy, true]);
});
const up = (e) => touchQ.push([...toScreen(e), false]);
canvas.addEventListener('pointerup', up);
canvas.addEventListener('pointercancel', up);
canvas.addEventListener('contextmenu', (e) => e.preventDefault());
function pumpTouch(now) {
  if (!touchQ.length) return;
  const next = touchQ[0];
  if (touchDown && !next[2] && now - touchHeldSince < 40) return;      // un tap trop bref serait invisible
  touchQ.shift();
  if (next[2] && !touchDown) touchHeldSince = now;
  touchDown = next[2];
  lastXY = [next[0], next[1]];
  engine.uiTouch(next[0], next[1], next[2]);
}

// ── Rendu : RGB565 → RGBA, puis agrandi sans flou ──
let scaleK = 1, crisp = true;
function resize() {
  const r = canvas.getBoundingClientRect();
  const k = Math.max(1, Math.min(6, Math.ceil(r.width * (window.devicePixelRatio || 1) / 480)));
  if (k !== scaleK || canvas.width !== 480 * k) { scaleK = k; canvas.width = 480 * k; canvas.height = 272 * k; present(); }
}
function present() {
  ctx.imageSmoothingEnabled = !crisp;
  ctx.drawImage(back, 0, 0, canvas.width, canvas.height);
}
function blit() {
  const [x0, y0, x1, y1] = engine.dirty();
  if (x1 < 0) return false;
  const fb = engine.fb(), d = img.data;
  for (let y = y0; y <= y1; y++) {
    for (let x = x0; x <= x1; x++) {
      const c = fb[y * 480 + x], o = (y * 480 + x) * 4;
      const r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
      d[o] = (r << 3) | (r >> 2); d[o + 1] = (g << 2) | (g >> 4); d[o + 2] = (b << 3) | (b >> 2); d[o + 3] = 255;
    }
  }
  bctx.putImageData(img, 0, 0, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
  return true;
}
new ResizeObserver(resize).observe(canvas);
$('btnCrisp').addEventListener('click', (e) => {
  crisp = !crisp;
  e.currentTarget.setAttribute('aria-pressed', String(crisp));
  bezel.classList.toggle('is-crisp', crisp);
  present();
});
if (!bezel.requestFullscreen) $('btnFull').hidden = true;     // iPhone : pas de plein écran pour un élément
$('btnFull').addEventListener('click', () => {
  if (document.fullscreenElement) document.exitFullscreen?.();
  else bezel.requestFullscreen?.().catch(() => {});
});

// ── Boucle d'affichage ──
let settingsSaved = '', settingsAt = 0;
function frame() {
  if (engine) {
    const now = performance.now();
    while (pending.length && !resyncing) if (!replay(pending.shift())) break;
    pumpTouch(now);
    engine.clock(epoch(), mono());
    engine.uiTick();
    if (blit()) present();
    const rq = engine.uiRequests();
    if (rq & 1) requestRefresh();
    if ((rq & 2) && !resyncing && !demo) resync('demandée depuis l\'écran');
    // Réglages modifiés sur l'écran : gardés dans ce navigateur
    if (now - settingsAt > 2000) {
      settingsAt = now;
      const s = engine.getFile('/settings.bin');
      if (s) { const v = btoa(String.fromCharCode(...s)); if (v !== settingsSaved) { settingsSaved = v; store.set('vn.settings', v); } }
    }
  }
  requestAnimationFrame(frame);
}

async function requestRefresh() {
  if (demo) return;
  try { await api('api/refresh', { method: 'POST' }); } catch { /* le bandeau hors ligne le dira */ }
}

// ── Liaison avec le serveur ──
async function getSnapshot() {
  for (;;) {
    try {
      const r = await api('api/twin/snapshot');
      if (r.status === 503) { setVeil('Le serveur attend son premier relevé des flux Vel\'OH!…'); await sleep(5000); continue; }
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const snap = await r.json();
      skew = snap.now - Date.now() / 1000;
      return snap;
    } catch (err) {
      setVeil(err.code === 401 ? 'Jeton requis : voir le panneau de droite.' : `Serveur injoignable (${err.message}). Nouvel essai…`);
      setPill('off', 'HORS LIGNE');
      await sleep(err.code === 401 ? 3000 : 5000);
    }
  }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function eventLoop() {
  let fails = 0;
  for (;;) {
    try {
      const r = await api(`api/twin/events?since=${queuedSeq}`);
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const j = await r.json();
      skew = j.now - Date.now() / 1000;
      if (j.reset) { await resync('le jumeau avait trop de retard'); continue; }
      for (const ev of j.events) if (ev.seq > queuedSeq) { pending.push(ev); queuedSeq = ev.seq; }
      lastEventAt = Date.now();
      fails = 0;
      if (netState !== WIFI.CONNECTED) { netState = WIFI.CONNECTED; engine.uiNet(WIFI.CONNECTED, -48, info?.name || '', ''); }
      setPill('live', 'LIVE');
    } catch (err) {
      fails++;
      if (fails >= 2 && netState !== WIFI.FAILED) { netState = WIFI.FAILED; engine?.uiNet(WIFI.FAILED, 0, '', ''); }
      setPill('off', 'HORS LIGNE');
      await sleep(Math.min(15000, 2000 * fails));
    }
  }
}

async function resync(why) {
  resyncing = true;
  setVeil(`Reprise de l'état du serveur (${why})…`);
  pending.length = 0;
  try {
    const snap = await getSnapshot();
    await startEngine(snap);
  } finally {
    resyncing = false;
    setVeil(null);
  }
}

// ── Panneau « serveur » ──
function badge(el, text, tone) { el.textContent = text; el.className = `badge ${tone || ''}`; }
async function refreshPanel() {
  try {
    const s = await (await fetch('api/summary')).json();
    info = s.server;
    const fresh = s.lastUpdate && Date.now() / 1000 + skew - s.lastUpdate < s.staleS;
    badge($('srvBadge'), !s.lastUpdate ? 'AUCUN RELEVÉ' : fresh ? 'FRAIS' : 'EN RETARD', !s.lastUpdate || !fresh ? 'warn' : 'good');
    $('kvLast').textContent = s.lastUpdate ? `${ago(s.lastUpdate)} · toutes les ${info.refreshS} s` : '—';
    $('kvSamples').textContent = s.learnSince ? `${fmtInt(s.learnSamples)} depuis le ${dateFr(s.learnSince)}` : '—';
    $('kvLearn').textContent = `${Math.round(s.learnPct)} % des créneaux`;
    $('kvDays').textContent = `${s.daysKept} jour${s.daysKept > 1 ? 's' : ''}`;
    $('kvWx').textContent = s.wxSrc === 2 ? `α = ${s.wxAlpha.toFixed(2).replace('.', ',')}` : s.wxSrc === 1 ? 'sans effet mesurable' : 'pas encore';
    $('kvArch').textContent = `${info.archives} instantané${info.archives > 1 ? 's' : ''}`;
    const up = info.uptimeS;
    $('kvUp').textContent = up < 3600 ? `${Math.round(up / 60)} min` : up < 86400 ? `${Math.round(up / 3600)} h` : `${Math.floor(up / 86400)} j`;
    if (engine && booted) {
      const mine = engine.summary();
      const same = mine.version === s.version && JSON.stringify(mine.totals) === JSON.stringify(s.totals);
      if (info.seq === seq) $('twinMeta').textContent = same ? `synchronisé · évènement ${seq} · calcul identique au serveur` : `évènement ${seq} · écart avec le serveur (version ${mine.version} / ${s.version})`;
      else $('twinMeta').textContent = `évènement ${seq} · dernier reçu ${lastEventAt ? ago(lastEventAt / 1000 + skew) : '—'}`;
    }
  } catch { /* le panneau garde ses dernières valeurs */ }
  try {
    const r = await api('api/devices');
    const list = await r.json();
    $('devCount').textContent = String(list.length);
    const ul = $('devs');
    ul.replaceChildren();
    if (!list.length) { const li = document.createElement('li'); li.className = 'empty'; li.textContent = 'Aucun écran ne s\'est encore synchronisé.'; ul.append(li); }
    for (const d of list) {
      const li = document.createElement('li');
      const b = document.createElement('b'); b.textContent = d.id;
      const w = document.createElement('span'); w.className = 'when'; w.textContent = ago(d.last);
      const sm = document.createElement('small');
      sm.textContent = [d.fw && `v${d.fw}`, d.result && `dernière synchro : ${d.result}`, d.took !== undefined && `adopté : ${d.took || 'rien'}`].filter(Boolean).join(' · ');
      li.append(b, w, sm);
      ul.append(li);
    }
  } catch { /* jeton manquant : la carte « jeton requis » le dit */ }
}

$('btnCopy')?.addEventListener('click', async () => {
  const t = $('syncUrl').textContent;
  try { await navigator.clipboard.writeText(t); $('btnCopy').textContent = 'Copié'; }
  catch { const r = document.createRange(); r.selectNodeContents($('syncUrl')); getSelection().removeAllRanges(); getSelection().addRange(r); }
  setTimeout(() => { $('btnCopy').textContent = 'Copier'; }, 1600);
});
$('tokenForm')?.addEventListener('submit', (e) => {
  e.preventDefault();
  token = $('tokenInput').value.trim();
  store.set('vn.token', token);
  $('tokenCard').hidden = true;
});

// ── Aperçu autonome, sans serveur ──
// window.VN_DEMO = { wasm: async () => octets du module, data: async () => ({ snap, events, summary }),
//                    frames: [{ label, src }] } : un instantané réel du serveur et les évènements qui l'ont
// suivi, rejoués à leur heure (puis repris du début) ; à défaut de WebAssembly, des captures de l'écran.
function fillDemoPanel(s) {
  const set = (id, v) => { const el = $(id); if (el) el.textContent = v; };
  set('kvLast', `${dateFr(s.lastUpdate)} · toutes les ${s.server?.refreshS || 60} s`);
  set('kvSamples', s.learnSince ? `${fmtInt(s.learnSamples)} depuis le ${dateFr(s.learnSince)}` : '—');
  set('kvLearn', `${Math.round(s.learnPct)} % des créneaux`);
  set('kvDays', `${s.daysKept} jour${s.daysKept > 1 ? 's' : ''}`);
  set('kvStations', `${s.stations} · ${fmtInt(s.totals.bikes)} vélos dispo`);
}
function demoFallback(err) {
  const fr = demo.frames || [];
  console.warn('aperçu : moteur indisponible', err);
  if (!fr.length) { setVeil(`Erreur : ${err.message}`); return; }
  veil.hidden = true;
  canvas.hidden = true;
  const img = document.createElement('img');
  img.className = 'frame';
  img.src = fr[0].src; img.alt = fr[0].label;
  canvas.parentElement.append(img);
  const bar = document.createElement('div');
  bar.className = 'frames';
  fr.forEach((f) => {
    const b = document.createElement('button');
    b.type = 'button'; b.className = 'btn btn--ghost'; b.textContent = f.label;
    b.addEventListener('click', () => { img.src = f.src; img.alt = f.label; });
    bar.append(b);
  });
  bezel.after(bar);
  $('twinMeta').textContent = 'captures de l\'écran : ce navigateur n\'exécute pas le moteur ici';
  setPill('', 'CAPTURES');
}
async function demoMain() {
  setVeil('Chargement du moteur (cœur + UI du firmware)…');
  let d;
  try {
    wasmBytes = await demo.wasm();
    d = await demo.data();
    await WebAssembly.compile(wasmBytes);
  } catch (err) { demoFallback(err); return; }
  fillDemoPanel(d.summary);
  requestAnimationFrame(frame);
  const evs = d.events, last = evs.length ? evs[evs.length - 1].t : d.snap.t;
  for (;;) {
    skew = d.snap.t - Date.now() / 1000;                // l'horloge du jumeau = celle de l'enregistrement
    pending.length = 0;
    try { await startEngine(d.snap); } catch (err) { demoFallback(err); return; }
    booted = true;
    setVeil(null);
    resize();
    setPill('live', 'APERÇU');
    let i = 0;
    for (;;) {
      const now = epoch();
      while (i < evs.length && evs[i].t <= now) { pending.push(evs[i]); queuedSeq = evs[i].seq; i++; }
      $('twinMeta').textContent = `enregistré le ${dateFr(d.snap.t)} · ${i} / ${evs.length} évènements rejoués à leur heure`;
      if (i >= evs.length && now > last + 75) break;
      await sleep(1000);
    }
  }
}

// ── Démarrage ──
async function main() {
  demo = window.VN_DEMO || null;
  if (demo) return demoMain();
  $('syncUrl').textContent = new URL('sync/manifest', location.href).href;
  setVeil('Chargement du moteur (cœur + UI du firmware)…');
  wasmBytes = await (await fetch('vn-twin.wasm')).arrayBuffer();
  requestAnimationFrame(frame);
  refreshPanel();
  setInterval(refreshPanel, 15000);
  setVeil('Reprise de l\'état du serveur…');
  const snap = await getSnapshot();
  info = { name: snap.name };
  await startEngine(snap);
  booted = true;
  setVeil(null);
  $('twinMeta').textContent = `synchronisé · évènement ${seq}`;
  setPill('live', 'LIVE');
  resize();
  eventLoop();
}
main().catch((err) => { console.error(err); setVeil(`Erreur : ${err.message}`); });
