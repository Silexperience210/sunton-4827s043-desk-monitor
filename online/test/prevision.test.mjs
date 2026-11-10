// ── Notation des prévisions (lib/forecast.mjs, score) : contrôlée contre un calcul indépendant ─
// Un jour synthétique (Luxembourg, heure d'hiver), 64 quarts d'heure de 6 h à 22 h, quatre stations ; chaque
// horizon a sa propre prévision (modèle : relevé + 1…5, profil : relevé − 1…5), de sorte qu'un index échangé
// change la note. La note de score() est comparée, champ par champ, à un calcul fait ici sans aucun module
// du serveur : MAE et RMSE du modèle, de la persistance et du profil à chaque horizon, MAE à +60 min par heure
// et par station.
//  m7. stations figées : une station dont la valeur n'a pas bougé depuis 24 h ou plus, ou qui ne loue ou ne
//      rend plus (relevé au moment de la prévision), n'est pas notée — elle flatte les deux MAE sans rien
//      prévoir. Le nombre de prévisions écartées est publié.
// node online/test/prevision.test.mjs
import { mkdtempSync, rmSync, mkdirSync, writeFileSync, readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { History } from '../lib/history.mjs';
import { ForecastLog, shadowVerdict, SHADOW_MIN_DAYS, errCheckVerdict, riskEval, riskNow, riskKey, targetVerdict, dayTypeReport,
  RISK_K, RISK_TRAIN_DAYS, RISK_MIN_CASES } from '../lib/forecast.mjs';
import { loadCalendar, dayType, SCHOOL_HOLIDAYS } from '../lib/calendrier.mjs';
import { loadEngine, FEED } from '../public/vn-engine.js';
import { site } from './harnais.mjs';

let fails = 0, checks = 0;
const ok = (c, msg) => { checks++; site(); if (!c) { fails++; console.log(`  ÉCHEC : ${msg}`); } else if (process.env.VN_TEST_DETAIL) console.log(`  ok : ${msg}`); };
const tmp = mkdtempSync(join(tmpdir(), 'vn-prevision-'));
let dirN = 0;
const fresh = () => join(tmp, `d${++dirN}`);

// ── Le jour synthétique ──
const DAY = '2026-10-28';
const T0 = Date.UTC(2026, 9, 26, 23, 0, 0) / 1000;           // 27/10 0 h 00 à Luxembourg (UTC+1)
const N = (2 * 24 + 3) * 12;                                    // jusqu'au 29/10 3 h 00
const ST = [['A', 'Gare', 20, 49.6, 6.13], ['B', 'Figée', 15, 49.61, 6.12], ['C', 'Hors service', 30, 49.62, 6.12], ['E', 'Calme', 25, 49.6, 6.1]];
const VAL = {
  A: (k) => (k * 7) % 11,
  B: () => 0,                                                    // à 0 depuis plus de 30 h : figée
  C: (k) => 2 + (k % 5),                                         // ne loue plus (relevé au moment de la prévision)
  E: (k) => (k >= 20 * 12 && k < 36 * 12 ? 3 : 1 + (k * 3) % 9), // immobile 16 h (27/10 20 h → 28/10 12 h) : notée
};
const HK = [3, 6, 12, 24, 48];
const K0 = 30 * 12;                                              // 28/10 6 h 00
const PRED_K = Array.from({ length: 64 }, (_, j) => K0 + 3 * j);
const renting = { A: 1, B: 1, C: 0, E: 1 };

// Témoin « couple de la classe » : à +60, +120 et +240 min, une prévision propre (relevé + 2, + 6, + 5) — le modèle
// y vaut relevé + 3, + 4, + 5 : meilleur, moins bon, égal selon l'horizon, et un index décalé change la note.
const CL_OFF = [2, 6, 5];
// 1.5.0 : erreur annoncée (centièmes), besoin livré et témoin, échéance « vide dans 30 min » quand il reste ≤ 2 vélos
const ER = { A: 150, B: 80, C: 60, E: null };
// E, immobile à 3 vélos le matin : retirer 2 (livré) ou 7 (témoin) — elle n'en a que 3 à donner, le témoin est borné à −3
const ND = (sid, cur) => (sid === 'A' && cur <= 4 ? [6, 3] : sid === 'E' && cur === 3 ? [-2, -7] : [0, 0]);
function writeDay(D, { withFlag = true, withCl = false, with15 = false } = {}) {
  const h = new History(D);
  h.append(Array.from({ length: N }, (_, k) => ({ t: T0 + k * 300, n: null, w: null, b: ST.map((s) => VAL[s[0]](k)), st: ST })));
  mkdirSync(join(D, 'prevision'), { recursive: true });
  let s = JSON.stringify({ sid: ST.map((x) => x[0]) }) + '\n';
  for (const k of PRED_K) {
    const st = ST.map(([sid, , cap]) => {
      const cur = VAL[sid](k);
      const row = [cur, ...HK.map((_, h) => cur + h + 1), ...HK.map((_, h) => Math.max(0, cur - h - 1)), with15 && cur > 0 && cur <= 2 ? 30 : -1, -1, cap];
      return withFlag ? [...row, renting[sid]] : row;
    });
    const cl = withCl ? { cl: ST.map(([sid]) => CL_OFF.map((o) => VAL[sid](k) + o)) } : {};
    const more = with15 ? { er: ST.map(([sid]) => ER[sid]), nd: ST.map(([sid]) => ND(sid, VAL[sid](k))) } : {};
    s += JSON.stringify({ slot: T0 + k * 300, t: T0 + k * 300 + 20, net60: 0, bikes: 0, st, ...cl, ...more }) + '\n';
  }
  writeFileSync(join(D, 'prevision', `${DAY}.jsonl`), s);
  return new ForecastLog(D, new History(D));
}

// Calcul indépendant : mêmes définitions que le banc (prévision − valeur de l'historique à t + h)
function expected(sids) {
  const acc = () => ({ s: 0, q: 0, n: 0 });
  const add = (a, e) => { a.s += Math.abs(e); a.q += e * e; a.n++; };
  const model = HK.map(acc), per = HK.map(acc), prof = HK.map(acc), byHour = {}, bySt = {};
  for (const k of PRED_K) {
    const hour = Math.floor(((k * 300) % 86400) / 3600);       // T0 = minuit local, pas de changement d'heure ce jour-là
    for (const sid of sids) {
      const cur = VAL[sid](k);
      HK.forEach((hk, h) => {
        const tv = VAL[sid](k + hk);
        add(model[h], cur + h + 1 - tv); add(per[h], cur - tv); add(prof[h], Math.max(0, cur - h - 1) - tv);
        if (hk === 12) {
          add((byHour[hour] ||= [acc(), acc()])[0], cur + h + 1 - tv); add(byHour[hour][1], cur - tv);
          add((bySt[sid] ||= [acc(), acc()])[0], cur + h + 1 - tv); add(bySt[sid][1], cur - tv);
        }
      });
    }
  }
  const mae = (a) => +(a.s / a.n).toFixed(4), rmse = (a) => +Math.sqrt(a.q / a.n).toFixed(4);
  return { model, per, prof, byHour, bySt, mae, rmse };
}
const same = (x, y) => Math.abs(x - y) < 1e-9;
function compare(what, s, x) {
  for (const [name, got, want] of [['modèle', s.model, x.model], ['persistance', s.per, x.per], ['profil', s.prof, x.prof]]) {
    const bad = HK.map((hk, h) => (got[h].n === want[h].n && same(got[h].mae, x.mae(want[h])) && same(got[h].rmse, x.rmse(want[h])) ? null
      : `+${hk * 5} min : n ${got[h].n} / ${want[h].n}, MAE ${got[h].mae} / ${x.mae(want[h])}`)).filter(Boolean);
    ok(!bad.length, `${what} : ${name}, cinq horizons (n, MAE, RMSE) ${bad.length ? `— ${bad.join(' ; ')}` : `: MAE ${got.map((a) => a.mae).join(' · ')}`}`);
  }
  const hours = Object.keys(x.byHour).map(Number);
  const badH = hours.filter((h) => { const g = s.hours.find((y) => y.h === h); return !g || g.model.n !== x.byHour[h][0].n || !same(g.model.mae, x.mae(x.byHour[h][0])) || !same(g.per.mae, x.mae(x.byHour[h][1])); });
  ok(s.hours.length === hours.length && !badH.length, `${what} : MAE +60 min par heure (${hours.length} heures)${badH.length ? ` — fausses : ${badH.join(', ')} h` : ''}`);
  const sids = Object.keys(x.bySt);
  const badS = sids.filter((sid) => { const g = s.stations.find((y) => y.sid === sid); return !g || g.model.n !== x.bySt[sid][0].n || !same(g.model.mae, x.mae(x.bySt[sid][0])) || !same(g.per.mae, x.mae(x.bySt[sid][1])); });
  ok(s.stations.length === sids.length && !badS.length, `${what} : MAE +60 min par station (${s.stations.map((y) => y.sid).join(', ')} ; attendu ${sids.join(', ')})${badS.length ? ` — fausses : ${badS.join(', ')}` : ''}`);
}

try {
  console.log('notation d\'un jour synthétique contre un calcul indépendant ; m7 : stations figées et hors service écartées');
  {
    const s = writeDay(fresh()).score(DAY);
    ok(s.preds === 64, `64 quarts d'heure lus (${s.preds})`);
    compare('journal avec relevé « en service »', s, expected(['A', 'E']));
    ok(s.skipped?.frozen === 64 && s.skipped?.notRenting === 64, `prévisions écartées publiées : figées ${s.skipped?.frozen} (64 attendues), hors service ${s.skipped?.notRenting} (64 attendues)`);
  }
  {
    // journal d'avant le correctif (14 champs, sans « en service ») : seule la règle des 24 h s'applique
    const s = writeDay(fresh(), { withFlag: false }).score(DAY);
    compare('journal sans relevé « en service »', s, expected(['A', 'C', 'E']));
    ok(s.skipped?.frozen === 64 && s.skipped?.notRenting === 0, `ancien journal : figées ${s.skipped?.frozen}, hors service ${s.skipped?.notRenting} (64 et 0 attendues)`);
  }
  console.log('témoin : couples par station contre couple de la classe, notés sur les mêmes paires');
  {
    const s = writeDay(fresh(), { withCl: true }).score(DAY);
    // calcul indépendant, stations notées A et E, horizons +60 / +120 / +240 min
    const want = [2, 3, 4].map((h) => {
      let sm = 0, sc = 0, n = 0;
      for (const k of PRED_K) for (const sid of ['A', 'E']) {
        const cur = VAL[sid](k), tv = VAL[sid](k + HK[h]);
        sm += Math.abs(cur + h + 1 - tv); sc += Math.abs(cur + CL_OFF[h - 2] - tv); n++;
      }
      return { sm, sc, n };
    });
    const bad = [2, 3, 4].filter((h) => { const w = want[h - 2], m = s.shadow?.model?.[h], c = s.shadow?.cls?.[h]; return !m || m.n !== w.n || c.n !== w.n || !same(m.s, w.sm) || !same(c.s, w.sc); });
    ok(!bad.length, `témoin noté à +60, +120, +240 min sur les paires du modèle (n ${s.shadow?.model?.[2]?.n}, attendu ${want[0].n})${bad.length ? ` — faux aux horizons ${bad.join(', ')}` : ''}`);
    ok(s.shadow.model[0].n === 0 && s.shadow.cls[1].n === 0, 'aucun témoin à +15 et +30 min (pas de couple par station à ces horizons)');
    ok(same(s.shadow.model[2].s, s.model[2].s) && s.shadow.model[2].n === s.model[2].n, 'colonne « modèle » du témoin = la note du modèle quand toutes les lignes portent le témoin');
    const s0 = writeDay(fresh()).score(DAY);
    ok(s0.shadow.model[2].n === 0 && s0.shadow.cls[2].n === 0, 'journal sans témoin (avant la 1.4.1) : rien de noté, rien d\'inventé');
  }
  console.log('témoin dans le moteur : il diffère là où une station a son couple, et ne change rien au modèle');
  {
    // Parc synthétique à partir de l'instantané réel : une station sur deux suit un cycle journalier strict (le
    // profil l'explique, son couple s'écarte de celui de la classe), l'autre une marche aléatoire. 3,4 jours au
    // pas de 5 min depuis le lundi 26/10/2026 0 h (Luxembourg) : fin le jeudi vers 9 h 36, classe « ouvré » mûre.
    const HERE = dirname(fileURLToPath(import.meta.url));
    const DON = join(HERE, '..', '..', 'docs', 'donnees');
    const info = readFileSync(join(DON, 'station_information.json'), 'utf8'), types = readFileSync(join(DON, 'vehicle_types.json'), 'utf8');
    const st0 = JSON.parse(readFileSync(join(DON, 'station_status.json'), 'utf8'));
    const e = await loadEngine(readFileSync(join(HERE, '..', 'public', 'vn-core.wasm')), { onLog: () => {} });
    let seed = 7;
    const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
    const t0 = Date.UTC(2026, 9, 25, 23, 0, 0) / 1000;
    e.clock(t0, 1000); e.init(); e.settings({ refreshSec: 60 });
    const caps = st0.data.stations.map((x) => x.num_vehicles_available + x.num_docks_available);
    const walk = st0.data.stations.map((x) => x.num_vehicles_available);
    let ms = 1000, tLast = t0;
    for (let k = 0; k < Math.floor(3.4 * 288); k++) {
      tLast = t0 + k * 300 + 20; ms += 300000; e.clock(tLast, ms);
      if (k === 0) e.ingest(FEED.TYPES, true, 200, types, 100, 0);
      if (k % 12 === 0) e.ingest(FEED.INFO, true, 200, info, 100, 0);
      const tod = (k % 288) / 288;
      st0.data.stations.forEach((x, i) => {
        const cap = caps[i];
        let b;
        if (i % 2 === 0) b = Math.round(cap * (0.5 + 0.42 * Math.sin(2 * Math.PI * (tod + (i % 7) / 7))));
        else { walk[i] = Math.max(0, Math.min(cap, walk[i] + Math.round((rnd() - 0.5) * 4))); b = walk[i]; }
        x.num_vehicles_available = b; x.num_docks_available = cap - b;
      });
      st0.last_updated = new Date(tLast * 1000).toISOString().replace('.000', '');
      e.ingest(FEED.STATUS, true, 200, JSON.stringify(st0), 300, 900);
    }
    e.save();
    const before = Buffer.from(e.getFile('/vn_prof.bin'));
    const f1 = e.forecast(), f2 = e.forecast();
    const rows = f1.st.map((v, i) => [v, f1.cl?.[i]]).filter(([v]) => v);
    const differ = rows.filter(([v, c]) => c[0] !== v[3] || c[1] !== v[4] || c[2] !== v[5]).length;
    ok(rows.length > 100 && differ >= 10 && differ < rows.length, `le témoin diffère du chiffre affiché sur ${differ} stations (de ${rows.length}), pas sur toutes`);
    const nErr = f1.er.filter((x) => x !== null).length;
    ok(Array.isArray(f1.er) && f1.er.length === f1.st.length && f1.st.every((v, i) => v !== null || f1.er[i] === null) && nErr >= 50
      && f1.er.every((x) => x === null || (Number.isInteger(x) && x >= 0 && x < 2000)), `erreur annoncée à +60 min : ${nErr} stations jugées sur elles-mêmes (centièmes de vélo), null ailleurs`);
    const needs = f1.nd.filter((x) => x && x[0] !== 0);
    ok(f1.nd.length === f1.st.length && f1.st.every((v, i) => (v === null) === (f1.nd[i] === null)) && needs.length >= 5
      && f1.nd.every((x) => x === null || (x.length === 2 && x.every(Number.isInteger) && (x[0] !== 0 || x[1] === 0) && Math.abs(x[1]) <= 60 && Math.abs(x[1]) !== 1)),
      `besoin : ${needs.length} stations en tension avec les deux règles ; témoin nul là où la règle livrée ne demande rien, jamais ± 1`);
    ok(needs.some((x) => x[0] !== x[1]), 'les deux règles ne conseillent pas toujours le même déplacement');
    ok(JSON.stringify(f1) === JSON.stringify(f2), 'deux lectures de suite : mêmes chiffres (le levier des couples par station est bien remis)');
    e.clock(tLast, ms); e.save();
    ok(before.equals(Buffer.from(e.getFile('/vn_prof.bin'))), 'calculer le témoin ne change pas un octet de ce qui est appris (/vn_prof.bin)');
  }
  console.log('1.5.0 : erreur annoncée, cases du risque de rupture, besoin témoin — notés contre un calcul indépendant');
  {
    const s = writeDay(fresh(), { with15: true }).score(DAY);
    const CAP = { A: 20, E: 25 };
    // erreur annoncée contre constatée : A seule en annonce une (E : null), 64 prévisions à +60 min
    let ann = 0, real = 0, n = 0;
    for (const k of PRED_K) { ann += 1.5; real += Math.abs(VAL.A(k) + 3 - VAL.A(k + 12)); n++; }
    ok(s.errCheck.n === n && same(s.errCheck.ann, ann) && same(s.errCheck.real, real), `erreur annoncée ${s.errCheck.ann} / constatée ${s.errCheck.real} sur ${s.errCheck.n} prévisions (attendu ${ann} / ${real} sur ${n})`);
    // cases du risque : (relevé, prévision à +60, alerte) → [prévisions, ruptures vues dans l'heure]
    const want = { e: {}, f: {} };
    const add = (t, key, hit) => { const c = (t[key] ||= [0, 0]); c[0]++; if (hit) c[1]++; };
    for (const k of PRED_K) for (const sid of ['A', 'E']) {
      const cur = VAL[sid](k), cap = CAP[sid], fut = Array.from({ length: 12 }, (_, j) => VAL[sid](k + j + 1));
      if (cur > 0) add(want.e, riskKey(cur, cur + 3, cur <= 2), fut.includes(0));
      if (cur < cap) add(want.f, riskKey(cap - cur, cap - cur - 3, false), fut.some((x) => x >= cap));
    }
    ok(JSON.stringify(s.risk.e) === JSON.stringify(want.e) && Object.keys(want.e).length > 5, `risque « vide » : ${Object.keys(s.risk.e).length} cases, identiques au calcul indépendant`);
    ok(JSON.stringify(s.risk.f) === JSON.stringify(want.f) && Object.values(s.risk.f).every((c) => c[1] === 0), 'risque « pleine » : cases identiques, aucune saturation dans ce jour');
    ok(Object.entries(s.risk.e).some(([k, c]) => (+k & 1) === 1 && c[0] > 0) && Object.entries(s.risk.e).some(([k]) => (+k & 1) === 0), 'l\'alerte « vide dans l\'heure » fait partie de la case');
    // besoin : trajectoire relevée sur 4 h décalée du déplacement, borné par ce que la station peut donner ou recevoir
    const t = { n: 0, slots: 0, bad0: 0, bad50: 0, badP: 0, mv50: 0, mvP: 0 };
    let asked = 0;
    for (const k of PRED_K) for (const sid of ['A', 'E']) {
      const cur = VAL[sid](k), cap = CAP[sid], nd = ND(sid, cur);
      if (!nd[0]) continue;
      const cl = (d) => Math.max(-cur, Math.min(cap - cur, d)), d50 = cl(nd[0]), dP = cl(nd[1]);
      const bad = (y) => y <= 0 || y >= cap;
      t.n++; t.mv50 += Math.abs(d50); t.mvP += Math.abs(dP); asked += Math.abs(nd[1]);
      for (let j = 1; j <= 48; j++) { const x = VAL[sid](k + j); t.slots++; if (bad(x)) t.bad0++; if (bad(x + d50)) t.bad50++; if (bad(x + dP)) t.badP++; }
    }
    ok(JSON.stringify(s.target) === JSON.stringify(t) && t.n > 20 && t.mvP < asked, `besoin (témoin borné : ${t.mvP} vélos déplacés pour ${asked} demandés) : ${s.target.n} cas, tranches en rupture ${s.target.bad0} / ${s.target.bad50} / ${s.target.badP} (sans rien / 50 % / demande à venir), comme le calcul indépendant`);
    const s0 = writeDay(fresh()).score(DAY);
    ok(s0.errCheck.n === 0 && s0.target.n === 0 && Object.keys(s0.risk.e).length > 5, 'journal d\'avant la 1.5.0 : ni erreur annoncée ni besoin témoin ; le risque se note quand même (relevé, prévision et alerte y sont)');
  }
  console.log('erreur annoncée : fidèle, trop optimiste, trop prudente, en cours');
  {
    const D = (k) => `2026-11-${String(k).padStart(2, '0')}`;
    const mk = (ratios) => ratios.map((r, k) => ({ day: D(k + 1), complete: true, errCheck: { ann: 1000, real: 1000 * r, n: 900 } }));
    const fid = errCheckVerdict(mk([1.02, 0.97, 1.05, 0.99, 1.03, 0.96, 1.01, 1.04]));
    ok(fid.verdict === 'fidèle' && same(fid.ratio, 1.0088) && fid.lo > 0.85 && fid.hi < 1.15 && fid.n === 7200 && same(fid.announced, +(1000 / 900).toFixed(4)), `8 journées autour de 1 : « fidèle » (${fid.ratio} [${fid.lo} ; ${fid.hi}])`);
    ok(errCheckVerdict(mk([1.4, 1.5, 1.35, 1.45, 1.5, 1.4, 1.38])).verdict === 'trop optimiste', 'on se trompe 40 % de plus qu\'annoncé : « trop optimiste »');
    ok(errCheckVerdict(mk([0.6, 0.65, 0.7, 0.62, 0.66, 0.64, 0.61])).verdict === 'trop prudente', 'on se trompe moins qu\'annoncé : « trop prudente »');
    ok(errCheckVerdict(mk([0.7, 1.4, 0.8, 1.3, 0.75, 1.35, 1.0])).verdict === 'non concluant', 'journées qui se contredisent : « non concluant »');
    const few = errCheckVerdict(mk([1.0, 1.01, 0.99, 1.0, 1.02, 0.98]));
    ok(few.verdict === 'en cours' && few.days === 6, '6 journées : « en cours »');
    ok(errCheckVerdict([]).ratio === null, 'aucune journée : rien d\'affirmé');
  }
  console.log('risque de rupture : annoncé avec les seules journées passées, vérifié par tranche, masqué tant qu\'il ne l\'est pas');
  {
    const D = (k) => `2026-11-${String(k).padStart(2, '0')}`;
    // Monde fabriqué : quatre cases dont la vraie fréquence de rupture est connue (2 %, 20 %, 40 %, 85 %)
    const K = [riskKey(9, 9, false), riskKey(3, 2, false), riskKey(2, 1, false), riskKey(1, 0, true)], F = [0.02, 0.20, 0.40, 0.85], N = [3000, 800, 500, 300];
    const dayCells = (freq, wob) => Object.fromEntries(K.map((k, q) => [k, [N[q], Math.round(N[q] * Math.min(1, Math.max(0, freq[q] + wob * (q % 2 ? 0.01 : -0.01))))]]));
    const stable = Array.from({ length: 12 }, (_, d) => ({ day: D(d + 1), complete: true, risk: { e: dayCells(F, (d % 3) - 1), f: {} } }));
    const ev = riskEval(stable);
    const e = ev.empty;
    ok(e.trainDays === 12 && e.judgedDays === 12 - RISK_TRAIN_DAYS && e.verdict === 'vérifiée', `12 journées stables : ${e.judgedDays} annoncées après ${RISK_TRAIN_DAYS} d'apprentissage, « ${e.verdict} »`);
    const used = e.bins.filter((b) => b.n >= RISK_MIN_CASES);
    ok(used.length === 4 && used.every((b) => b.verdict === 'vérifiée' && Math.abs(b.observed - b.announced) < 0.03), `quatre tranches peuplées, annoncé ≈ constaté (${used.map((b) => `${b.announced}/${b.observed}`).join(' · ')})`);
    ok(e.brier < e.brierBase && e.brier < e.brierAlert && e.skillVsBase > 0.2, `score de Brier ${e.brier} : mieux que la part du parc (${e.brierBase}) et que l'alerte seule (${e.brierAlert})`);
    // calcul indépendant de la première journée annoncée (la 4ᵉ) : table des 3 premières, rétrécissement à deux niveaux
    {
      const tr = stable.slice(0, RISK_TRAIN_DAYS), cell = {}, lvl = {};
      let tn = 0, th = 0;
      for (const d of tr) for (const [k, [n, h]] of Object.entries(d.risk.e)) { const c = (cell[k] ||= [0, 0]); c[0] += n; c[1] += h; const l = (lvl[Math.floor(k / 26)] ||= [0, 0]); l[0] += n; l[1] += h; tn += n; th += h; }
      const base = (th + 1) / (tn + 2);
      const p = (k) => { const l = lvl[Math.floor(k / 26)], c = cell[k]; return (c[1] + RISK_K * ((l[1] + RISK_K * base) / (l[0] + RISK_K))) / (c[0] + RISK_K); };
      let b = 0, n = 0;
      for (const [k, [cn, ch]] of Object.entries(stable[RISK_TRAIN_DAYS].risk.e)) { b += ch * (1 - p(k)) ** 2 + (cn - ch) * p(k) ** 2; n += cn; }
      const one = riskEval(stable.slice(0, RISK_TRAIN_DAYS + 1)).empty;
      ok(one.judgedDays === 1 && one.n === n && same(one.brier, +(b / n).toFixed(4)) && one.verdict === 'en cours', `une journée annoncée : Brier ${one.brier} = calcul indépendant ${(b / n).toFixed(4)}, « en cours »`);
    }
    // pas de fuite : une journée très différente de son passé (80 % de ruptures après trois journées à 20 %) est
    // annoncée à ~20 %, la fréquence de SON PASSÉ — pas à une valeur déjà tirée vers ce qu'elle va montrer
    {
      const k1 = riskKey(3, 2, false), past = Array.from({ length: RISK_TRAIN_DAYS }, (_, d) => ({ day: D(d + 1), complete: true, risk: { e: { [k1]: [500, 100] }, f: {} } }));
      const hn = 100 * RISK_TRAIN_DAYS, nn = 500 * RISK_TRAIN_DAYS, base = (hn + 1) / (nn + 2), pl = (hn + RISK_K * base) / (nn + RISK_K), pc = (hn + RISK_K * pl) / (nn + RISK_K);
      const r = riskEval([...past, { day: D(RISK_TRAIN_DAYS + 1), complete: true, risk: { e: { [k1]: [500, 400] }, f: {} } }]).empty;
      const b = r.bins.find((x) => x.n);
      ok(r.judgedDays === 1 && b.n === 500 && same(b.announced, +pc.toFixed(4)) && same(b.observed, 0.8) && b.from === 0.15,
        `journée à 80 % après un passé à 20 % : annoncée ${b.announced} (son passé seul : ${pc.toFixed(4)}), constatée ${b.observed}`);
    }
    ok(riskEval(stable.slice(0, RISK_TRAIN_DAYS)).empty.judgedDays === 0 && riskEval(stable.slice(0, RISK_TRAIN_DAYS)).empty.n === 0, `${RISK_TRAIN_DAYS} journées : apprentissage seulement, rien d'annoncé`);
    // Le monde change après un long passé (la case à 20 % passe à 60 %) : la table, lourde de son passé, continue
    // d'annoncer ~25 % — la tranche est « fausse », l'ensemble aussi, et c'est dit.
    const heavy = (d) => Object.fromEntries(Object.entries(d).map(([k, [n, h]]) => [k, [n * 40, h * 40]]));
    const drift = stable.map((d, q) => (q < RISK_TRAIN_DAYS ? { ...d, risk: { e: heavy(d.risk.e), f: {} } } : { ...d, risk: { e: dayCells([0.02, 0.60, 0.40, 0.85], 0), f: {} } }));
    const dv = riskEval(drift).empty;
    ok(dv.verdict === 'fausse' && dv.bins.some((b) => b.verdict === 'fausse' && b.observed > b.announced + 0.1), `fréquence réelle qui change : « ${dv.verdict} » (${dv.bins.filter((b) => b.verdict === 'fausse').map((b) => `${b.announced} annoncé, ${b.observed} constaté`).join(' ; ')})`);
    // Le même changement sur un passé court : la table suit, les tranches bougent — pas « fausse », mais plus « vérifiée »
    const soft = stable.map((d, q) => (q < 4 ? d : { ...d, risk: { e: dayCells([0.02, 0.60, 0.40, 0.85], 0), f: {} } }));
    const sv = riskEval(soft);
    ok(sv.empty.verdict !== 'vérifiée' && riskNow(sv, [[3, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, -1, -1, 20]]).empty === null, `changement récent : « ${sv.empty.verdict} », donc rien en direct`);
    // en direct : rien tant que ce n'est pas vérifié ; ensuite la liste triée, stations vides écartées (déjà en rupture)
    const rows = [[1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 30, -1, 20], [9, 9, 9, 9, 9, 9, 0, 0, 0, 0, 0, -1, -1, 20], null, [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 20], [2, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, -1, -1, 20]];
    const live = riskNow(ev, rows), hidden = riskNow(riskEval(drift), rows);
    ok(hidden.empty === null && hidden.full === null, 'probabilité non vérifiée : rien en direct');
    ok(live.full === null && live.empty.length === 3 && live.empty[0].i === 0 && live.empty[1].i === 4 && live.empty[2].i === 1 && live.empty[0].p > 0.75 && live.empty[2].p < 0.05,
      `vérifiée : ${live.empty.map((x) => `st${x.i} ${Math.round(x.p * 100)} %`).join(' · ')} ; côté « pleine » (jamais vérifié ici) toujours masqué`);
    ok(!JSON.stringify(ev).includes('tables'), 'la table interne ne part pas dans la réponse de l\'API');
  }
  console.log('besoin conseillé : comparaison estimée des deux règles');
  {
    const D = (k) => `2026-11-${String(k).padStart(2, '0')}`;
    const mk = (pairs) => pairs.map(([b50, bP], k) => ({ day: D(k + 1), complete: true, target: { n: 40, slots: 1920, bad0: 300, bad50: b50, badP: bP, mv50: 240, mvP: 200 } }));
    const v = targetVerdict(mk([[120, 90], [110, 95], [130, 100], [115, 92], [125, 97], [118, 99], [122, 91]]));
    const S = 7 * 1920;
    ok(v.cases === 280 && same(v.none, +(100 * 2100 / S).toFixed(2)) && same(v.half, +(100 * 840 / S).toFixed(2)) && same(v.ahead, +(100 * 664 / S).toFixed(2))
      && same(v.movedHalf, 6) && same(v.movedAhead, 5) && same(v.diff, +(100 * (664 - 840) / S).toFixed(2)) && v.verdict === 'moins de ruptures',
      `7 journées : ${v.none} % sans rien, ${v.half} % règle 50 %, ${v.ahead} % demande à venir, écart ${v.diff} [${v.lo} ; ${v.hi}] : « ${v.verdict} »`);
    ok(targetVerdict(mk([[90, 120], [95, 110], [100, 130], [92, 115], [97, 125], [99, 118], [91, 122]])).verdict === 'plus de ruptures', 'rôles échangés : « plus de ruptures »');
    ok(targetVerdict(mk([[120, 90], [90, 120], [130, 100], [100, 130], [125, 97], [97, 125], [110, 110]])).verdict === 'non concluant', 'journées qui se contredisent : « non concluant »');
    ok(targetVerdict(mk([[120, 90], [110, 95]])).verdict === 'en cours' && targetVerdict([]).cases === 0, 'deux journées : « en cours » ; aucune : rien');
  }
  console.log('calendrier et prévision par type de jour');
  {
    const cal0 = loadCalendar(fresh());
    ok(!cal0.file && cal0.events.length === 0 && cal0.school.length === SCHOOL_HOLIDAYS.length && cal0.errors.length === 0, 'sans calendrier.json : les vacances officielles, aucun événement');
    ok(dayType(cal0, '2026-11-03', 0).type === 'vacances' && dayType(cal0, '2026-11-03', 0).name === 'Toussaint' && dayType(cal0, '2026-11-09', 0).type === 'ouvre'
      && dayType(cal0, '2026-10-30', 0).type === 'ouvre' && dayType(cal0, '2026-11-01', 1).type === 'weekend' && dayType(cal0, '2026-12-28', 0).type === 'vacances'
      && dayType(cal0, '2027-01-04', 0).type === 'ouvre', 'Toussaint 2026 du 31/10 au 08/11 : mardi 03/11 « vacances », lundi 09/11 « ouvré », dimanche 01/11 « week-end et fériés »');
    ok(SCHOOL_HOLIDAYS.every(([du, au], k) => du < au && new Date(`${du}T12:00:00Z`).getUTCDay() >= 5 && (k === 0 || SCHOOL_HOLIDAYS[k - 1][1] < du)),
      'périodes officielles : dans l\'ordre, sans recouvrement, commençant un vendredi ou un samedi');
    const dir = fresh();
    mkdirSync(dir, { recursive: true });
    writeFileSync(join(dir, 'calendrier.json'), JSON.stringify({ evenements: [{ du: '2026-11-05', au: '2026-11-06', nom: 'Foire' }, { du: '2026-13-40', nom: 'faux' }, { du: '2026-11-20', au: '2026-11-10', nom: 'à l\'envers' }],
      vacances: [{ du: '2026-11-16', nom: 'pont' }] }));
    const cal = loadCalendar(dir);
    ok(cal.file && cal.events.length === 1 && cal.errors.length === 2 && dayType(cal, '2026-11-05', 0).type === 'evenement' && dayType(cal, '2026-11-05', 0).name === 'Foire'
      && dayType(cal, '2026-11-16', 0).type === 'vacances' && dayType(cal, '2026-11-07', 1).type === 'weekend',
      `calendrier.json : 1 événement gardé, 2 entrées fausses nommées (${cal.errors.length}), l'événement passe avant les vacances`);
    writeFileSync(join(dir, 'calendrier.json'), '{ pas du json');
    ok(loadCalendar(dir).errors.length === 1 && loadCalendar(dir).events.length === 0, 'calendrier.json illisible : dit, rien d\'inventé');
    // rapport par type : MAE du modèle et de la persistance à +60 min, écart apparié par journées
    const note = (day, m, p, n) => ({ day, complete: true, model: [0, 0, { s: m, n }], per: [0, 0, { s: p, n }] });
    const notes = [note('2026-10-27', 900, 1000, 1000), note('2026-10-28', 880, 1000, 1000), note('2026-10-29', 910, 1000, 1000), note('2026-10-30', 905, 1000, 1000),
      note('2026-10-31', 500, 520, 600), note('2026-11-01', 480, 500, 600),
      note('2026-11-02', 1150, 1000, 1000), note('2026-11-03', 1180, 1000, 1000), note('2026-11-04', 1120, 1000, 1000), note('2026-11-05', 1160, 1000, 1000)];
    const cls = (d) => (['2026-10-31', '2026-11-01'].includes(d) ? 1 : 0);
    const rep = dayTypeReport(notes, (d) => dayType(cal0, d, cls(d)));
    const by = Object.fromEntries(rep.map((r) => [r.type, r]));
    ok(rep.length === 3 && by.ouvre.days === 4 && same(by.ouvre.model, 0.8988) && same(by.ouvre.per, 1) && same(by.ouvre.gainPct, 10.1) && by.ouvre.verdict === 'gagne',
      `jours ouvrés hors vacances : ${by.ouvre.model} contre ${by.ouvre.per}, « ${by.ouvre.verdict} »`);
    ok(by.vacances.days === 4 && same(by.vacances.gainPct, -15.3) && by.vacances.verdict === 'perd' && by.vacances.names[0] === 'Toussaint' && by.weekend.verdict === 'en cours',
      `vacances de la Toussaint : le modèle « ${by.vacances.verdict} » contre la persistance (${by.vacances.gainPct} %) ; week-end, 2 journées : « ${by.weekend.verdict} »`);
    ok(dayTypeReport(notes, null) === null, 'sans calendrier branché : pas de rapport par type');
  }
  console.log('verdict du témoin : quotient par journées, intervalle de Student, aucun verdict avant une semaine');
  {
    // notes fabriquées : par jour, somme des erreurs du modèle et du témoin à +60 min sur n paires
    const note = (day, sm, sc, n, complete = true) => ({ day, complete, shadow: {
      model: [0, 0, 1, 1, 1].map((on) => (on ? { s: sm, n } : { s: 0, n: 0 })), cls: [0, 0, 1, 1, 1].map((on) => (on ? { s: sc, n } : { s: 0, n: 0 })) } });
    const D = (k) => `2026-11-${String(k).padStart(2, '0')}`;
    const data = [[900, 1000, 1000], [1780, 2000, 2000], [1400, 1500, 1500], [950, 1000, 1000], [1900, 2000, 2000], [930, 1000, 1000], [1850, 2000, 2000], [1420, 1500, 1500]];
    const notes = data.map(([sm, sc, n], k) => note(D(k + 1), sm, sc, n));
    const six = shadowVerdict(notes.slice(0, 6));
    ok(six.days === 6 && six.horizons.length === 3 && six.horizons.every((r) => r.verdict === 'en cours' && r.lo !== null),
      `6 journées : intervalle publié, verdict « en cours » (il en faut ${SHADOW_MIN_DAYS})`);
    const v = shadowVerdict(notes);
    // calcul indépendant
    const S = data.reduce((a, [sm, sc]) => a + sm - sc, 0), N = data.reduce((a, d) => a + d[2], 0), R = S / N, J = data.length;
    const q = data.reduce((a, [sm, sc, n]) => a + (sm - sc - R * n) ** 2, 0);
    const half = 2.365 * Math.sqrt(q * J / (J - 1)) / N;                       // Student, 7 degrés de liberté
    const r = v.horizons[0];
    ok(r.h === '+60 min' && r.n === N && r.days === J && same(r.diff, +R.toFixed(4)) && same(r.lo, +(R - half).toFixed(4)) && same(r.hi, +(R + half).toFixed(4)),
      `8 journées : écart ${r.diff} [${r.lo} ; ${r.hi}], attendu ${R.toFixed(4)} [${(R - half).toFixed(4)} ; ${(R + half).toFixed(4)}]`);
    ok(r.verdict === 'gagne' && r.hi < 0 && same(r.model, +(data.reduce((a, d) => a + d[0], 0) / N).toFixed(4)), `tout l'intervalle sous zéro : « gagne » (${r.verdict})`);
    const lose = shadowVerdict(data.map(([sm, sc, n], k) => note(D(k + 1), sc, sm, n)));
    ok(lose.horizons[0].verdict === 'perd' && same(lose.horizons[0].diff, -r.diff), `rôles échangés : « perd », écart opposé (${lose.horizons[0].verdict}, ${lose.horizons[0].diff})`);
    const mixed = shadowVerdict(data.map(([sm, sc, n], k) => note(D(k + 1), k % 2 ? sm : sc + (sc - sm), sc, n)));
    ok(mixed.horizons[0].verdict === 'non concluant' && mixed.horizons[0].lo < 0 && mixed.horizons[0].hi > 0, `jours qui se contredisent : « non concluant » (${mixed.horizons[0].lo} ; ${mixed.horizons[0].hi})`);
    const part = shadowVerdict([...notes, note(D(9), 100, 5000, 1000, false), note(D(10), 100, 5000, 1000)], D(10));
    ok(part.days === 8 && same(part.horizons[0].diff, r.diff), 'journée incomplète et journée en cours : hors du verdict');
    const none = shadowVerdict([]);
    ok(none.days === 0 && none.horizons.every((x) => x.verdict === 'en cours' && x.diff === null), 'aucune journée : rien d\'affirmé');
  }
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`\n${checks} vérifications, ${fails} échec(s)`);
process.exit(fails ? 1 : 0);
