// ── Erreur des prévisions, mesurée en continu ────────────────────────
// Toutes les 15 min de 6 h à 22 h (heure de Luxembourg), au premier relevé de la tranche, le serveur note
// ce que la carte afficherait : prévision du modèle à +15, +30, +60, +120 et +240 min, persistance (le
// relevé lui-même) et profil seul — exactement les trois colonnes de sim/bench_reel.cpp, avec la même
// règle (prévision arrondie − valeur de l'anneau à t + h). Quand l'historique contient la tranche visée,
// l'erreur est connue : chaque nuit, la journée de la veille est notée et gardée.
//
//   <données>/prevision/AAAA-MM-JJ.jsonl     {"sid":[…]} puis une ligne par quart d'heure :
//                                            {"slot":…, "t":…, "net60":…, "bikes":…, "st":[[relevé, m15…m240, p15…p240, échéance vide, échéance pleine, capacité] | null, …]}
//   <données>/prevision/note/AAAA-MM-JJ.json la note du jour (voir score())
import { mkdirSync, readdirSync, readFileSync, appendFileSync, existsSync, statSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { gzipSync, gunzipSync } from 'node:zlib';
import { atomicWrite, luxParts } from './store.mjs';
import { dayOf, SLOT_S } from './history.mjs';

export const HK = [3, 6, 12, 24, 48];                     // horizons en tranches de 5 min
export const HL = ['+15 min', '+30 min', '+60 min', '+120 min', '+240 min'];
const H60 = 2;
const NOTE_VERSION = 2;

class Acc {
  constructor() { this.s = 0; this.q = 0; this.n = 0; }
  add(e) { this.s += Math.abs(e); this.q += e * e; this.n++; }
  merge(o) { this.s += o.s; this.q += o.q; this.n += o.n; }
  json() { return { n: this.n, mae: this.n ? +(this.s / this.n).toFixed(4) : null, rmse: this.n ? +Math.sqrt(this.q / this.n).toFixed(4) : null, s: +this.s.toFixed(2), q: +this.q.toFixed(2) }; }
  static from(o) { const a = new Acc(); if (o) { a.s = o.s; a.q = o.q; a.n = o.n; } return a; }
}

export class ForecastLog {
  constructor(dataDir, history, log = () => {}) {
    this.dir = join(dataDir, 'prevision');
    this.noteDir = join(this.dir, 'note');
    mkdirSync(this.noteDir, { recursive: true });
    this.history = history;
    this.log = log;
    this.lastSlot = 0;
    this.curDay = '';
    this.curKey = '';
    this.live = { at: 0, v: null };
    const fs = this.files();
    for (let i = fs.length - 1; i >= 0 && !this.lastSlot; i--) {
      const recs = this.readDay(fs[i].day);
      if (recs.length) this.lastSlot = recs.at(-1).slot;
    }
  }

  files() {
    let names = [];
    try { names = readdirSync(this.dir); } catch { return []; }
    const by = new Map();
    for (const n of names) {
      const m = n.match(/^(\d{4}-\d{2}-\d{2})\.jsonl(\.gz)?$/);
      if (!m) continue;
      if (!by.has(m[1]) || !m[2]) by.set(m[1], { day: m[1], gz: !!m[2], path: join(this.dir, n), bytes: statSync(join(this.dir, n)).size });
    }
    return [...by.values()].sort((a, b) => (a.day < b.day ? -1 : 1));
  }
  readDay(day) {
    const f = this.files().find((x) => x.day === day);
    if (!f) return [];
    let text;
    try { const b = readFileSync(f.path); text = (f.gz ? gunzipSync(b) : b).toString('utf8'); } catch { return []; }
    const out = [];
    let sid = null;
    for (const line of text.split('\n')) {
      if (!line) continue;
      let o;
      try { o = JSON.parse(line); } catch { continue; }
      if (o.sid) { sid = o.sid; continue; }
      if (o.slot && sid) { o.sid = sid; out.push(o); }
    }
    return out;
  }

  // Après chaque cycle : premier relevé d'un quart d'heure de 6 h à 22 h → prévisions notées
  capture(engine) {
    const head = engine.ringHead();
    if (!head || head <= this.lastSlot) return false;
    const lp = luxParts(head);
    if (lp.mi % 15 !== 0 || lp.h < 6 || lp.h >= 22) return false;
    const f = engine.forecast();
    if (f.slot !== head) return false;
    const sid = engine.stations().map((s) => s.sid);
    const day = dayOf(head);
    const p = join(this.dir, `${day}.jsonl`);
    if (day !== this.curDay) {
      this.curDay = day;
      this.curKey = '';
      if (existsSync(p)) { const r = this.readDay(day); if (r.length) this.curKey = r.at(-1).sid.join(','); }
      this.compressDone(day);
    }
    let s = '';
    const k = sid.join(',');
    if (k !== this.curKey) { s += JSON.stringify({ sid }) + '\n'; this.curKey = k; }
    s += JSON.stringify({ slot: head, t: f.t, net60: f.net60, bikes: f.bikes, st: f.st }) + '\n';
    appendFileSync(p, s);
    this.lastSlot = head;
    return true;
  }
  compressDone(today = dayOf(Math.floor(Date.now() / 1000))) {
    for (const f of this.files()) {
      if (f.gz || f.day >= today) continue;
      try { atomicWrite(f.path + '.gz', gzipSync(readFileSync(f.path), { level: 9 })); rmSync(f.path); } catch { /* réessayé plus tard */ }
    }
  }

  // ── Note d'une journée ──
  // Pour chaque prévision dont la tranche visée est dans l'historique : erreur = prévision arrondie −
  // vélos de l'anneau à t + h (|e| moyen = MAE, comme le banc). En plus : MAE à +60 min par heure et par
  // station, et la fiabilité de l'alerte « vide / pleine dans l'heure » (échéance ≤ 60 min) : part
  // confirmée (la station a bien été vide / pleine dans l'heure) et part des ruptures annoncées.
  score(day) {
    const preds = this.readDay(day);
    const H = HK.length;
    const model = HK.map(() => new Acc()), per = HK.map(() => new Acc()), prof = HK.map(() => new Acc());
    const byHour = {}, bySt = {};
    const al = { emptyPred: 0, emptyHit: 0, emptyReal: 0, emptyCaught: 0, fullPred: 0, fullHit: 0, fullReal: 0, fullCaught: 0 };
    let lastNeeded = 0, missing = 0;
    const colCache = new Map();
    const col = (rec, sid) => {
      let m = colCache.get(rec.st);
      if (!m) { m = new Map(rec.st.map((s, i) => [s[0], i])); colCache.set(rec.st, m); }
      return m.get(sid);
    };
    for (const p of preds) {
      const hour = luxParts(p.slot).h;
      lastNeeded = Math.max(lastNeeded, p.slot + HK.at(-1) * SLOT_S);
      // tranches de l'heure suivante (alertes) et des horizons
      const fut = [];
      for (let k = 1; k <= 48; k++) fut[k] = this.history.at(p.slot + k * SLOT_S);
      p.st.forEach((v, i) => {
        if (!v) return;
        const sid = p.sid[i];
        const cur = v[0];
        for (let h = 0; h < H; h++) {
          const r = fut[HK[h]];
          if (!r) { if (h === H60) missing++; continue; }
          const c = col(r, sid);
          const tv = c === undefined ? null : r.b[c];
          if (tv === null || tv === undefined) continue;
          model[h].add(v[1 + h] - tv);
          per[h].add(cur - tv);
          prof[h].add(v[6 + h] - tv);
          if (h === H60) {
            (byHour[hour] ||= [new Acc(), new Acc()])[0].add(v[1 + h] - tv);
            byHour[hour][1].add(cur - tv);
            (bySt[sid] ||= [new Acc(), new Acc()])[0].add(v[1 + h] - tv);
            bySt[sid][1].add(cur - tv);
          }
        }
        // alertes à une heure : vide dans ≤ 60 min annoncé ? et constaté ?
        const etaE = v[11], etaF = v[12], cap = v[13];
        let sawE = false, sawF = false, known = 0;
        for (let k = 1; k <= 12; k++) {
          const r = fut[k];
          if (!r) continue;
          const c = col(r, sid);
          const x = c === undefined ? null : r.b[c];
          if (x === null || x === undefined) continue;
          known++;
          if (x === 0) sawE = true;
          if (cap > 0 && x >= cap) sawF = true;
        }
        if (known < 6) return;                    // heure suivante trop peu relevée pour juger
        const predE = etaE > 0 && etaE <= 60, predF = etaF > 0 && etaF <= 60;
        if (cur > 0) {
          if (predE) { al.emptyPred++; if (sawE) al.emptyHit++; }
          if (sawE) { al.emptyReal++; if (predE) al.emptyCaught++; }
        }
        if (cap > 0 && cur < cap) {
          if (predF) { al.fullPred++; if (sawF) al.fullHit++; }
          if (sawF) { al.fullReal++; if (predF) al.fullCaught++; }
        }
      });
    }
    const now = Math.floor(Date.now() / 1000);
    const hours = Object.keys(byHour).map(Number).sort((a, b) => a - b).map((h) => ({ h, model: byHour[h][0].json(), per: byHour[h][1].json() }));
    const stations = Object.entries(bySt).map(([sid, a]) => ({ sid, model: a[0].json(), per: a[1].json() }));
    return {
      v: NOTE_VERSION, day, preds: preds.length, complete: preds.length > 0 && now > lastNeeded + 600 && this.history.lastT >= lastNeeded,
      missing60: missing, horizons: HL,
      model: model.map((a) => a.json()), per: per.map((a) => a.json()), prof: prof.map((a) => a.json()),
      hours, stations, alerts: al, scoredAt: now,
    };
  }
  noteFile(day) { return join(this.noteDir, `${day}.json`); }
  readNote(day) { try { const n = JSON.parse(readFileSync(this.noteFile(day), 'utf8')); return n.v === NOTE_VERSION ? n : null; } catch { return null; } }
  // Note définitive des journées finies et pas encore notées (tâche de nuit, et au démarrage)
  scorePending() {
    const today = dayOf(Math.floor(Date.now() / 1000));
    let n = 0;
    for (const f of this.files()) {
      if (f.day >= today) continue;
      const cur = this.readNote(f.day);
      if (cur && cur.complete) continue;
      const s = this.score(f.day);
      atomicWrite(this.noteFile(f.day), JSON.stringify(s));
      n++;
      if (s.complete) this.log(`[prévision] ${f.day} noté : MAE +60 modèle ${s.model[H60].mae ?? '—'} · persistance ${s.per[H60].mae ?? '—'} (${s.model[H60].n} prévisions)`);
    }
    return n;
  }

  // Notes des `days` derniers jours (+ aujourd'hui, partiel, recalculé au plus toutes les 2 min),
  // et leur somme : ce que la page affiche
  report(days = 14) {
    const now = Math.floor(Date.now() / 1000);
    const today = dayOf(now);
    const list = [];
    for (const f of this.files().slice(-(days + 1))) {
      let n;
      if (f.day === today) {
        if (!this.live.v || now - this.live.at > 120 || this.live.v.day !== today) this.live = { at: now, v: this.score(today) };
        n = this.live.v;
      } else n = this.readNote(f.day) || this.score(f.day);
      list.push(n);
    }
    const H = HK.length;
    const tot = { model: HK.map(() => new Acc()), per: HK.map(() => new Acc()), prof: HK.map(() => new Acc()) };
    const hours = {}, st = {};
    const al = {};
    for (const n of list) {
      for (let h = 0; h < H; h++) { tot.model[h].merge(Acc.from(n.model[h])); tot.per[h].merge(Acc.from(n.per[h])); tot.prof[h].merge(Acc.from(n.prof[h])); }
      for (const x of n.hours) { (hours[x.h] ||= [new Acc(), new Acc()])[0].merge(Acc.from(x.model)); hours[x.h][1].merge(Acc.from(x.per)); }
      for (const x of n.stations) { (st[x.sid] ||= [new Acc(), new Acc()])[0].merge(Acc.from(x.model)); st[x.sid][1].merge(Acc.from(x.per)); }
      for (const [k, v] of Object.entries(n.alerts)) al[k] = (al[k] || 0) + v;
    }
    const pct = (a, b) => (b ? +(100 * a / b).toFixed(1) : null);
    const stations = Object.entries(st).map(([sid, a]) => ({ sid, n: a[0].n, model: a[0].json().mae, per: a[1].json().mae }))
      .filter((x) => x.n >= 8).sort((a, b) => b.model - a.model);
    return {
      horizons: HL, days: list.map((n) => ({ day: n.day, preds: n.preds, complete: n.complete,
        model: n.model.map((a) => a.mae), per: n.per.map((a) => a.mae), prof: n.prof.map((a) => a.mae), n60: n.model[H60].n })),
      total: { model: tot.model.map((a) => a.json()), per: tot.per.map((a) => a.json()), prof: tot.prof.map((a) => a.json()) },
      hours: Object.keys(hours).map(Number).sort((a, b) => a - b).map((h) => ({ h, model: hours[h][0].json().mae, per: hours[h][1].json().mae, n: hours[h][0].n })),
      hardest: stations.slice(0, 8), easiest: stations.slice(-5).reverse(), stationsScored: stations.length,
      alerts: { ...al, emptyPrecision: pct(al.emptyHit || 0, al.emptyPred || 0), emptyRecall: pct(al.emptyCaught || 0, al.emptyReal || 0),
        fullPrecision: pct(al.fullHit || 0, al.fullPred || 0), fullRecall: pct(al.fullCaught || 0, al.fullReal || 0) },
      lastSlot: this.lastSlot,
    };
  }
}
