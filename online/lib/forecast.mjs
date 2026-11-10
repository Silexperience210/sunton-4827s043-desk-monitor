// ── Erreur des prévisions, mesurée en continu ────────────────────────
// Toutes les 15 min de 6 h à 22 h (heure de Luxembourg), au premier relevé de la tranche, le serveur note
// ce que la carte afficherait : prévision du modèle à +15, +30, +60, +120 et +240 min, persistance (le
// relevé lui-même) et profil seul — exactement les trois colonnes de sim/bench_reel.cpp, avec la même
// règle (prévision arrondie − valeur de l'anneau à t + h). Quand l'historique contient la tranche visée,
// l'erreur est connue : chaque nuit, la journée de la veille est notée et gardée.
//
//   <données>/prevision/AAAA-MM-JJ.jsonl     {"sid":[…]} puis une ligne par quart d'heure :
//                                            {"slot":…, "t":…, "net60":…, "bikes":…, "st":[[relevé, m15…m240, p15…p240, échéance vide, échéance pleine, capacité, en service] | null, …]}
//                                            (en service : 1 si la station loue et rend ; absent des journaux d'avant le 02/11/2026)
//                                            "cl":[[c60, c120, c240] | null, …] : témoin « couple de la classe » (la même prévision sans les
//                                            couples par station) — absent des journaux d'avant la 1.4.1
//                                            "er":[centièmes | null, …] : erreur annoncée à +60 min par la fiche de la station (1.5.0)
//                                            "nd":[[besoin livré, besoin témoin] | null, …] : vélos à déplacer, règle « 50 % » et « demande à venir » (1.5.0)
//   <données>/prevision/note/AAAA-MM-JJ.json la note du jour (voir score())
import { mkdirSync, readdirSync, readFileSync, existsSync, statSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { gzipSync, gunzipSync } from 'node:zlib';
import { atomicWrite, appendLines, luxParts } from './store.mjs';
import { dayOf, SLOT_S } from './history.mjs';
import { FEED } from '../public/vn-engine.js';

export const HK = [3, 6, 12, 24, 48];                     // horizons en tranches de 5 min
export const HL = ['+15 min', '+30 min', '+60 min', '+120 min', '+240 min'];
const H60 = 2;
const NOTE_VERSION = 6;                                    // 6 : prévisions traversées par une intervention déclarée écartées                                    // 3 : stations figées et hors service écartées ; 4 : témoin (shadow) ;
                                                           // 5 : erreur annoncée, risque de rupture, besoin témoin
const FROZEN_S = 24 * 3600;

class Acc {
  constructor() { this.s = 0; this.q = 0; this.n = 0; }
  add(e) { this.s += Math.abs(e); this.q += e * e; this.n++; }
  merge(o) { this.s += o.s; this.q += o.q; this.n += o.n; }
  json() { return { n: this.n, mae: this.n ? +(this.s / this.n).toFixed(4) : null, rmse: this.n ? +Math.sqrt(this.q / this.n).toFixed(4) : null, s: +this.s.toFixed(2), q: +this.q.toFixed(2) }; }
  static from(o) { const a = new Acc(); if (o) { a.s = o.s; a.q = o.q; a.n = o.n; } return a; }
}

// ── Témoin : couples par station contre couple de la classe, hors échantillon ──
// Écart apparié de MAE (modèle livré − témoin, négatif = les couples par station se trompent moins), sur les
// journées COMPLÈTES qui portent le témoin. Une journée = une grappe : les quarts d'heure d'un même jour ne sont
// pas indépendants (une prévision à +4 h recouvre les 16 suivantes), les jours le sont bien davantage. Estimateur
// par quotient R = Σ s_j / Σ n_j (s_j = somme des écarts du jour j, n_j = ses paires), variance linéarisée
// Σ (s_j − R·n_j)² · J / (J − 1) / (Σ n_j)², intervalle à 95 % de Student à J − 1 degrés de liberté.
// Aucun verdict avant SHADOW_MIN_DAYS journées (une semaine : les deux classes de jour y sont) : « en cours ».
export const SHADOW_MIN_DAYS = 7;
const T975 = [0, 12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179, 2.160, 2.145, 2.131,
  2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042];
const t975 = (df) => (df < 1 ? Infinity : df <= 30 ? T975[df] : 1.96 + 2.4 / df);
export function shadowVerdict(notes, today = '') {
  const days = notes.filter((n) => n.complete && n.day !== today && n.shadow?.model?.[H60]?.n > 0);
  const out = { days: days.length, minDays: SHADOW_MIN_DAYS, from: days[0]?.day || null, to: days.at(-1)?.day || null, horizons: [] };
  for (let h = H60; h < HK.length; h++) {
    let S = 0, N = 0, sm = 0, sc = 0;
    const per = [];
    for (const n of days) {
      const m = n.shadow.model[h], c = n.shadow.cls[h];
      if (!m?.n || m.n !== c?.n) continue;
      per.push([m.s - c.s, m.n]);
      S += m.s - c.s; N += m.n; sm += m.s; sc += c.s;
    }
    const J = per.length;
    const row = { h: HL[h], n: N, days: J, model: N ? +(sm / N).toFixed(4) : null, cls: N ? +(sc / N).toFixed(4) : null,
      diff: null, lo: null, hi: null, pct: null, verdict: 'en cours' };
    if (N) {
      const R = S / N;
      row.diff = +R.toFixed(4);
      row.pct = sc ? +(100 * S / sc).toFixed(1) : null;
      if (J >= 2) {
        let q = 0;
        for (const [s, n] of per) q += (s - R * n) ** 2;
        const half = t975(J - 1) * Math.sqrt(q * J / (J - 1)) / N;
        row.lo = +(R - half).toFixed(4); row.hi = +(R + half).toFixed(4);
        if (J >= SHADOW_MIN_DAYS) row.verdict = row.hi < 0 ? 'gagne' : row.lo > 0 ? 'perd' : 'non concluant';
      }
    }
    out.horizons.push(row);
  }
  return out;
}

// ── Quotient par grappes : R = Σ y_j / Σ x_j, une journée = une grappe ──
// Variance linéarisée Σ (y_j − R·x_j)² · J / (J − 1) / (Σ x_j)², intervalle à 95 % de Student (J − 1 degrés de liberté).
function ratioCI(pairs) {
  const J = pairs.length;
  let Y = 0, X = 0;
  for (const [y, x] of pairs) { Y += y; X += x; }
  if (!J || !X) return { R: null, lo: null, hi: null, J, X, Y };
  const R = Y / X;
  if (J < 2) return { R, lo: null, hi: null, J, X, Y };
  let q = 0;
  for (const [y, x] of pairs) q += (y - R * x) ** 2;
  const half = t975(J - 1) * Math.sqrt(q * J / (J - 1)) / Math.abs(X);
  return { R, lo: R - half, hi: R + half, J, X, Y };
}
const done = (notes, today) => notes.filter((n) => n.complete && n.day !== today);
const r4 = (v) => (v === null || v === undefined ? null : +v.toFixed(4));

// ── Erreur annoncée par la fiche contre erreur constatée (+60 min) ──
// La fiche d'une station dit « Erreur mesurée : ± x vélo à +1 h » (Model::fcErrAt). Ici : la somme des erreurs
// réellement faites ensuite, divisée par la somme des erreurs annoncées, sur les mêmes prévisions. 1 = l'annonce
// dit vrai. « fidèle » si tout l'intervalle tient entre 0,85 et 1,15 ; « trop optimiste » s'il est au-dessus de
// 1,15 (on se trompe plus qu'annoncé) ; « trop prudente » s'il est sous 0,85 ; verdict après 7 journées.
export const ERR_TOL = 0.15;
export function errCheckVerdict(notes, today = '') {
  const days = done(notes, today).filter((n) => n.errCheck?.n > 0 && n.errCheck.ann > 0);
  const c = ratioCI(days.map((n) => [n.errCheck.real, n.errCheck.ann]));
  const n = days.reduce((a, d) => a + d.errCheck.n, 0);
  let verdict = 'en cours';
  if (c.J >= SHADOW_MIN_DAYS && c.lo !== null) {
    verdict = c.lo >= 1 - ERR_TOL && c.hi <= 1 + ERR_TOL ? 'fidèle' : c.lo > 1 + ERR_TOL ? 'trop optimiste' : c.hi < 1 - ERR_TOL ? 'trop prudente' : 'non concluant';
  }
  return { days: c.J, minDays: SHADOW_MIN_DAYS, n, announced: n ? r4(c.X / n) : null, observed: n ? r4(c.Y / n) : null,
    ratio: r4(c.R), lo: r4(c.lo), hi: r4(c.hi), tol: ERR_TOL, verdict };
}

// ── Risque de rupture dans l'heure, en probabilité ──
// Pas de formule : une fréquence. Pour chaque prévision passée, le journal garde le relevé, la prévision à +60 min
// et l'alerte ; l'historique dit si la station a réellement été vide (ou pleine) dans l'heure. La probabilité
// annoncée pour une case (relevé, prévision, alerte) est la part des prévisions PASSÉES de cette case qui ont fini
// en rupture, rétrécie vers celle du même relevé puis vers celle du parc (RISK_K prévisions fictives par niveau).
// Vérification, comme la feuille de route l'exige (« les 70 % annoncés se réalisent 70 fois sur 100, sinon on ne
// l'affiche pas ») : chaque journée est annoncée avec les seules journées qui la précèdent (au moins
// RISK_TRAIN_DAYS), puis comparée à ce qui s'est passé, par tranche de probabilité. Une tranche est « vérifiée »
// quand tout l'intervalle à 95 % de (constaté − annoncé) tient dans ± RISK_TOL ; « fausse » quand il est tout
// entier dehors. L'ensemble est « vérifié » après 7 journées annoncées, si toutes les tranches qui ont au moins
// RISK_MIN_CASES cas le sont, et qu'il y en a au moins trois. Sinon rien n'est affiché en direct.
export const RISK_K = 20, RISK_TRAIN_DAYS = 3, RISK_TOL = 0.10, RISK_MIN_CASES = 100;
export const RISK_BINS = [0, 0.05, 0.15, 0.30, 0.50, 0.70, 1.000001];
const RISK_C = 13;                                         // niveaux 0…12 (12 = « 12 ou plus »)
export function riskKey(level, fc, alert) {
  const c = Math.max(0, Math.min(RISK_C - 1, Math.round(level))), m = Math.max(0, Math.min(RISK_C - 1, Math.round(fc)));
  return (c * RISK_C + m) * 2 + (alert ? 1 : 0);
}
class RiskTable {
  constructor() { this.cell = new Map(); this.byLevel = new Map(); this.byAlert = [[0, 0], [0, 0]]; this.n = 0; this.h = 0; }
  add(cells) {
    for (const [k, [n, h]] of Object.entries(cells || {})) {
      const key = +k, lvl = Math.floor(key / 2 / RISK_C), al = key & 1;
      const c = this.cell.get(key) || [0, 0]; c[0] += n; c[1] += h; this.cell.set(key, c);
      const l = this.byLevel.get(lvl) || [0, 0]; l[0] += n; l[1] += h; this.byLevel.set(lvl, l);
      this.byAlert[al][0] += n; this.byAlert[al][1] += h;
      this.n += n; this.h += h;
    }
  }
  base() { return (this.h + 1) / (this.n + 2); }           // part des prévisions suivies d'une rupture (lissée)
  p(key) {
    const lvl = Math.floor(key / 2 / RISK_C);
    const l = this.byLevel.get(lvl) || [0, 0], c = this.cell.get(key) || [0, 0];
    const pl = (l[1] + RISK_K * this.base()) / (l[0] + RISK_K);
    return (c[1] + RISK_K * pl) / (c[0] + RISK_K);
  }
  pAlert(al) { const a = this.byAlert[al]; return (a[1] + RISK_K * this.base()) / (a[0] + RISK_K); }
}
function riskSide(days, side) {
  const T = new RiskTable();
  const bins = RISK_BINS.slice(0, -1).map((from, b) => ({ from, to: Math.min(1, RISK_BINS[b + 1]), per: [] }));
  let trained = 0, judged = 0, n = 0, bM = 0, bC = 0, bA = 0;
  for (const d of days) {
    const cells = d.risk?.[side];
    if (!cells) continue;
    if (trained >= RISK_TRAIN_DAYS) {
      judged++;
      const day = bins.map(() => [0, 0, 0]);                 // cas, Σ probabilité annoncée, ruptures vues
      const base = T.base();
      for (const [k, [cn, ch]] of Object.entries(cells)) {
        const key = +k, pr = T.p(key), pa = T.pAlert(key & 1);
        let b = 0;
        while (b < bins.length - 1 && pr >= RISK_BINS[b + 1]) b++;
        day[b][0] += cn; day[b][1] += pr * cn; day[b][2] += ch;
        n += cn;
        bM += ch * (1 - pr) ** 2 + (cn - ch) * pr ** 2;      // score de Brier : modèle, part du parc, alerte seule
        bC += ch * (1 - base) ** 2 + (cn - ch) * base ** 2;
        bA += ch * (1 - pa) ** 2 + (cn - ch) * pa ** 2;
      }
      day.forEach((x, b) => { if (x[0]) bins[b].per.push(x); });
    }
    T.add(cells); trained++;
  }
  let checked = 0, wrong = 0;
  const out = bins.map((bn) => {
    const cases = bn.per.reduce((a, x) => a + x[0], 0);
    const c = ratioCI(bn.per.map(([cn, sp, ch]) => [ch - sp, cn]));    // constaté − annoncé, par cas
    const ann = cases ? bn.per.reduce((a, x) => a + x[1], 0) / cases : null, obs = cases ? bn.per.reduce((a, x) => a + x[2], 0) / cases : null;
    let verdict = 'trop peu de cas';
    if (cases >= RISK_MIN_CASES && c.lo !== null) {
      verdict = c.lo >= -RISK_TOL && c.hi <= RISK_TOL ? 'vérifiée' : (c.lo > RISK_TOL || c.hi < -RISK_TOL) ? 'fausse' : 'en cours';
      if (verdict === 'vérifiée') checked++;
      if (verdict === 'fausse') wrong++;
    }
    return { from: bn.from, to: bn.to, n: cases, days: bn.per.length, announced: r4(ann), observed: r4(obs), lo: r4(c.lo), hi: r4(c.hi), verdict };
  });
  const enough = out.filter((b) => b.n >= RISK_MIN_CASES);
  let verdict = 'en cours';
  if (wrong) verdict = 'fausse';
  else if (judged >= SHADOW_MIN_DAYS && enough.length >= 3 && checked === enough.length) verdict = 'vérifiée';
  return { verdict, judgedDays: judged, trainDays: trained, n, base: trained ? r4(T.base()) : null, bins: out,
    brier: n ? r4(bM / n) : null, brierBase: n ? r4(bC / n) : null, brierAlert: n ? r4(bA / n) : null,
    skillVsBase: n && bC ? r4(1 - bM / bC) : null, skillVsAlert: n && bA ? r4(1 - bM / bA) : null, table: T };
}
export function riskEval(notes, today = '') {
  const days = done(notes, today).filter((n) => n.risk);
  const e = riskSide(days, 'e'), f = riskSide(days, 'f');
  const strip = ({ table, ...x }) => x;
  const out = { minDays: SHADOW_MIN_DAYS, trainDays: RISK_TRAIN_DAYS, tol: RISK_TOL, minCases: RISK_MIN_CASES, empty: strip(e), full: strip(f) };
  Object.defineProperty(out, 'tables', { value: { e: e.table, f: f.table }, enumerable: false });   // pour riskNow, pas pour le JSON
  return out;
}
// Risque en direct, seulement pour un côté (vide, plein) dont la vérification est acquise : [{ i, p }] trié.
// rows = engine.forecast().st ([relevé, m15…m240, p15…p240, échéance vide, échéance pleine, capacité] | null).
export function riskNow(ev, rows, max = 8) {
  const out = { empty: null, full: null };
  const one = (side, table, mk) => {
    if (ev[side].verdict !== 'vérifiée') return null;
    const l = [];
    rows.forEach((v, i) => { if (v) { const k = mk(v); if (k !== null) l.push({ i, p: +table.p(k).toFixed(3) }); } });
    return l.sort((a, b) => b.p - a.p).slice(0, max);
  };
  out.empty = one('empty', ev.tables.e, (v) => (v[0] > 0 ? riskKey(v[0], v[3], v[11] > 0 && v[11] <= 60) : null));
  out.full = one('full', ev.tables.f, (v) => (v[13] > 0 && v[0] < v[13] ? riskKey(v[13] - v[0], v[13] - v[3], v[12] > 0 && v[12] <= 60) : null));
  return out;
}

// ── Besoin conseillé : « viser 50 % à +1 h » contre « demande à venir » ──
// Une ESTIMATION, pas une mesure : on ne voit jamais ce qui se serait passé si le camion était venu. La seule chose
// rejouable est la suite réellement relevée (4 h) décalée du déplacement conseillé, flux supposés inchangés — ce
// qui flatte les deux règles (une station vide cache la demande qu'elle n'a pas servie). Elles sont donc comparées
// entre elles, sur les mêmes cas : tranches de 5 min vides ou pleines pour 100 tranches suivies, et vélos déplacés.
export function targetVerdict(notes, today = '') {
  const days = done(notes, today).filter((n) => n.target?.n > 0);
  const tot = { n: 0, slots: 0, bad0: 0, bad50: 0, badP: 0, mv50: 0, mvP: 0 };
  for (const d of days) for (const k of Object.keys(tot)) tot[k] += d.target[k];
  const c = ratioCI(days.map((d) => [d.target.badP - d.target.bad50, d.target.slots]));
  const pct = (v) => (tot.slots ? +(100 * v / tot.slots).toFixed(2) : null);
  let verdict = 'en cours';
  if (c.J >= SHADOW_MIN_DAYS && c.lo !== null) verdict = c.hi < 0 ? 'moins de ruptures' : c.lo > 0 ? 'plus de ruptures' : 'non concluant';
  return { days: c.J, minDays: SHADOW_MIN_DAYS, cases: tot.n, slots: tot.slots, none: pct(tot.bad0), half: pct(tot.bad50), ahead: pct(tot.badP),
    movedHalf: tot.n ? +(tot.mv50 / tot.n).toFixed(2) : null, movedAhead: tot.n ? +(tot.mvP / tot.n).toFixed(2) : null,
    diff: c.R === null ? null : +(100 * c.R).toFixed(2), lo: c.lo === null ? null : +(100 * c.lo).toFixed(2), hi: c.hi === null ? null : +(100 * c.hi).toFixed(2), verdict };
}

// ── Par type de jour : le modèle se trompe-t-il plus pendant les vacances scolaires ou un événement ? ──
// Un diagnostic avant de construire un type de jour de plus (feuille de route, chantier 6) : MAE à +60 min du
// modèle et de la persistance, par type (lib/calendrier.mjs). Écart apparié modèle − persistance par prévision,
// intervalle par journées ; « gagne » / « perd » / « non concluant » dès 3 journées du type, « en cours » avant.
export const DAYTYPE_MIN_DAYS = 3;
export function dayTypeReport(notes, typeOf) {
  if (!typeOf) return null;
  const by = new Map();
  for (const n of notes.filter((x) => x.complete && x.model?.[H60]?.n > 0)) {
    const t = typeOf(n.day);
    if (!t) continue;
    if (!by.has(t.type)) by.set(t.type, { type: t.type, label: t.label, names: new Set(), days: [] });
    const g = by.get(t.type);
    if (t.name) g.names.add(t.name);
    g.days.push(n);
  }
  return [...by.values()].map((g) => {
    const m = g.days.reduce((a, n) => a + n.model[H60].s, 0), p = g.days.reduce((a, n) => a + n.per[H60].s, 0), cnt = g.days.reduce((a, n) => a + n.model[H60].n, 0);
    const c = ratioCI(g.days.map((n) => [n.model[H60].s - n.per[H60].s, n.model[H60].n]));
    let verdict = 'en cours';
    if (c.J >= DAYTYPE_MIN_DAYS && c.lo !== null) verdict = c.hi < 0 ? 'gagne' : c.lo > 0 ? 'perd' : 'non concluant';
    return { type: g.type, label: g.label, names: [...g.names], days: c.J, n: cnt, model: r4(m / cnt), per: r4(p / cnt),
      gainPct: p ? +(100 * (1 - m / p)).toFixed(1) : null, diff: r4(c.R), lo: r4(c.lo), hi: r4(c.hi), verdict };
  });
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
    this.interventions = null;                              // (t0, t1) → [{ sid, at }] : interventions déclarées, posé par le serveur
    this.dayType = null;                                    // (jour 'AAAA-MM-JJ') → type de jour, posé par le serveur (lib/calendrier.mjs)
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
    const stations = engine.stations();
    const sid = stations.map((s) => s.sid);
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
    // « en service » au moment de la prévision (même ordre de stations) : la notation écarte les autres
    const st = f.st.map((v, i) => (v ? [...v, stations[i]?.renting && stations[i]?.returning ? 1 : 0] : v));
    s += JSON.stringify({ slot: head, t: f.t, net60: f.net60, bikes: f.bikes, st, ...(f.cl ? { cl: f.cl } : {}), ...(f.er ? { er: f.er } : {}), ...(f.nd ? { nd: f.nd } : {}) }) + '\n';
    appendLines(p, s);
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
  // Écartées (skipped) : une station qui ne loue ou ne rend plus, ou dont la valeur n'a pas bougé depuis 24 h
  // ou plus (capteur figé, station à l'arrêt) — toute prévision y est « juste » sans rien prévoir, elle flatte
  // les deux MAE (2,7 % des paires à +60 min en production, revue du 01/11/2026, m7).
  score(day) {
    const preds = this.readDay(day);
    const H = HK.length;
    const model = HK.map(() => new Acc()), per = HK.map(() => new Acc()), prof = HK.map(() => new Acc());
    const byHour = {}, bySt = {};
    const al = { emptyPred: 0, emptyHit: 0, emptyReal: 0, emptyCaught: 0, fullPred: 0, fullHit: 0, fullReal: 0, fullCaught: 0 };
    const skipped = { frozen: 0, notRenting: 0, intervention: 0 };
    // Interventions déclarées (registre de régulation) : { sid → heures triées }. Une prévision dont la fenêtre
    // [tranche d'émission, fin de la tranche visée] en contient une n'est pas notée — même règle que l'apprentissage
    // (Model::fcLearnSlot) : son erreur est celle du camion, pas celle de la prévision. Comptées (skipped.intervention).
    const iv = new Map();
    if (this.interventions && preds.length) {
      for (const e of this.interventions(preds[0].slot, preds.at(-1).slot + (HK.at(-1) + 1) * SLOT_S)) { if (!iv.has(e.sid)) iv.set(e.sid, []); iv.get(e.sid).push(e.at); }
    }
    const crossed = (sid, t0, t1) => { const l = iv.get(sid); return !!l && l.some((t) => t >= t0 && t < t1); };
    // Témoin : sur les seules lignes qui le portent, modèle livré (couples par station) et couple de la classe,
    // notés sur exactement les mêmes paires (prévision, relevé)
    const shModel = HK.map(() => new Acc()), shCls = HK.map(() => new Acc());
    // Erreur annoncée (fiche de la station) contre erreur constatée, sur les mêmes prévisions à +60 min
    const ec = { ann: 0, real: 0, n: 0 };
    // Risque de rupture dans l'heure : par case (relevé, prévision à +60 min, alerte), prévisions et ruptures vues
    const rk = { e: {}, f: {} };
    const rkAdd = (t, key, hit) => { const c = (t[key] ||= [0, 0]); c[0]++; if (hit) c[1]++; };
    // Besoin conseillé : règle livrée et témoin, rejoués sur les 4 h réellement relevées ensuite
    const tg = { n: 0, slots: 0, bad0: 0, bad50: 0, badP: 0, mv50: 0, mvP: 0 };
    const frozen = this.frozenAt(day, preds);
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
      const still = frozen.get(p.slot);
      p.st.forEach((v, i) => {
        if (!v) return;
        const sid = p.sid[i];
        if (v.length > 14 && !v[14]) { skipped.notRenting++; return; }
        if (still?.has(sid)) { skipped.frozen++; return; }
        const cur = v[0];
        for (let h = 0; h < H; h++) {
          const r = fut[HK[h]];
          if (!r) { if (h === H60) missing++; continue; }
          const c = col(r, sid);
          const tv = c === undefined ? null : r.b[c];
          if (tv === null || tv === undefined) continue;
          if (crossed(sid, p.slot, p.slot + (HK[h] + 1) * SLOT_S)) { skipped.intervention++; continue; }
          model[h].add(v[1 + h] - tv);
          per[h].add(cur - tv);
          prof[h].add(v[6 + h] - tv);
          if (h >= H60 && p.cl?.[i]) { shModel[h].add(v[1 + h] - tv); shCls[h].add(p.cl[i][h - H60] - tv); }
          if (h === H60 && p.er?.[i] !== null && p.er?.[i] !== undefined) { ec.ann += p.er[i] / 100; ec.real += Math.abs(v[1 + h] - tv); ec.n++; }
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
        if (crossed(sid, p.slot, p.slot + 13 * SLOT_S)) return;   // camion déclaré dans l'heure : ni alerte, ni risque, ni besoin jugés
        const predE = etaE > 0 && etaE <= 60, predF = etaF > 0 && etaF <= 60;
        if (cur > 0) {
          if (predE) { al.emptyPred++; if (sawE) al.emptyHit++; }
          if (sawE) { al.emptyReal++; if (predE) al.emptyCaught++; }
        }
        if (cap > 0 && cur < cap) {
          if (predF) { al.fullPred++; if (sawF) al.fullHit++; }
          if (sawF) { al.fullReal++; if (predF) al.fullCaught++; }
        }
        // mêmes cas que les alertes : station ni vide ni pleine au moment de la prévision, heure suivante relevée
        if (cur > 0) rkAdd(rk.e, riskKey(cur, v[1 + H60], predE), sawE);
        if (cap > 0 && cur < cap) rkAdd(rk.f, riskKey(cap - cur, cap - v[1 + H60], predF), sawF);
        // besoin conseillé : la trajectoire réellement relevée sur 4 h, décalée du déplacement (borné par ce que la
        // station peut donner ou recevoir à l'instant) ; tranche « en rupture » = vide ou pleine
        const nd = p.nd?.[i];
        if (nd && nd[0] !== 0 && cap > 0 && !crossed(sid, p.slot, p.slot + 49 * SLOT_S)) {
          const d50 = Math.max(-cur, Math.min(cap - cur, nd[0])), dP = Math.max(-cur, Math.min(cap - cur, nd[1]));
          let seen = 0, b0 = 0, b50 = 0, bP = 0;
          const bad = (y) => y <= 0 || y >= cap;
          for (let k = 1; k <= 48; k++) {
            const r = fut[k];
            if (!r) continue;
            const c = col(r, sid);
            const x = c === undefined ? null : r.b[c];
            if (x === null || x === undefined) continue;
            seen++;
            if (bad(x)) b0++;
            if (bad(x + d50)) b50++;
            if (bad(x + dP)) bP++;
          }
          if (seen >= 36) { tg.n++; tg.slots += seen; tg.bad0 += b0; tg.bad50 += b50; tg.badP += bP; tg.mv50 += Math.abs(d50); tg.mvP += Math.abs(dP); }
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
      hours, stations, alerts: al, skipped, scoredAt: now,
      shadow: { model: shModel.map((a) => a.json()), cls: shCls.map((a) => a.json()) },
      errCheck: { ann: +ec.ann.toFixed(2), real: ec.real, n: ec.n }, risk: rk, target: tg,
    };
  }
  // Stations immobiles depuis 24 h ou plus à l'instant de chaque prévision : { slot → Set(sid) }, d'après
  // l'historique de la veille et du jour (premier jour de l'historique : rien n'est assez ancien pour juger)
  frozenAt(day, preds) {
    const out = new Map();
    const slots = [...new Set(preds.map((p) => p.slot))].sort((a, b) => a - b);
    if (!slots.length) return out;
    const recs = this.history.records(dayOf(slots[0] - FROZEN_S - SLOT_S), day);
    const since = new Map();                       // sid → { v, t } : valeur et début de sa tenue
    const snap = (t) => out.set(t, new Set([...since].filter(([, x]) => t - x.t >= FROZEN_S).map(([sid]) => sid)));
    let j = 0;
    for (const r of recs) {
      while (j < slots.length && slots[j] < r.t) snap(slots[j++]);
      r.b.forEach((v, c) => {
        if (v === null || v === undefined) return;
        const sid = r.st[c][0], x = since.get(sid);
        if (!x || x.v !== v) since.set(sid, { v, t: r.t });
      });
    }
    while (j < slots.length) snap(slots[j++]);
    return out;
  }
  // Notes des journées finies pour le témoin : jusqu'à 60 jours (la demi-vie de l'apprentissage est de 7 jours)
  shadowNotes(today, max = 60) {
    return this.files().filter((f) => f.day < today).slice(-max).map((f) => this.readNote(f.day)).filter(Boolean);
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
    const al = {}, skipped = { frozen: 0, notRenting: 0, intervention: 0 };
    for (const n of list) {
      for (const k of Object.keys(skipped)) skipped[k] += n.skipped?.[k] || 0;
      for (let h = 0; h < H; h++) { tot.model[h].merge(Acc.from(n.model[h])); tot.per[h].merge(Acc.from(n.per[h])); tot.prof[h].merge(Acc.from(n.prof[h])); }
      for (const x of n.hours) { (hours[x.h] ||= [new Acc(), new Acc()])[0].merge(Acc.from(x.model)); hours[x.h][1].merge(Acc.from(x.per)); }
      for (const x of n.stations) { (st[x.sid] ||= [new Acc(), new Acc()])[0].merge(Acc.from(x.model)); st[x.sid][1].merge(Acc.from(x.per)); }
      for (const [k, v] of Object.entries(n.alerts)) al[k] = (al[k] || 0) + v;
    }
    const pct = (a, b) => (b ? +(100 * a / b).toFixed(1) : null);
    const sn = this.shadowNotes(today);                      // journées finies (60 au plus) : témoins et vérifications
    const stations = Object.entries(st).map(([sid, a]) => ({ sid, n: a[0].n, model: a[0].json().mae, per: a[1].json().mae }))
      .filter((x) => x.n >= 8).sort((a, b) => b.model - a.model);
    return {
      horizons: HL, days: list.map((n) => ({ day: n.day, preds: n.preds, complete: n.complete,
        model: n.model.map((a) => a.mae), per: n.per.map((a) => a.mae), prof: n.prof.map((a) => a.mae), n60: n.model[H60].n, skipped: n.skipped })),
      total: { model: tot.model.map((a) => a.json()), per: tot.per.map((a) => a.json()), prof: tot.prof.map((a) => a.json()) },
      hours: Object.keys(hours).map(Number).sort((a, b) => a - b).map((h) => ({ h, model: hours[h][0].json().mae, per: hours[h][1].json().mae, n: hours[h][0].n })),
      hardest: stations.slice(0, 8), easiest: stations.slice(-5).reverse(), stationsScored: stations.length, skipped,
      alerts: { ...al, emptyPrecision: pct(al.emptyHit || 0, al.emptyPred || 0), emptyRecall: pct(al.emptyCaught || 0, al.emptyReal || 0),
        fullPrecision: pct(al.fullHit || 0, al.fullPred || 0), fullRecall: pct(al.fullCaught || 0, al.fullReal || 0) },
      lastSlot: this.lastSlot, shadow: shadowVerdict(sn, today),
      errCheck: errCheckVerdict(sn, today), risk: riskEval(sn, today),
      target: targetVerdict(sn, today), dayTypes: dayTypeReport(sn, this.dayType),
    };
  }
}

// ── Rejeu de l'historique dans un moteur neuf, au pas de 5 min ──
// Chaque tranche redevient un station_status GBFS (et station_information toutes les heures) ingéré par le
// moteur à son heure ; le journal note les prévisions comme en direct. Sert à vérifier la mesure en continu
// contre le banc natif (même archive, même règle : test/features.test.mjs) et à l'exemple de l'aperçu.
// Sans météo (le banc, lui, la rejoue) : écart attendu de l'ordre du centième de vélo.
export function replayForecasts({ engine, history, forecastLog, from, to }) {
  const recs = history.records(from, to);
  if (!recs.length) return 0;
  let ms = 1000, logged = 0, infoKey = '';
  engine.clock(recs[0].t, ms);
  for (let k = 0; k < recs.length; k++) {
    const r = recs[k];
    const t = r.t + 20;
    ms += SLOT_S * 1000;
    engine.clock(t, ms);
    engine.setFetching(true);
    const key = r.st.map((s) => `${s[0]}:${s[2]}`).join(',');
    if (key !== infoKey || k % 12 === 0) {
      infoKey = key;
      engine.ingest(FEED.INFO, true, 200, JSON.stringify({ last_updated: new Date(t * 1000).toISOString(), ttl: 300, version: '3.0', data: { stations: r.st.map((s) => ({
        station_id: s[0], name: [{ text: s[1], language: 'fr' }], lat: s[3], lon: s[4], address: '', capacity: s[2] })) } }), 300, 0);
    }
    const iso = new Date(t * 1000).toISOString().replace('.000', '');
    const stations = [];
    r.st.forEach((s, i) => {
      const b = r.b[i];
      if (b === null || b === undefined) return;
      const cap = Math.max(s[2], b);
      stations.push({ station_id: s[0], num_vehicles_available: b, vehicle_types_available: [{ vehicle_type_id: 'electrical', count: b }], num_vehicles_disabled: 0,
        num_docks_available: cap - b, num_docks_disabled: 0, is_installed: true, is_renting: true, is_returning: true, last_reported: iso });
    });
    engine.ingest(FEED.STATUS, true, 200, JSON.stringify({ last_updated: iso, ttl: 60, version: '3.0', data: { stations } }), 300, 900);
    engine.setFetching(false);
    if (forecastLog.capture(engine)) logged++;
  }
  return logged;
}
