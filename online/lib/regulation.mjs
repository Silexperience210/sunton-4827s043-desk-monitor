// ── Régulation : interventions DÉCLARÉES, recoupées avec le flux ─────
// Règle du projet (docs/feuille-de-route.md) : une intervention n'existe que si un régulateur la déclare.
// Jamais déduite d'un saut dans le flux — neuf cyclistes qui rendent leurs vélos ensemble ressemblent à un
// camion. Le flux sert seulement à RECOUPER une déclaration, et les deux restent côte à côte : « déclaré +6,
// observé +6 ». Rien n'est corrigé en silence.
//
//   <données>/regulation/config.json       camions (capacité), régulateurs, atelier (point DÉCLARÉ), réglages du plan
//   <données>/regulation/tours.json        tournées : camion, régulateur, appareil, chargement de départ déclaré
//   <données>/regulation/AAAA-MM-JJ.jsonl  une ligne par déclaration (arrêt validé, impossible), puis une ligne
//                                          par recoupement ({"check": id, …}) quand la fenêtre du flux est passée
import { mkdirSync, readFileSync, appendFileSync, readdirSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { atomicWrite } from './store.mjs';
import { dayOf } from '../public/vn-series.js';

export const CFG_DEFAULT = {
  trucks: [],                          // [{id, name, capacity}]
  regulators: [],                      // [{id, name}]
  atelier: { name: 'Atelier', address: 'rue des Romains, Bertrange', lat: null, lon: null },
  plan: {
    speedKmh: 18,                      // vitesse moyenne en ville (estimée ; remplacée par la mesure des tournées déclarées)
    detour: 1.35,                      // distance par la route ≈ vol d'oiseau × détour (estimé)
    stopMin: 4,                        // temps fixe d'un arrêt (se garer, libérer)
    perBikeMin: 0.5,                   // par vélo manipulé
    horizonMin: 120,                   // durée de tournée planifiée d'avance
    maxStops: 6,
    targetFill: 0.5,                   // remplissage visé après une dépose
  },
  atelierStock: { ok: 0, at: 0 },      // vélos neufs ou réparés prêts à l'atelier (DÉCLARÉ)
};

const N = (v) => (Number.isFinite(+v) ? +v : 0);
const I = (v) => Math.max(0, Math.round(N(v)));
const cnt = (o) => ({ ok: I(o?.ok), hs: I(o?.hs) });
export const haversineKm = (a, b) => {
  const R = 6371, rad = Math.PI / 180;
  const dLat = (b.lat - a.lat) * rad, dLon = (b.lon - a.lon) * rad;
  const s = Math.sin(dLat / 2) ** 2 + Math.cos(a.lat * rad) * Math.cos(b.lat * rad) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(s)));
};

