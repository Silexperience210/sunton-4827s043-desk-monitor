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
import { mkdtempSync, rmSync, mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { History } from '../lib/history.mjs';
import { ForecastLog } from '../lib/forecast.mjs';

let fails = 0, checks = 0;
const ok = (c, msg) => { checks++; if (!c) { fails++; console.log(`  ÉCHEC : ${msg}`); } else if (process.env.VN_TEST_DETAIL) console.log(`  ok : ${msg}`); };
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

function writeDay(D, { withFlag = true } = {}) {
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
    s += JSON.stringify({ slot: T0 + k * 300, t: T0 + k * 300 + 20, net60: 0, bikes: 0, st }) + '\n';
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
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`\n${checks} vérifications, ${fails} échec(s)`);
process.exit(fails ? 1 : 0);
