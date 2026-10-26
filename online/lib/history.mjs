// ── Historique long : chaque tranche de 5 min, gardée sans limite ────
// La carte ne garde que 24 h (l'anneau de Model) ; le serveur, allumé en permanence, garde tout : de quoi
// tracer des semaines, comparer deux lundis, rejouer une période au banc.
//
// Disposition : <données>/historique/AAAA-MM-JJ.jsonl (jour local de Luxembourg), compressé en .jsonl.gz
// une fois le jour fini. Une ligne par objet JSON :
//   {"st":[[sid, nom, capacité, lat, lon], …]}                    stations, dans l'ordre des colonnes « b »
//   {"t":…, "n":[dispo, bornes, HS, bruts, retraits, retours, vides, pleines], "w":[°C, mm, %, km/h, WMO], "b":[…]}
// « n », « w », « b » sont exactement ce que l'anneau de la carte contient pour la tranche (NetSample et
// vélos par station : ce que /vn_hist.bin sauvegarde, ce que le banc rejoue) ; null = inconnu. Une ligne
// « st » ouvre chaque fichier et revient quand la liste des stations change dans la journée.
//
// Une tranche est écrite quand la suivante s'ouvre : elle est alors définitive (retraits et retours compris).
import { mkdirSync, readdirSync, readFileSync, writeFileSync, appendFileSync, existsSync, statSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { gzipSync, gunzipSync } from 'node:zlib';
import { atomicWrite } from './store.mjs';
import { SLOT_S, dayOf, luxParts, aggregate, dailySummary } from '../public/vn-series.js';

export { SLOT_S, dayOf };
const DAY_RE = /^(\d{4}-\d{2}-\d{2})\.jsonl(\.gz)?$/;
const stKey = (st) => st.map((s) => `${s[0]}:${s[2]}`).join(',');

// Lit un fichier de jour (clair ou compressé) : [{t, n, w, b, st}] où st est la ligne « st » en vigueur
export function parseDay(buf) {
  const text = buf.toString('utf8');
  const out = [];
  let st = null;
  for (const line of text.split('\n')) {
    if (!line) continue;
    let o;
    try { o = JSON.parse(line); } catch { continue; }      // ligne tronquée par une coupure : ignorée
    if (o.st) { st = o.st; continue; }
    if (typeof o.t === 'number' && st) { o.st = st; out.push(o); }
  }
  return out;
}

function serialize(recs) {
  let s = '', key = null;
  for (const r of recs) {
    const k = stKey(r.st);
    if (k !== key) { s += JSON.stringify({ st: r.st }) + '\n'; key = k; }
    s += JSON.stringify({ t: r.t, n: r.n ?? null, w: r.w ?? null, b: r.b }) + '\n';
  }
  return s;
}

export class History {
  constructor(dataDir, log = () => {}) {
    this.dir = join(dataDir, 'historique');
    this.log = log;
    mkdirSync(this.dir, { recursive: true });
    this.lastT = 0;            // dernière tranche écrite
    this.curDay = '';          // fichier du jour en cours d'écriture
    this.curKey = '';          // liste de stations de la dernière ligne « st » de ce fichier
    this.cache = new Map();    // jour fini → enregistrements (lecture)
    this.stCache = null;       // stations du moteur, mises en cache par clé
    this._scanTail();
  }

  // ── fichiers ──
  files() {
    // relu au plus toutes les 500 ms (une note de journée en demande des milliers d'affilée)
    const now = Date.now();
    if (this._files && now - this._filesAt < 500) return this._files;
    this._files = this._listFiles();
    this._filesAt = now;
    return this._files;
  }
  _listFiles() {
    let names = [];
    try { names = readdirSync(this.dir); } catch { return []; }
    const by = new Map();
    for (const n of names) {
      const m = n.match(DAY_RE);
      if (!m) continue;
      const cur = by.get(m[1]);
      // si les deux existent (coupure pendant la compression), le clair fait foi
      if (!cur || !m[2]) by.set(m[1], { day: m[1], gz: !!m[2], path: join(this.dir, n), bytes: statSync(join(this.dir, n)).size });
    }
    return [...by.values()].sort((a, b) => (a.day < b.day ? -1 : 1));
  }
  _read(f) {
    try {
      const b = readFileSync(f.path);
      return parseDay(f.gz ? gunzipSync(b) : b);
    } catch (err) { this.log(`[historique] ${f.path} illisible : ${err.message}`); return []; }
  }
  readDay(day) {
    const f = this.files().find((x) => x.day === day);
    if (!f) return [];
    // cache par jour, invalidé dès que le fichier change (taille ou nature) : le jour en cours grandit
    const key = `${f.gz ? 'z' : 'p'}${f.bytes}`;
    const c = this.cache.get(day);
    if (c && c.key === key) return c.recs;
    const recs = this._read(f);
    this.cache.delete(day);
    this.cache.set(day, { key, recs });
    if (this.cache.size > 120) this.cache.delete(this.cache.keys().next().value);
    return recs;
  }
  _scanTail() {
    const fs = this.files();
    for (let i = fs.length - 1; i >= 0; i--) {
      const recs = this._read(fs[i]);
      if (recs.length) { this.lastT = recs.at(-1).t; break; }
    }
  }

  // ── écriture ──
  _stations(engine) {
    const st = engine.stations().map((s) => [s.sid, s.name, s.cap, s.lat, s.lon]);
    return st;
  }
  // Ajoute des tranches (triées, toutes postérieures à lastT) au fichier de leur jour
  append(recs) {
    for (const r of recs) {
      if (r.t <= this.lastT) continue;
      const day = dayOf(r.t);
      if (day !== this.curDay) {
        this.curDay = day;
        const p = join(this.dir, `${day}.jsonl`);
        // fichier du jour déjà commencé (redémarrage) : reprendre sa dernière liste de stations
        this.curKey = existsSync(p) ? (() => { const x = parseDay(readFileSync(p)); return x.length ? stKey(x.at(-1).st) : ''; })() : '';
        this.compressDone(day);
      }
      const p = join(this.dir, `${day}.jsonl`);
      const k = stKey(r.st);
      let s = '';
      if (k !== this.curKey) { s += JSON.stringify({ st: r.st }) + '\n'; this.curKey = k; }
      s += JSON.stringify({ t: r.t, n: r.n ?? null, w: r.w ?? null, b: r.b }) + '\n';
      appendFileSync(p, s);
      this._files = null;
      this.lastT = r.t;
    }
  }
  // Relève dans l'anneau du moteur les tranches finies pas encore écrites (appelé après chaque cycle,
  // et au démarrage : l'anneau relu de /vn_hist.bin comble ce qui manque depuis la dernière écriture)
  capture(engine) {
    const head = engine.ringHead();
    if (!head || head === this.lastHead) return 0;
    this.lastHead = head;
    const got = [];
    for (let back = 1; back < 400; back++) {
      const s = engine.slot(back);
      if (!s || s.t <= this.lastT) break;
      if (s.n === null && !s.b.some((v) => v !== null)) continue;     // tranche vide (carte éteinte, trou)
      got.push(s);
    }
    if (!got.length) return 0;
    const st = this._stations(engine);
    got.reverse();
    for (const s of got) s.st = st;
    this.append(got);
    return got.length;
  }
  // Les jours finis passent en .jsonl.gz (≈ 6 fois moins de place)
  compressDone(today = dayOf(Math.floor(Date.now() / 1000))) {
    for (const f of this.files()) {
      if (f.gz || f.day >= today) continue;
      try {
        const raw = readFileSync(f.path);
        atomicWrite(f.path + '.gz', gzipSync(raw, { level: 9 }));
        rmSync(f.path);
        this._files = null;
      } catch (err) { this.log(`[historique] compression de ${f.day} impossible : ${err.message}`); }
    }
  }
  // Fusionne des tranches venues d'ailleurs (archive, vidage de carte) : seules celles qui manquent entrent
  merge(recs) {
    const byDay = new Map();
    for (const r of recs) { const d = dayOf(r.t); if (!byDay.has(d)) byDay.set(d, []); byDay.get(d).push(r); }
    const today = dayOf(Math.floor(Date.now() / 1000));
    let added = 0;
    for (const [day, list] of byDay) {
      const f = this.files().find((x) => x.day === day);
      const have = f ? this._read(f) : [];
      const seen = new Set(have.map((r) => r.t));
      const fresh = list.filter((r) => !seen.has(r.t));
      if (!fresh.length) continue;
      for (const r of fresh) seen.add(r.t);
      const all = [...have, ...fresh].sort((a, b) => a.t - b.t);
      const text = serialize(all);
      if (day < today) {
        atomicWrite(join(this.dir, `${day}.jsonl.gz`), gzipSync(text, { level: 9 }));
        try { rmSync(join(this.dir, `${day}.jsonl`)); } catch { /* absent */ }
      } else {
        atomicWrite(join(this.dir, `${day}.jsonl`), text);
        if (day === this.curDay) this.curKey = all.length ? stKey(all.at(-1).st) : '';
      }
      this.cache.delete(day);
      this._files = null;
      added += fresh.length;
    }
    if (recs.length) this.lastT = Math.max(this.lastT, ...recs.map((r) => r.t));
    return added;
  }

  // ── lecture ──
  range() {
    const fs = this.files();
    let slots = 0, bytes = 0;
    for (const f of fs) bytes += f.bytes;
    return { days: fs.length, from: fs[0]?.day || null, to: fs.at(-1)?.day || null, bytes, lastT: this.lastT, slots };
  }
  list() {
    return this.files().map((f) => ({ day: f.day, bytes: f.bytes, gz: f.gz }));
  }
  // Tranches de [from, to] (jours AAAA-MM-JJ inclus)
  records(from, to) {
    const out = [];
    for (const f of this.files()) if (f.day >= from && f.day <= to) for (const r of this.readDay(f.day)) out.push(r);
    return out;
  }
  // Une tranche précise (pour noter une prévision) ; null si absente
  at(t) {
    const recs = this.readDay(dayOf(t));
    let lo = 0, hi = recs.length - 1;
    while (lo <= hi) {
      const m = (lo + hi) >> 1;
      if (recs[m].t === t) return recs[m];
      if (recs[m].t < t) lo = m + 1; else hi = m - 1;
    }
    return null;
  }

  // Série agrégée par pas de `step` minutes : réseau, ou une station (sid) — public/vn-series.js
  series({ from, to, step = 60, sid = null }) {
    return aggregate(this.records(from, to), { from, to, step, sid });
  }
  // Un résumé par jour, tiré de l'historique seul (au-delà des 60 jours des résumés de la carte)
  dailySummary(from, to) {
    return dailySummary(this.files().filter((f) => f.day >= from && f.day <= to).map((f) => [f.day, this.readDay(f.day)]));
  }

  // Export CSV d'une période : une ligne par tranche (réseau), ou par tranche et station (stations=1)
  csv(from, to, perStation = false) {
    const recs = this.records(from, to);
    const iso = (t) => new Date(t * 1000).toISOString();
    const L = [];
    if (!perStation) {
      L.push('t_utc,heure_locale,dispo,bornes,hs,bruts,retraits,retours,vides,pleines,temp_c,pluie_mm,proba_pluie,vent_kmh,code_wmo');
      for (const r of recs) {
        const p = luxParts(r.t);
        const n = r.n || Array(8).fill(''), w = r.w || Array(5).fill('');
        L.push([iso(r.t), `${p.y}-${p.mo}-${p.d} ${String(p.h).padStart(2, '0')}:${String(p.mi).padStart(2, '0')}`, ...n, ...w.map((x) => (x === null ? '' : x))].join(','));
      }
    } else {
      L.push('t_utc,sid,velos,capacite');
      for (const r of recs) r.b.forEach((v, i) => { if (v !== null) L.push(`${iso(r.t)},${r.st[i][0]},${v},${r.st[i][2]}`); });
    }
    return L.join('\n') + '\n';
  }
}

// ── Import d'une archive d'instantanés (archive/AAAAMMJJ-HHMMSS/, ou vidages de cartes : même disposition)
// Chaque instantané contient l'anneau 24 h de son heure : relu par Model::load dans un moteur à part
// (à l'heure de l'instantané, pour que la règle des 20 h l'accepte), puis versé dans l'historique.
export async function importArchive({ dirs, history, makeEngine, log = () => {} }) {
  let snaps = 0, added = 0, refused = 0;
  for (const dir of dirs) {
    let man = null;
    try { man = JSON.parse(readFileSync(join(dir, 'manifeste.json'), 'utf8')); } catch { /* vidage sans manifeste */ }
    const hist = join(dir, 'vn_hist.bin');
    if (!existsSync(hist)) continue;
    const e = await makeEngine();
    // instant de l'instantané : manifeste, sinon savedAt de l'en-tête de /vn_hist.bin (même place que le profil)
    const hb = readFileSync(hist);
    const t = man?.t_carte || (hb.length >= 40 ? Number(hb.readBigInt64LE(32)) : 0);
    if (!t) { refused++; continue; }
    e.clock(t, 1000);
    e.init();
    for (const f of ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin']) if (existsSync(join(dir, f))) e.putFile('/' + f, readFileSync(join(dir, f)));
    e.load(false);
    // le profil ne garde que les identifiants : noms, capacités et positions viennent de vn_stations.csv
    const csv = readStationsCsv(join(dir, 'vn_stations.csv'));
    const st = e.stations().map((s) => { const c = csv.get(s.sid); return c ? [s.sid, c.name, c.cap, c.lat, c.lon] : [s.sid, s.name, s.cap, s.lat, s.lon]; });
    const recs = [];
    // tranche 0 comprise si l'instantané est ancien de plus de 5 min (sinon elle était en cours)
    const head = e.ringHead();
    for (let back = t - head >= SLOT_S ? 0 : 1; back < 400; back++) {
      const s = e.slot(back);
      if (!s) break;
      if (s.n === null && !s.b.some((v) => v !== null)) continue;
      s.st = st;
      recs.push(s);
    }
    if (!recs.length) { refused++; continue; }
    recs.sort((a, b) => a.t - b.t);
    // l'anneau peut contenir des stations disparues depuis : les colonnes suivent la liste du moment
    added += history.merge(recs);
    snaps++;
  }
  log(`[historique] import : ${snaps} instantané(s) lus, ${refused} sans anneau exploitable, ${added} tranche(s) ajoutée(s)`);
  return { snaps, added, refused };
}

// vn_stations.csv (Model::stationsCsv) : « # vn_stations v1 t=… », puis sid;capacite;publiee;lat;lon;nom
export function readStationsCsv(path) {
  const m = new Map();
  let text;
  try { text = readFileSync(path, 'utf8'); } catch { return m; }
  for (const line of text.split('\n')) {
    if (!line || line.startsWith('#') || line.startsWith('sid;')) continue;
    const f = line.split(';');
    if (f.length < 6) continue;
    m.set(f[0], { cap: +f[1] || 0, lat: +(+f[3]).toFixed(5), lon: +(+f[4]).toFixed(5), name: f.slice(5).join(';') });
  }
  return m;
}

// Dossiers d'instantanés sous un dossier (archive du serveur ou dossier de vidages), du plus ancien au plus récent
export function snapshotDirs(root) {
  const out = [];
  const walk = (d, depth) => {
    let names = [];
    try { names = readdirSync(d); } catch { return; }
    if (names.includes('vn_hist.bin')) { out.push(d); return; }
    if (depth > 3) return;
    for (const n of names.sort()) {
      if (n.startsWith('.')) continue;
      const p = join(d, n);
      try { if (statSync(p).isDirectory()) walk(p, depth + 1); } catch { /* disparu */ }
    }
  };
  walk(root, 0);
  return out.sort();
}
