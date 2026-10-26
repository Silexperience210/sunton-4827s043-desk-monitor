// ── Page du régulateur (téléphone, secours du Sunton du camion) ──────
// Même API que le Sunton (/reg/*), même registre. Chaque geste est d'abord rangé dans une file locale, puis
// envoyé : une coupure (tunnel, partage de connexion coupé) ne perd rien, et l'écran dit combien attendent.
// Les nombres déclarés sont ceux que le régulateur confirme ; le plan n'est qu'une proposition (estimée).
const $ = (id) => document.getElementById(id);
const store = {
  get(k, d = null) { try { const v = localStorage.getItem(k); return v === null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch { /* navigation privée : la file reste en mémoire */ } },
};
const rid = () => Math.random().toString(36).slice(2, 8);
let device = store.get('vn.reg.device');
if (!device) { device = `tel-${rid()}`; store.set('vn.reg.device', device); }
let token = (() => { try { return localStorage.getItem('vn.token') || new URLSearchParams(location.search).get('token') || ''; } catch { return ''; } })();
const nowS = () => Math.floor(Date.now() / 1000);
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; };

let snap = store.get('vn.reg.snap', null);          // dernier état reçu du serveur
let queue = store.get('vn.reg.queue', []);          // gestes pas encore acceptés par le serveur
let rejected = store.get('vn.reg.rejected', []);    // refusés par le serveur, avec la raison
let doneLocal = store.get('vn.reg.done', []);       // arrêts du plan déjà faits (en attente du nouveau plan)
let online = false, current = null, mode = 'plan';
const saveQ = () => { store.set('vn.reg.queue', queue); store.set('vn.reg.rejected', rejected); store.set('vn.reg.done', doneLocal); };

async function api(path, body) {
  const r = await fetch(path, { method: body ? 'POST' : 'GET', headers: { 'X-VN-Device': device, ...(token ? { Authorization: `Bearer ${token}` } : {}), ...(body ? { 'Content-Type': 'application/json' } : {}) }, body: body ? JSON.stringify(body) : undefined });
  if (r.status === 401) { $('tokenCard').hidden = false; throw Object.assign(new Error('jeton requis'), { auth: true }); }
  const j = await r.json().catch(() => ({}));
  return { status: r.status, j };
}

// ── chargement à bord : celui du serveur + les gestes en file ──
function tourNow() {
  const startQ = queue.find((q) => q.path === '/reg/tour/start');
  if (startQ) return { id: startQ.body.id, capacity: startQ.cap, truckName: startQ.truckName, regulatorName: startQ.regName, local: true };
  if (queue.some((q) => q.path === '/reg/tour/end')) return null;
  return snap?.tour || null;
}
function loadNow() {
  const startQ = queue.find((q) => q.path === '/reg/tour/start');
  const l = startQ ? { ...startQ.body.load } : { ...(snap?.load || { ok: 0, hs: 0 }) };
  for (const q of queue) {
    if (q.path !== '/reg/event' || q.body.impossible) continue;
    l.ok += (q.body.took?.ok || 0) - (q.body.dropped?.ok || 0);
    l.hs += (q.body.took?.hs || 0) - (q.body.dropped?.hs || 0);
  }
  return l;
}

// ── file : envoi dans l'ordre ; un refus du serveur sort de la file avec sa raison ──
let flushing = false;
async function flush() {
  if (flushing || !queue.length) return;
  flushing = true;
  try {
    while (queue.length) {
      const q = queue[0];
      let r;
      try { r = await api(q.path, q.body); } catch (e) { setOnline(false); break; }
      setOnline(true);
      if (r.status >= 500) break;
      queue.shift();
      if (r.status >= 400 || r.j.ok === false) rejected.unshift({ at: nowS(), what: q.label, why: r.j.error || `HTTP ${r.status}` });
      saveQ();
    }
  } finally { flushing = false; }
  render();
  if (!queue.length) refresh();
}
function enqueue(path, body, label, extra = {}) {
  queue.push({ path, body, label, ...extra });
  saveQ();
  render();
  flush();
}

