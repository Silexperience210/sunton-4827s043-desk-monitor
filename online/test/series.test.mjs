// ── Séries de l'historique (public/vn-series.js) : heure de Luxembourg et changements d'heure ─
// Les jours sont ceux de Luxembourg : 288 tranches de 5 min, 300 le 25/10/2026 (on recule d'une heure), 276
// le 29/03/2026 (on avance). Chaque tranche synthétique compte 1 retrait : la somme d'un seau est sa durée.
//  m8. agrégats au pas de 180 et 1440 min : un seau par jour local (de minuit à minuit), seaux de 3 h à 0 h,
//      3 h, 6 h… ; couverture et retraits rapportés à la durée réelle du seau (25 h ou 23 h ces jours-là) ;
//      même règle pour le résumé par jour (dailySummary, depFull).
// node online/test/series.test.mjs
import { luxParts, dayOf, aggregate, dailySummary, SLOT_S } from '../public/vn-series.js';

let fails = 0, checks = 0;
const ok = (c, msg) => { checks++; if (!c) { fails++; console.log(`  ÉCHEC : ${msg}`); } else if (process.env.VN_TEST_DETAIL) console.log(`  ok : ${msg}`); };
const iso = (t) => new Date(t * 1000).toISOString().slice(0, 16);
const ST = [['1', 'Gare', 20, 49.6, 6.13]];
// tranches de 5 min de `from` (inclus) à `to` (exclu), epoch UTC
const slots = (from, to) => {
  const out = [];
  for (let t = from; t < to; t += SLOT_S) out.push({ t, n: [100, 50, 0, 100, 1, 1, 0, 0], w: null, b: [5], st: ST });
  return out;
};
const U = (y, mo, d, h) => Date.UTC(y, mo - 1, d, h) / 1000;

console.log('heure de Luxembourg : hiver, été, et les deux nuits de changement');
ok(luxParts(U(2026, 1, 15, 12)).h === 13 && luxParts(U(2026, 7, 1, 12)).h === 14, 'midi UTC : 13 h en janvier, 14 h en juillet');
ok(dayOf(U(2026, 7, 1, 22) + 1800) === '2026-07-02' && dayOf(U(2026, 1, 15, 22) + 1800) === '2026-01-15', '22 h 30 UTC : lendemain en été, même jour en hiver');
ok(slots(U(2026, 10, 24, 22), U(2026, 10, 25, 23)).every((r) => dayOf(r.t) === '2026-10-25'), '25/10/2026 : 300 tranches, de 22 h UTC la veille à 23 h UTC');
ok(slots(U(2026, 3, 28, 23), U(2026, 3, 29, 22)).every((r) => dayOf(r.t) === '2026-03-29'), '29/03/2026 : 276 tranches, de 23 h UTC la veille à 22 h UTC');

for (const [what, day, from, to, len] of [
  ['automne, 25/10/2026', '2026-10-25', U(2026, 10, 24, 22), U(2026, 10, 25, 23), 300],
  ['printemps, 29/03/2026', '2026-03-29', U(2026, 3, 28, 23), U(2026, 3, 29, 22), 276],
]) {
  console.log(`m8. ${what} : pas de 1440 et 180 min`);
  // la veille, le jour, le lendemain, complets
  const recs = slots(from - 86400, to + 86400);
  const prev = recs.filter((r) => dayOf(r.t) < day).length, next = recs.filter((r) => dayOf(r.t) > day).length;
  const d = aggregate(recs, { step: 1440 });
  ok(d.t.length === 3 && d.t.every((t) => luxParts(t).h === 0 && luxParts(t).mi === 0) && dayOf(d.t[1]) === day,
    `pas d'un jour : 3 seaux à minuit (${d.t.map((t) => `${dayOf(t)} ${String(luxParts(t).h).padStart(2, '0')}:${String(luxParts(t).mi).padStart(2, '0')}`).join(' · ')})`);
  ok(d.cover.every((c) => c === 1), `pas d'un jour : couverture ${d.cover.join(' · ')} (1 · 1 · 1 attendu)`);
  ok(d.dep[1] === len && d.dep[0] === prev && d.dep[2] === next, `pas d'un jour : retraits ${d.dep.join(' · ')} (${prev} · ${len} · ${next} attendus)`);
  // moitié des tranches du jour seulement : couverture 0,5, retraits ramenés à la journée entière
  const half = recs.filter((r) => dayOf(r.t) !== day || ((r.t - from) / SLOT_S) % 2 === 0);
  const h = aggregate(half, { step: 1440 });
  ok(h.cover[1] === 0.5 && h.dep[1] === len, `une tranche sur deux : couverture ${h.cover[1]} (0,5), retraits ramenés à ${h.dep[1]} (${len})`);
  const three = aggregate(recs.filter((r) => dayOf(r.t) === day), { step: 180 });
  const hours = three.t.map((t) => luxParts(t));
  ok(three.t.length === 8 && hours.every((p, k) => p.h === 3 * k && p.mi === 0), `pas de 3 h : 8 seaux à 0 h, 3 h… 21 h (${hours.map((p) => `${p.h}:${String(p.mi).padStart(2, '0')}`).join(' ')})`);
  ok(three.cover.every((c) => c === 1) && three.dep.reduce((a, b) => a + b, 0) === len,
    `pas de 3 h : couverture ${three.cover.join(' ')} ; retraits ${three.dep.join(' ')} (somme ${three.dep.reduce((a, b) => a + b, 0)}, ${len} attendus)`);
  const ds = dailySummary([[day, recs.filter((r) => dayOf(r.t) === day)], [`${day} (moitié)`, half.filter((r) => dayOf(r.t) === day)]]);
  ok(ds[0].slots === len && ds[0].depFull === len && ds[1].depFull === len, `résumé du jour : ${ds[0].slots} tranches, retraits ramenés à la journée ${ds[0].depFull} et ${ds[1].depFull} (${len})`);
}

console.log(`\n${checks} vérifications, ${fails} échec(s)`);
process.exit(fails ? 1 : 0);