// Coordonnées collées par l'utilisateur : « 49.61, 6.08 », lien OpenStreetMap (#map=18/49.61/6.08 ou
// ?mlat=&mlon=) ou Google Maps (@49.61,6.08,17z). null si rien de sûr — jamais de géocodage approximatif.
export function parsePoint(text) {
  const s = String(text || '').trim();
  let m = s.match(/^(-?\d{1,2}\.\d+)\s*[,; ]\s*(-?\d{1,3}\.\d+)$/)
    || s.match(/[#&?]map=\d+\/(-?\d{1,2}\.\d+)\/(-?\d{1,3}\.\d+)/)
    || s.match(/@(-?\d{1,2}\.\d+),(-?\d{1,3}\.\d+)/);
  if (!m) {
    const a = s.match(/[?&]mlat=(-?\d{1,2}\.\d+)/), b = s.match(/[?&]mlon=(-?\d{1,3}\.\d+)/);
    if (a && b) m = [null, a[1], b[1]];
  }
  if (!m) return null;
  const lat = +m[1], lon = +m[2];
  if (!(lat > 49.3 && lat < 50.3 && lon > 5.6 && lon < 6.6)) return null;   // Luxembourg et alentours
  return { lat: +lat.toFixed(6), lon: +lon.toFixed(6) };
}

// Journal à la minute des stations (vélos en service et hors service), tenu à chaque relevé : la matière
// du recoupement. 3 h en mémoire ; au-delà, ou après un redémarrage, l'historique au pas de 5 min prend le relais.
export class MinuteLog {
  constructor(keepS = 3 * 3600) { this.keep = keepS; this.rows = []; this.cols = null; this.key = ''; }
  push(t, stations) {
    const key = stations.map((s) => s.sid).join(',');
    if (key !== this.key) { this.key = key; this.cols = new Map(stations.map((s, i) => [s.sid, i])); this.colsRef = this.cols; }
    const b = new Int16Array(stations.length), d = new Int16Array(stations.length);
    stations.forEach((s, i) => { b[i] = s.live ? s.bikes : -1; d[i] = s.live ? s.bikesDis : -1; });
    if (this.rows.length && this.rows.at(-1).t === t) this.rows.pop();
    this.rows.push({ t, b, d, cols: this.cols });
    while (this.rows.length && this.rows[0].t < t - this.keep) this.rows.shift();
  }
  // [{t, ok, hs}] de la station sur [t0, t1]
  series(sid, t0, t1) {
    const out = [];
    for (const r of this.rows) {
      if (r.t < t0 || r.t > t1) continue;
      const i = r.cols.get(sid);
      if (i === undefined || r.b[i] < 0) continue;
      out.push({ t: r.t, ok: r.b[i], hs: r.d[i] });
    }
    return out;
  }
  covers(t0) { return this.rows.length > 0 && this.rows[0].t <= t0; }
}

// ── Recoupement d'une déclaration avec le flux ──
// Fenêtre : de 20 min avant la validation à 10 min après (le régulateur valide en fin d'arrêt ; le flux a
// une minute de retard). Variation observée = la plus forte variation sur 6 min consécutives dans le sens
// déclaré (les clients bougent la station d'un vélo à la fois ; une dépose de 6 vélos se voit en quelques
// minutes). Concordant si |observé − déclaré| ≤ max(1, 25 % du déclaré). Au pas de 5 min (historique),
// la variation sur la fenêtre entière. Les deux nombres sont toujours rendus, le verdict n'est qu'un résumé.
export const CHECK = { before: 20 * 60, after: 10 * 60, span: 6 * 60 };
function bestMove(series, sign, field, spanS) {
  let best = 0;
  for (let i = 0; i < series.length; i++) {
    for (let j = i + 1; j < series.length && series[j].t - series[i].t <= spanS; j++) {
      const d = series[j][field] - series[i][field];
      if (sign > 0 ? d > best : d < best) best = d;
    }
  }
  return best;
}
export function checkEvent(ev, { minuteLog, history, now }) {
  if (ev.kind !== 'station' || ev.impossible) return { verdict: 'hors-flux', why: ev.kind === 'atelier' ? 'l\'atelier n\'est pas dans le flux' : 'arrêt non fait' };
  const T = ev.at;
  if (now < T + CHECK.after + 60) return null;                         // fenêtre pas encore passée
  const dOk = I(ev.dropped?.ok) - I(ev.took?.ok), dHs = -I(ev.took?.hs);
  const t0 = T - CHECK.before, t1 = T + CHECK.after;
  let s = minuteLog && minuteLog.covers(t0) ? minuteLog.series(ev.sid, t0, t1) : [];
  let method = 'minute';
  if (s.length < 5) {
    method = '5min';
    s = [];
    if (history) {
      for (let t = Math.floor(t0 / 300) * 300; t <= t1; t += 300) {
        const r = history.at(t);
        if (!r) continue;
        const i = r.st.findIndex((x) => x[0] === ev.sid);
        if (i >= 0 && r.b[i] !== null && r.b[i] !== undefined) s.push({ t, ok: r.b[i], hs: null });
      }
    }
  }
  if (s.length < 2) return { verdict: 'non-visible', why: 'aucun relevé de la station autour de l\'heure déclarée', declared: { ok: dOk, hs: dHs }, method };
  const obsOk = method === 'minute' ? bestMove(s, Math.sign(dOk) || 1, 'ok', CHECK.span) : s.at(-1).ok - s[0].ok;
  const obsHs = method === 'minute' && dHs ? bestMove(s, -1, 'hs', CHECK.span) : null;
  const tol = (d) => Math.max(1, Math.round(0.25 * Math.abs(d)));
  let ok = dOk === 0 ? true : Math.abs(obsOk - dOk) <= tol(dOk);
  if (dHs && obsHs !== null) ok = ok && Math.abs(obsHs - dHs) <= tol(dHs);
  return { verdict: ok ? 'concordant' : 'ecart', declared: { ok: dOk, hs: dHs }, observed: { ok: obsOk, hs: obsHs }, method, samples: s.length };
}

export class Regulation {
  constructor({ dataDir, history, log = () => {}, now = () => Math.floor(Date.now() / 1000) }) {
    this.dir = join(dataDir, 'regulation');
    mkdirSync(this.dir, { recursive: true });
    this.history = history;
    this.log = log;
    this.now = now;
    this.minuteLog = new MinuteLog();
    this.cfg = structuredClone(CFG_DEFAULT);
    try { const j = JSON.parse(readFileSync(join(this.dir, 'config.json'), 'utf8')); this.cfg = { ...this.cfg, ...j, plan: { ...this.cfg.plan, ...(j.plan || {}) }, atelier: { ...this.cfg.atelier, ...(j.atelier || {}) } }; } catch { /* premier démarrage */ }
    this.tours = [];
    try { this.tours = JSON.parse(readFileSync(join(this.dir, 'tours.json'), 'utf8')); } catch { /* aucune */ }
    this.events = new Map();           // id → déclaration (+ check)
    this.byTour = new Map();           // tour → [ids]
    this.load30();
  }

  // ── fichiers ──
  saveCfg() { atomicWrite(join(this.dir, 'config.json'), JSON.stringify(this.cfg, null, 1)); }
  saveTours() { atomicWrite(join(this.dir, 'tours.json'), JSON.stringify(this.tours)); }
  dayFiles() { try { return readdirSync(this.dir).filter((f) => /^\d{4}-\d{2}-\d{2}\.jsonl$/.test(f)).sort(); } catch { return []; } }
  load30() {
    const files = this.dayFiles().slice(-60);
    for (const f of files) {
      for (const line of readFileSync(join(this.dir, f), 'utf8').split('\n')) {
        if (!line) continue;
        let o; try { o = JSON.parse(line); } catch { continue; }
        if (o.check) { const e = this.events.get(o.check); if (e) e.check = o.result; continue; }
        this.index(o);
      }
    }
  }
  index(e) {
    this.events.set(e.id, e);
    if (e.tour) { if (!this.byTour.has(e.tour)) this.byTour.set(e.tour, []); this.byTour.get(e.tour).push(e.id); }
  }
  append(obj, t) { appendFileSync(join(this.dir, `${dayOf(t)}.jsonl`), JSON.stringify(obj) + '\n'); }

  // ── réglages ──
  publicConfig() { return this.cfg; }
  updateConfig(p) {
    const c = structuredClone(this.cfg);
    if (Array.isArray(p.trucks)) {
      c.trucks = p.trucks.map((t, k) => ({ id: String(t.id || `c${k + 1}`).slice(0, 16), name: String(t.name || `Camion ${k + 1}`).slice(0, 40), capacity: I(t.capacity) }))
        .filter((t) => t.capacity >= 1 && t.capacity <= 60);
    }
    if (Array.isArray(p.regulators)) {
      c.regulators = p.regulators.map((r, k) => ({ id: String(r.id || `r${k + 1}`).slice(0, 16), name: String(r.name || '').trim().slice(0, 40) })).filter((r) => r.name);
    }
    if (p.atelier) {
      if (p.atelier.name !== undefined) c.atelier.name = String(p.atelier.name).slice(0, 40);
      if (p.atelier.address !== undefined) c.atelier.address = String(p.atelier.address).slice(0, 80);
      if (p.atelier.point !== undefined) {
        if (p.atelier.point === '' || p.atelier.point === null) { c.atelier.lat = null; c.atelier.lon = null; }
        else {
          const pt = parsePoint(p.atelier.point);
          if (!pt) throw new Error('point de l\'atelier non reconnu : coller « 49.61, 6.08 » ou un lien OpenStreetMap / Google Maps qui contient les coordonnées');
          c.atelier.lat = pt.lat; c.atelier.lon = pt.lon;
        }
      }
    }
    if (p.plan) for (const k of Object.keys(CFG_DEFAULT.plan)) if (p.plan[k] !== undefined && Number.isFinite(+p.plan[k]) && +p.plan[k] > 0) c.plan[k] = +p.plan[k];
    if (p.atelierStock !== undefined) c.atelierStock = { ok: I(p.atelierStock.ok ?? p.atelierStock), at: this.now() };
    this.cfg = c;
    this.saveCfg();
    return c;
  }

  // ── tournées ──
  openTourOf(device) { return this.tours.find((t) => t.device === device && !t.endedAt) || null; }
  tour(id) { return this.tours.find((t) => t.id === id) || null; }
  startTour({ id, device, truck, regulator, load, at }) {
    const tr = this.cfg.trucks.find((t) => t.id === truck);
    if (!tr) throw new Error('camion inconnu : le choisir dans la liste des camions');
    const reg = this.cfg.regulators.find((r) => r.id === regulator);
    if (!reg) throw new Error('régulateur inconnu : le choisir dans la liste');
    const l = cnt(load);
    if (l.ok + l.hs > tr.capacity) throw new Error(`chargement de départ (${l.ok + l.hs}) supérieur à la capacité du camion (${tr.capacity})`);
    if (id && this.tour(id)) return { tour: this.tour(id), duplicate: true };
    const prev = this.openTourOf(device);
    if (prev) { prev.endedAt = this.now(); prev.endWhy = 'remplacée par une nouvelle tournée du même appareil'; }
    const t = { id: id || `t${this.now().toString(36)}${Math.random().toString(36).slice(2, 6)}`, device, truck: tr.id, truckName: tr.name, capacity: tr.capacity,
      regulator: reg.id, regulatorName: reg.name, load0: l, startedAt: I(at) || this.now(), receivedAt: this.now(), endedAt: 0 };
    this.tours.push(t);
    this.saveTours();
    this.log(`[régulation] tournée ${t.id} : ${reg.name}, ${tr.name} (${tr.capacity} vélos), à bord ${l.ok} + ${l.hs} HS`);
    return { tour: t };
  }
  endTour({ tour, load, at }) {
    const t = this.tour(tour);
    if (!t) throw new Error('tournée inconnue');
    if (!t.endedAt) {
      t.endedAt = I(at) || this.now();
      if (load) { t.loadEndDeclared = cnt(load); t.loadEndComputed = this.loadOf(t.id); }
      this.saveTours();
    }
    return { tour: t };
  }
  tourEvents(id) { return (this.byTour.get(id) || []).map((k) => this.events.get(k)).filter(Boolean).sort((a, b) => a.at - b.at || a.seq - b.seq); }
  // Chargement à bord : départ déclaré + somme des arrêts déclarés
  loadOf(id, events = this.tourEvents(id)) {
    const t = this.tour(id);
    const l = { ...(t?.load0 || { ok: 0, hs: 0 }) };
    for (const e of events) {
      if (e.impossible) continue;
      l.ok += I(e.took?.ok) - I(e.dropped?.ok);
      l.hs += I(e.took?.hs) - I(e.dropped?.hs);
    }
    return l;
  }

  // ── déclarations ──
  // e : {id, tour, kind: 'station'|'atelier', sid, at, took:{ok,hs}, dropped:{ok,hs}, impossible, planned, seq}
  declare(e, device, stateAtReceipt = null) {
    if (!e || !e.id || !/^[\w.-]{6,64}$/.test(String(e.id))) throw Object.assign(new Error('identifiant de déclaration manquant'), { code: 400 });
    const dup = this.events.get(String(e.id));
    if (dup) return { ok: true, duplicate: true, event: dup, load: dup.tour ? this.loadOf(dup.tour) : null };
    const t = this.tour(e.tour);
    if (!t) throw Object.assign(new Error('tournée inconnue (commencer une tournée)'), { code: 409 });
    const kind = e.kind === 'atelier' ? 'atelier' : 'station';
    const took = cnt(e.took), dropped = cnt(e.dropped);
    if (kind === 'station' && dropped.hs) throw Object.assign(new Error('on ne dépose pas de vélo hors service en station'), { code: 400 });
    if (kind === 'atelier' && took.hs) throw Object.assign(new Error('on ne reprend pas de vélo hors service à l\'atelier'), { code: 400 });
    if (kind === 'station' && !e.sid) throw Object.assign(new Error('station manquante'), { code: 400 });
    const ev = { id: String(e.id), tour: t.id, device, regulator: t.regulator, truck: t.truck, kind, sid: kind === 'station' ? String(e.sid) : null,
      name: String(e.name || '').slice(0, 60), at: I(e.at) || this.now(), receivedAt: this.now(), seq: I(e.seq),
      took, dropped, impossible: e.impossible ? String(e.impossible).slice(0, 80) : null, planned: e.planned || null,
      state: stateAtReceipt };                                          // état de la station à la réception (mesuré)
    if (ev.at > this.now() + 300) throw Object.assign(new Error('heure de l\'appareil dans le futur : vérifier l\'heure du Sunton'), { code: 400 });
    if (!ev.impossible) {
      const after = this.loadOf(t.id, [...this.tourEvents(t.id), ev]);
      if (after.ok < 0 || after.hs < 0) throw Object.assign(new Error(`chargement négatif après cet arrêt (${after.ok} + ${after.hs} HS) : vérifier les nombres déclarés`), { code: 409 });
      if (after.ok + after.hs > t.capacity) throw Object.assign(new Error(`chargement supérieur à la capacité (${after.ok + after.hs} / ${t.capacity})`), { code: 409 });
    }
    this.index(ev);
    this.append(ev, ev.at);
    if (kind === 'atelier' && !ev.impossible && took.ok) {               // le stock déclaré de l'atelier baisse d'autant
      this.cfg.atelierStock = { ok: Math.max(0, I(this.cfg.atelierStock?.ok) - took.ok), at: this.now() };
      this.saveCfg();
    }
    return { ok: true, event: ev, load: this.loadOf(t.id) };
  }

  // ── à chaque relevé : journal à la minute, recoupements dont la fenêtre est passée ──
  onCycle(stations, t = this.now()) {
    this.minuteLog.push(t, stations);
    let n = 0;
    for (const e of this.events.values()) {
      if (e.check || e.at < t - 2 * 86400) continue;
      const r = checkEvent(e, { minuteLog: this.minuteLog, history: this.history, now: t });
      if (!r) continue;
      e.check = r;
      this.append({ check: e.id, result: r }, e.at);
      n++;
    }
    return n;
  }

  // Stations à ne pas replanifier : arrêt déclaré (fait ou impossible) depuis moins de 30 min
  recentStops(t = this.now(), windowS = 1800) {
    const out = new Set();
    for (const e of this.events.values()) if (e.kind === 'station' && e.at >= t - windowS) out.add(e.sid);
    return out;
  }
  // Position du camion : dernier arrêt fait (station ou atelier) ; null = inconnue (le plan part de l'atelier)
  positionOf(tourId, coords) {
    const last = this.tourEvents(tourId).filter((e) => !e.impossible).at(-1);
    if (!last) return null;
    if (last.kind === 'atelier') return Number.isFinite(this.cfg.atelier.lat) ? { lat: this.cfg.atelier.lat, lon: this.cfg.atelier.lon } : null;
    return coords.get(last.sid) || null;
  }
  // Trajets entre deux arrêts faits consécutifs d'une même tournée (heures DÉCLARÉES) : matière de la vitesse mesurée
  travelSamples(coords) {
    const out = [];
    const atelier = Number.isFinite(this.cfg.atelier.lat) ? { lat: this.cfg.atelier.lat, lon: this.cfg.atelier.lon } : null;
    const pos = (e) => (e.kind === 'atelier' ? atelier : coords.get(e.sid));
    for (const t of this.tours) {
      const ev = this.tourEvents(t.id).filter((e) => !e.impossible);
      for (let k = 1; k < ev.length; k++) {
        const a = pos(ev[k - 1]), b = pos(ev[k]);
        if (!a || !b) continue;
        out.push({ km: haversineKm(a, b), min: (ev[k].at - ev[k - 1].at) / 60, bikes: ev[k].took.ok + ev[k].took.hs + ev[k].dropped.ok + ev[k].dropped.hs });
      }
    }
    return out;
  }

  // ── lecture ──
  eventsBetween(from, to) {
    return [...this.events.values()].filter((e) => { const d = dayOf(e.at); return d >= from && d <= to; }).sort((a, b) => b.at - a.at);
  }
  state() {
    const open = this.tours.filter((t) => !t.endedAt).map((t) => ({ ...t, load: this.loadOf(t.id), stops: this.tourEvents(t.id).length,
      last: this.tourEvents(t.id).at(-1) || null }));
    return { config: this.cfg, open, recentTours: this.tours.slice(-20).reverse() };
  }

  // ── Indicateurs : seulement du déclaré et du mesuré, chaque valeur avec sa classe ──
  kpi(from, to) {
    const ev = this.eventsBetween(from, to);
    const days = new Map();
    const day = (d) => { if (!days.has(d)) days.set(d, { day: d, stops: 0, impossible: 0, dropOk: 0, takeOk: 0, takeHs: 0, atelierIn: 0, atelierOut: 0, concordant: 0, ecart: 0, nonVisible: 0, pending: 0 }); return days.get(d); };
    for (const e of ev) {
      const d = day(dayOf(e.at));
      if (e.impossible) { d.impossible++; continue; }
      d.stops++;
      if (e.kind === 'station') {
        d.dropOk += e.dropped.ok; d.takeOk += e.took.ok; d.takeHs += e.took.hs;
        const v = e.check?.verdict;
        if (!v) d.pending++; else if (v === 'concordant') d.concordant++; else if (v === 'ecart') d.ecart++; else if (v === 'non-visible') d.nonVisible++;
      } else { d.atelierIn += e.dropped.ok + e.dropped.hs; d.atelierOut += e.took.ok; }
    }
    // délai de traitement : dépose déclarée dans une station vide (mesuré au pas de 5 min dans l'historique)
    const delays = [];
    for (const e of ev) {
      if (e.kind !== 'station' || e.impossible || !e.dropped.ok || !this.history) continue;
      const slot = Math.floor((e.at - 300) / 300) * 300;
      const at = this.history.at(slot);
      const i = at ? at.st.findIndex((x) => x[0] === e.sid) : -1;
      if (i < 0 || at.b[i] !== 0) continue;
      let t0 = slot;
      for (let k = 1; k <= 288; k++) {
        const r = this.history.at(slot - k * 300);
        const j = r ? r.st.findIndex((x) => x[0] === e.sid) : -1;
        if (j < 0 || r.b[j] !== 0) break;
        t0 = slot - k * 300;
      }
      delays.push({ sid: e.sid, name: e.name, at: e.at, emptySince: t0, minutes: Math.round((e.at - t0) / 60) });
    }
    const med = (a) => { if (!a.length) return null; const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length / 2)]; };
    // temps entre deux arrêts validés de la même tournée (déclaré) et distance à vol d'oiseau : la vitesse mesurée
    return {
      from, to, classes: { stops: 'déclaré', bikes: 'déclaré', verdicts: 'déclaré recoupé avec le flux', delays: 'déclaré + mesuré (pas de 5 min)' },
      days: [...days.values()].sort((a, b) => (a.day < b.day ? -1 : 1)),
      delays: { n: delays.length, medianMin: med(delays.map((d) => d.minutes)), list: delays.slice(0, 20) },
    };
  }
}