async function refresh() {
  if (queue.length) return flush();
  try {
    const joined = store.get('vn.reg.joined');
    const r = await api(joined ? `/reg/tour?tour=${encodeURIComponent(joined)}` : '/reg/tour');
    if (r.status !== 200) throw new Error(r.j.error || r.status);
    setOnline(true);
    snap = r.j;
    if (joined && !snap.tour) store.set('vn.reg.joined', null);          // tournée rejointe terminée
    // le serveur a recalculé le plan : les arrêts faits localement n'y sont plus
    doneLocal = [];
    store.set('vn.reg.snap', snap);
    saveQ();
    if (!store.get('vn.reg.stations') || Math.random() < 0.05) loadStations();
  } catch (e) { if (!e.auth) setOnline(false); }
  render();
}
async function loadStations() {
  try { const r = await api('/api/stations'); if (r.status === 200) store.set('vn.reg.stations', r.j.filter((s) => s.active).map((s) => ({ sid: s.sid, name: s.name, bikes: s.bikes, docks: s.docks, dis: s.bikesDis }))); } catch { /* hors ligne */ }
}
function setOnline(v) {
  online = v;
  const pill = $('pill');
  pill.classList.toggle('is-live', v);
  pill.classList.toggle('is-off', !v);
  $('pillTxt').textContent = v ? (queue.length ? `ENVOI · ${queue.length}` : 'EN LIGNE') : `HORS LIGNE${queue.length ? ' · ' + queue.length + ' EN ATTENTE' : ''}`;
}

// ── compteurs − / + ──
const counters = {};
function stepper(host, key, label, value, max = 99) {
  counters[key] = value;
  const d = el('div', 'stepper');
  const out = el('output', null, String(value));
  const minus = el('button', null, '−'), plus = el('button', null, '+');
  minus.type = plus.type = 'button';
  minus.setAttribute('aria-label', `${label} : moins`); plus.setAttribute('aria-label', `${label} : plus`);
  minus.onclick = () => { counters[key] = Math.max(0, counters[key] - 1); out.textContent = counters[key]; };
  plus.onclick = () => { counters[key] = Math.min(max, counters[key] + 1); out.textContent = counters[key]; };
  const row = el('div'); row.append(minus, out, plus);
  d.append(el('span', null, label), row);
  host.appendChild(d);
}
function wireStatic(key, def) {
  const box = document.querySelector(`.stepper[data-k="${key}"]`);
  counters[key] = def;
  const out = box.querySelector('output');
  out.textContent = def;
  box.querySelectorAll('button').forEach((b) => { b.onclick = () => { counters[key] = Math.max(0, Math.min(99, counters[key] + +b.dataset.d)); out.textContent = counters[key]; }; });
}

