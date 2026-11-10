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
import { ForecastLog, shadowVerdict, SHADOW_MIN_DAYS } from '../lib/forecast.mjs';
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
function writeDay(D, { withFlag = true, withCl = false } = {}) {
  const h = new History(D);
  h.append(Array.from({ length: N }, (_, k) => ({ t: T0 + k * 300, n: null, w: null, b: ST.map((s) => VAL[s[0]](k)), st: ST })));
  mkdirSync(join(D, 'prevision'), { recursive: true });
  let s = JSON.stringify({ sid: ST.map((x) => x[0]) }) + '\n';
  for (const k of PRED_K) {
    const st = ST.map(([sid, , cap]) => {
      const cur = VAL[sid](k);
      const row = [cur, ...HK.map((_, h) => cur + h + 1), ...HK.map((_, h) => Math.max(0, cur - h - 1)), -1, -1, cap];
      return withFlag ? [...row, renting[sid]] : row;
    });
    const cl = withCl ? { cl: ST.map(([sid]) => CL_OFF.map((o) => VAL[sid](k) + o)) } : {};
    s += JSON.stringify({ slot: T0 + k * 300, t: T0 + k * 300 + 20, net60: 0, bikes: 0, st, ...cl }) + '\n';
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
    ok(JSON.stringify(f1) === JSON.stringify(f2), 'deux lectures de suite : mêmes chiffres (le levier des couples par station est bien remis)');
    e.clock(tLast, ms); e.save();
    ok(before.equals(Buffer.from(e.getFile('/vn_prof.bin'))), 'calculer le témoin ne change pas un octet de ce qui est appris (/vn_prof.bin)');
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