// ── rendu ──
function render() {
  setOnline(online);
  const tour = tourNow();
  $('vStart').hidden = !!tour || mode === 'end';
  $('vTour').hidden = !tour || mode === 'end';
  $('vEnd').hidden = mode !== 'end';
  if (!tour) return renderStart();
  const l = loadNow();
  const cap = tour.capacity || 1;
  $('loadTxt').textContent = `${l.ok + l.hs} / ${cap}`;
  $('loadSub').textContent = `${l.ok} en service · ${l.hs} HS · ${tour.truckName || ''}${tour.regulatorName ? ' · ' + tour.regulatorName : ''}`;
  $('barOk').style.width = `${Math.min(100, 100 * l.ok / cap)}%`;
  $('barHs').style.width = `${Math.min(100, 100 * l.hs / cap)}%`;
  const qn = queue.length;
  $('queueTxt').hidden = !qn && !rejected.length;
  $('queueTxt').textContent = [qn ? `${qn} déclaration(s) en attente d'envoi (gardées sur ce téléphone)` : '', rejected.length ? `${rejected.length} refusée(s) : ${rejected[0].what} — ${rejected[0].why}` : ''].filter(Boolean).join(' · ');
  // arrêt courant : choisi à la main, sinon le premier arrêt du plan pas encore fait
  const stops = (snap?.plan?.stops || []).filter((s) => !doneLocal.includes(`${s.kind}:${s.sid || ''}`));
  const stop = current || stops[0] || null;
  renderStop(stop, !current);
  const nl = $('nextList');
  nl.replaceChildren();
  const rest = current ? stops : stops.slice(1);
  if (!rest.length) nl.appendChild(el('li', 'empty', snap?.plan ? 'Rien d\'autre de prévu pour l\'instant.' : 'Le plan arrive dès la connexion.'));
  for (const s of rest.slice(0, 5)) {
    const li = el('li');
    li.append(el('span', 'n', String(s.seq)), el('span', null, s.kind === 'atelier' ? (snap?.atelier?.name || 'Atelier') : s.name), el('span', 'a', actText(s)));
    nl.appendChild(li);
  }
  $('planMeta').textContent = snap?.plan ? `plan de ${new Date((snap.now || nowS()) * 1000).toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' })} · ≈ ${snap.plan.benefitMin} min de rupture évitées (estimé)` : '';
  $('method').textContent = snap?.method || '';
  const dl = $('doneList');
  dl.replaceChildren();
  const pend = queue.filter((q) => q.path === '/reg/event').map((q) => ({ ...q.body, pending: true }));
  const evs = [...pend.reverse(), ...(snap?.events || [])].slice(0, 8);
  if (!evs.length) dl.appendChild(el('li', 'empty', 'Aucune déclaration pour l\'instant.'));
  for (const e of evs) {
    const li = el('li');
    const v = e.pending ? ['en attente d\'envoi', ''] : !e.check ? ['recoupement dans 10 min', ''] : e.check.verdict === 'concordant' ? ['concordant', 'concordant'] : e.check.verdict === 'ecart' ? [`écart : observé ${fmtS(e.check.observed?.ok)}`, 'ecart'] : e.check.verdict === 'non-visible' ? ['non visible dans le flux', ''] : ['hors flux', ''];
    li.append(el('span', 'n', new Date(e.at * 1000).toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' })),
      el('span', null, `${e.kind === 'atelier' ? 'Atelier' : (e.name || e.sid)} · ${e.impossible ? 'impossible : ' + e.impossible : declText(e)}`),
      el('span', `verdict ${v[1]}`, v[0]));
    dl.appendChild(li);
  }
}
const fmtS = (v) => (v > 0 ? `+${v}` : String(v ?? '—'));
function actText(s) {
  const parts = [];
  if (s.drop?.ok) parts.push(`déposer ${s.drop.ok}`);
  if (s.drop?.hs) parts.push(`décharger ${s.drop.hs} HS`);
  if (s.take?.ok) parts.push(s.kind === 'atelier' ? `reprendre ${s.take.ok} neufs` : `prendre ${s.take.ok}`);
  if (s.take?.hs) parts.push(`ramasser ${s.take.hs} HS`);
  return parts.join(' · ') || '—';
}
function declText(e) {
  const p = [];
  if (e.dropped?.ok) p.push(`déposé ${e.dropped.ok}`);
  if (e.dropped?.hs) p.push(`déchargé ${e.dropped.hs} HS`);
  if (e.took?.ok) p.push(`${e.kind === 'atelier' ? 'repris' : 'pris'} ${e.took.ok}`);
  if (e.took?.hs) p.push(`pris ${e.took.hs} HS`);
  return p.join(', ') || 'rien';
}
function renderStop(stop, planned) {
  $('stopEyebrow').textContent = planned ? 'PROCHAIN ARRÊT' : 'ARRÊT HORS TOURNÉE';
  $('stopBadge').textContent = planned ? 'PLAN ESTIMÉ' : 'CHOISI';
  $('validCard').hidden = !stop;
  if (!stop) {
    $('stopName').textContent = 'Rien à faire pour l\'instant';
    $('stopEta').textContent = ''; $('stopAct').textContent = ''; $('stopWhy').textContent = ''; $('stopNow').textContent = '';
    return;
  }
  const atelier = stop.kind === 'atelier';
  $('stopName').textContent = atelier ? (snap?.atelier?.name || 'Atelier') : stop.name;
  $('stopEta').textContent = planned && stop.etaMin !== undefined ? `dans ~${stop.etaMin} min (estimé)` : '';
  $('stopAct').textContent = planned ? actText(stop) : 'à déclarer';
  $('stopAct').classList.toggle('take', !!(stop.take?.ok && !stop.drop?.ok));
  $('stopWhy').textContent = stop.why || '';
  $('stopNow').textContent = stop.now ? `maintenant : ${stop.now.bikes} vélos · ${stop.now.docks} bornes libres · ${stop.now.dis} HS (mesuré)` : '';
  const host = $('validSteppers');
  host.replaceChildren();
  if (atelier) {
    stepper(host, 'dHs', 'Vélos HS déchargés', stop.drop?.hs || 0);
    stepper(host, 'dOk', 'En service déposés', 0);
    stepper(host, 'tOk', 'Neufs / réparés repris', stop.take?.ok || 0);
    counters.tHs = 0;
  } else {
    stepper(host, 'dOk', 'Vélos déposés', stop.drop?.ok || 0);
    stepper(host, 'tOk', 'Vélos pris', stop.take?.ok || 0);
    stepper(host, 'tHs', 'Vélos HS pris', stop.take?.hs || 0);
    counters.dHs = 0;
  }
  $('btnDone').onclick = () => declare(stop, planned);
  $('btnImpossible').onclick = () => { $('impCard').hidden = false; $('validCard').hidden = true; };
}
function declare(stop, planned, impossible = null) {
  const tour = tourNow();
  if (!tour) return;
  const body = { id: `${device}-${Date.now().toString(36)}-${rid()}`, tour: tour.id, kind: stop.kind, sid: stop.sid || null, name: stop.kind === 'atelier' ? (snap?.atelier?.name || 'Atelier') : stop.name, at: nowS(), seq: (snap?.events?.length || 0) + queue.length + 1,
    took: { ok: impossible ? 0 : counters.tOk || 0, hs: impossible ? 0 : counters.tHs || 0 },
    dropped: { ok: impossible ? 0 : counters.dOk || 0, hs: impossible ? 0 : counters.dHs || 0 },
    impossible, planned: planned ? { seq: stop.seq, take: stop.take, drop: stop.drop } : null };
  if (!impossible) {
    const l = loadNow();
    const ok2 = l.ok + body.took.ok - body.dropped.ok, hs2 = l.hs + body.took.hs - body.dropped.hs;
    if (ok2 < 0 || hs2 < 0) return say(`Impossible : il n'y a que ${l.ok} vélo(s) en service et ${l.hs} HS à bord.`);
    if (ok2 + hs2 > (tour.capacity || 99)) return say(`Impossible : le camion ne prend que ${tour.capacity} vélos (${ok2 + hs2} après cet arrêt).`);
    if (!body.took.ok && !body.took.hs && !body.dropped.ok && !body.dropped.hs) return say('Rien de déclaré : régler les nombres, ou « Impossible ».');
  }
  enqueue('/reg/event', body, `${body.name}`);
  doneLocal.push(`${stop.kind}:${stop.sid || ''}`);
  current = null;
  saveQ();
  say(impossible ? 'Arrêt déclaré impossible.' : 'Déclaré.');
  $('impCard').hidden = true;
  render();
}
const say = (t) => { $('validSay').textContent = t; };

function renderStart() {
  const regs = snap?.regulators || [], trucks = snap?.trucks || [];
  const pr = $('pickReg'), pt = $('pickTruck');
  const selR = store.get('vn.reg.lastReg'), selT = store.get('vn.reg.lastTruck');
  pr.replaceChildren(...regs.map((r) => { const b = el('button', null, r.name); b.type = 'button'; b.setAttribute('aria-pressed', r.id === selR ? 'true' : 'false'); b.onclick = () => { store.set('vn.reg.lastReg', r.id); render(); }; return b; }));
  pt.replaceChildren(...trucks.map((t) => { const b = el('button', null, `${t.name} · ${t.capacity} vélos`); b.type = 'button'; b.setAttribute('aria-pressed', t.id === selT ? 'true' : 'false'); b.onclick = () => { store.set('vn.reg.lastTruck', t.id); render(); }; return b; }));
  $('startHelp').textContent = !snap ? 'Connexion au serveur pour lire la liste des camions…' : !regs.length || !trucks.length ? 'Les camions et les régulateurs se règlent sur la page du serveur (onglet Régulation).' : '';
  // téléphone de secours : rejoindre la tournée déjà commencée sur le Sunton du camion
  const jl = $('joinList');
  const open = (snap?.openTours || []).filter((o) => o.device !== device);
  $('joinCard').hidden = !open.length;
  jl.replaceChildren(...open.map((o) => {
    const b = el('button', null, `${o.regulatorName} · ${o.truckName} · depuis ${new Date(o.startedAt * 1000).toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' })}`);
    b.type = 'button';
    b.onclick = () => { store.set('vn.reg.joined', o.id); refresh(); };
    return b;
  }));
}
$('btnStart').onclick = () => {
  const r = (snap?.regulators || []).find((x) => x.id === store.get('vn.reg.lastReg'));
  const t = (snap?.trucks || []).find((x) => x.id === store.get('vn.reg.lastTruck'));
  if (!r || !t) return ($('startSay').textContent = 'Choisir le régulateur et le camion.');
  const load = { ok: counters.l_ok || 0, hs: counters.l_hs || 0 };
  if (load.ok + load.hs > t.capacity) return ($('startSay').textContent = `Le ${t.name} ne prend que ${t.capacity} vélos.`);
  const id = `t-${device}-${Date.now().toString(36)}`;
  enqueue('/reg/tour/start', { id, truck: t.id, regulator: r.id, load, at: nowS() }, 'début de tournée', { cap: t.capacity, truckName: t.name, regName: r.name });
  try { navigator.wakeLock?.request('screen').catch(() => {}); } catch { /* écran : réglage du téléphone */ }
};
$('btnEnd').onclick = () => { const l = loadNow(); mode = 'end'; wireStatic('e_ok', l.ok); wireStatic('e_hs', l.hs); render(); };
$('btnEndCancel').onclick = () => { mode = 'plan'; render(); };
$('btnEndOk').onclick = () => {
  const tour = tourNow();
  if (tour) enqueue('/reg/tour/end', { tour: tour.id, load: { ok: counters.e_ok || 0, hs: counters.e_hs || 0 }, at: nowS() }, 'fin de tournée');
  mode = 'plan';
  doneLocal = []; current = null; saveQ();
  render();
};
const WHY = ['Station inaccessible (travaux, véhicule garé)', 'Impossible de se garer', 'Camion plein', 'Bornes en panne', 'Plus besoin (déjà équilibrée)', 'Autre'];
$('pickWhy').replaceChildren(...WHY.map((w) => { const b = el('button', null, w); b.type = 'button'; b.onclick = () => { const st = current || (snap?.plan?.stops || []).filter((s) => !doneLocal.includes(`${s.kind}:${s.sid || ''}`))[0]; if (st) declare(st, !current, w); }; return b; }));
$('impCancel').onclick = () => { $('impCard').hidden = true; $('validCard').hidden = false; };
$('btnOther').onclick = () => { $('otherCard').hidden = false; $('validCard').hidden = true; renderOther(); $('otherSearch').focus(); };
$('otherCancel').onclick = () => { $('otherCard').hidden = true; $('validCard').hidden = false; };
$('otherAtelier').onclick = () => { current = { kind: 'atelier' }; $('otherCard').hidden = true; render(); };
$('otherSearch').oninput = renderOther;
function renderOther() {
  const q = $('otherSearch').value.trim().toLowerCase();
  const list = (store.get('vn.reg.stations') || []).filter((s) => !q || s.name.toLowerCase().includes(q) || s.sid === q).slice(0, 30);
  $('otherList').replaceChildren(...list.map((s) => {
    const b = el('button', null, `${s.name} · #${s.sid} · ${s.bikes} vélos, ${s.docks} bornes`);
    b.type = 'button';
    b.onclick = () => { current = { kind: 'station', sid: s.sid, name: s.name, now: { bikes: s.bikes, docks: s.docks, dis: s.dis } }; $('otherCard').hidden = true; render(); };
    return b;
  }));
}
$('tokenForm').addEventListener('submit', (e) => {
  e.preventDefault();
  token = $('tokenInput').value.trim();
  try { localStorage.setItem('vn.token', token); } catch { /* */ }
  $('tokenCard').hidden = true;
  refresh();
});
wireStatic('l_ok', 0); wireStatic('l_hs', 0);
render();
refresh();
setInterval(() => { if (!document.hidden) (queue.length ? flush() : refresh()); }, 20000);
window.addEventListener('online', () => flush());
document.addEventListener('visibilitychange', () => { if (!document.hidden) refresh(); });
