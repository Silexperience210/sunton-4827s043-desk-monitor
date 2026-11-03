// ── Réplication : le jumeau calcule-t-il exactement comme le serveur ? ─
// Sans réseau : un « parc » simulé à partir de l'instantané réel docs/donnees (marche aléatoire des
// vélos, pannes de flux, météo qui change), rejoué sur 3 jours au pas de 60 s.
//  1. serveur (vn-core.wasm) et jumeau (vn-twin.wasm) reçoivent les mêmes appels : leurs fichiers
//     sauvegardés doivent être identiques octet pour octet, à chaque sauvegarde ;
//  2. un jumeau qui démarre en cours de route sur un instantané du serveur, puis rejoue la suite,
//     doit lui aussi finir identique (c'est ce que fait la page) ;
//  3. un instantané relu par Model::load (le chemin de la carte) redonne les mêmes totaux.
// node online/test/replication.test.mjs
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadEngine, FEED, ING } from '../public/vn-engine.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..', '..');
const D = join(ROOT, 'docs', 'donnees');
const PUB = join(HERE, '..', 'public');
let fails = 0, checks = 0;
const ok = (c, msg) => { checks++; if (!c) { fails++; console.log(`  ÉCHEC : ${msg}`); } };
const quiet = { onLog: () => {} };

// ── Parc simulé (graine fixe) ──
let seed = 12345;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
const info = readFileSync(join(D, 'station_information.json'), 'utf8');
const types = readFileSync(join(D, 'vehicle_types.json'), 'utf8');
const wx0 = JSON.parse(readFileSync(join(D, 'weather.json'), 'utf8'));
const st0 = JSON.parse(readFileSync(join(D, 'station_status.json'), 'utf8'));
const elecId = JSON.parse(types).data.vehicle_types.find((v) => /elec/i.test(v.propulsion_type || ''))?.vehicle_type_id;
function statusAt(t) {
  const hour = new Date(t * 1000).getUTCHours();
  for (const s of st0.data.stations) {
    const cap = s.num_vehicles_available + s.num_docks_available;
    const drift = hour >= 6 && hour < 10 ? -0.6 : hour >= 16 && hour < 20 ? 0.6 : 0;
    let b = s.num_vehicles_available + Math.round((rnd() - 0.5) * 2 + drift * rnd());
    b = Math.max(0, Math.min(cap, b));
    s.num_vehicles_available = b;
    s.num_docks_available = cap - b;
    if (s.vehicle_types_available && elecId) s.vehicle_types_available = [{ vehicle_type_id: elecId, count: b }];
  }
  st0.last_updated = new Date(t * 1000).toISOString().replace('.000', '');
  return JSON.stringify(st0);
}
function weatherAt(t) {
  const w = structuredClone(wx0);
  const rain = Math.sin(t / 20000) > 0.6 ? 2.4 : 0;
  w.current.precipitation = rain;
  w.current.weather_code = rain ? 63 : 3;
  w.current.temperature_2m = 9 + 5 * Math.sin(t / 43200);
  return JSON.stringify(w);
}

// Le même déroulé que lib/collector.mjs : un évènement = un appel au moteur
function* scenario(t0, cycles) {
  let wxAt = 0;
  for (let k = 0; k < cycles; k++) {
    const t = t0 + k * 60;
    yield { k: 'fetching', on: true, t };
    if (k === 0) yield { k: 'ingest', t, feed: FEED.TYPES, got: true, code: 200, body: types, dur: 120, cyc: 0 };
    if (k % 60 === 0) yield { k: 'ingest', t, feed: FEED.INFO, got: true, code: 200, body: info, dur: 300, cyc: 0 };
    if (k % 97 === 13) {                               // panne du flux : trois tentatives, échec
      yield { k: 'ingest', t: t + 5, feed: FEED.STATUS, got: false, code: -11, body: '', dur: 46000, cyc: 46500 };
      yield { k: 'cycle', t: t + 5, ok: false };
    } else if (k % 131 === 7) {                        // JSON tronqué
      yield { k: 'ingest', t: t + 1, feed: FEED.STATUS, got: true, code: 200, body: '{"data":{"stati', dur: 300, cyc: 900 };
      yield { k: 'cycle', t: t + 1, ok: false };
    } else {
      yield { k: 'ingest', t: t + 1, feed: FEED.STATUS, got: true, code: 200, body: statusAt(t + 1), dur: 300, cyc: 900 };
    }
    yield { k: 'fetching', on: false, t: t + 2 };
    if (!wxAt || t - wxAt >= 900) {
      wxAt = t;
      yield k % 50 === 3 ? { k: 'ingest', t: t + 3, feed: FEED.WEATHER, got: false, code: 503, body: '', dur: 2000, cyc: 0 }
        : { k: 'ingest', t: t + 3, feed: FEED.WEATHER, got: true, code: 200, body: weatherAt(t), dur: 200, cyc: 0 };
    }
  }
}
function apply(e, ev, ms) {
  e.clock(ev.t, ms);
  switch (ev.k) {
    case 'ingest': return e.ingest(ev.feed, ev.got, ev.code, ev.body, ev.dur, ev.cyc);
    case 'cycle': return e.cycleEnd(ev.ok);
    case 'fetching': return e.setFetching(ev.on);
  }
}
const files = ['/vn_prof.bin', '/vn_hist.bin', '/vn_days.bin', '/vn_live.bin'];
function saved(e, t, ms) {
  e.clock(t, ms);
  e.save();
  return Object.fromEntries(files.map((f) => [f, e.getFile(f)]));
}
const same = (a, b) => (!a && !b) || (a && b && a.length === b.length && a.every((v, i) => v === b[i]));

const core = readFileSync(join(PUB, 'vn-core.wasm'));
const twinW = readFileSync(join(PUB, 'vn-twin.wasm'));
const server = await loadEngine(core, quiet);
const twin = await loadEngine(twinW, quiet);
const t0 = Date.UTC(2026, 9, 23, 22, 0, 0) / 1000;      // jeudi 22/10 minuit à Luxembourg — passage à l'heure d'hiver le 25/10
for (const e of [server, twin]) { e.clock(t0, 1000); e.init(); e.settings({ refreshSec: 60 }); }

console.log('1. serveur et jumeau, mêmes appels, 3 jours au pas de 60 s');
const evs = [...scenario(t0, 3 * 1440)];
let late = null, lateAt = Math.floor(evs.length * 0.62), ms = 1000;
for (let i = 0; i < evs.length; i++) {
  ms += 50;
  apply(server, evs[i], ms);
  apply(twin, evs[i], ms);
  if (i === lateAt) {                                  // instantané pris en cours de route (comme la page)
    const snap = saved(server, evs[i].t, ms);
    late = await loadEngine(twinW, quiet);
    late.clock(evs[i].t, ms);
    late.init(); late.settings({ refreshSec: 60 });
    for (const f of files) late.putFile(f, snap[f]);
    ok(late.load(true) === 3, 'le jumeau tardif relit les fichiers ET l\'état vivant');
  } else if (late && i > lateAt) apply(late, evs[i], ms);
  if (i % 997 === 0 || i === evs.length - 1) {
    const a = saved(server, evs[i].t, ms), b = saved(twin, evs[i].t, ms);
    for (const f of files) ok(same(a[f], b[f]), `évènement ${i} : ${f} diffère entre serveur et jumeau`);
  }
}
const tEnd = evs[evs.length - 1].t;
const sA = server.summary(), sB = twin.summary();
ok(JSON.stringify(sA) === JSON.stringify(sB), 'résumés identiques');
console.log(`   ${evs.length} appels · ${sA.learnSamples} tranches apprises · profil ${sA.learnPct.toFixed(1)} % · ${sA.daysKept} jours résumés · ${sA.alerts} alertes`);
ok(sA.learnSamples > 800, 'le profil a appris (tranches de 5 min)');
ok(sA.daysKept >= 3, 'trois jours au moins sont résumés');
ok(sA.feeds[0].errCount > 0 && sA.fetchErrors >= 0, 'les pannes du flux sont comptées');

console.log('2. jumeau démarré en cours de route sur un instantané, puis rejeu');
const a2 = saved(server, tEnd, ms), c2 = saved(late, tEnd, ms);
for (const f of files) ok(same(a2[f], c2[f]), `${f} diffère entre le serveur et le jumeau tardif`);
ok(JSON.stringify(server.summary()) === JSON.stringify(late.summary()), 'résumé identique pour le jumeau tardif');

console.log('3. chemin de la carte : Model::load sur les seuls fichiers de la carte');
const card = await loadEngine(core, quiet);
card.clock(tEnd, ms); card.init(); card.settings({ refreshSec: 60 });
for (const f of ['/vn_prof.bin', '/vn_hist.bin', '/vn_days.bin']) card.putFile(f, a2[f]);
ok((card.load(false) & 1) === 1, 'la carte relit les trois fichiers');
// Sans état vivant, elle n'a pas les stations à jour : un relevé suffit pour retrouver les totaux
card.clock(tEnd, ms); card.ingest(FEED.TYPES, true, 200, types, 100, 0); card.ingest(FEED.INFO, true, 200, info, 100, 0);
const lastStatus = evs.filter((e) => e.k === 'ingest' && e.feed === FEED.STATUS && e.got && e.body.length > 100).pop();
for (const e of [card, server]) { e.clock(tEnd + 60, ms + 60000); e.ingest(FEED.STATUS, true, 200, lastStatus.body, 300, 900); }
const cs = card.summary(), ss = server.summary();
ok(cs.totals.bikes === ss.totals.bikes && cs.totals.docks === ss.totals.docks, 'mêmes vélos / bornes après un relevé');
ok(cs.learnSamples === ss.learnSamples, `même apprentissage (${cs.learnSamples} contre ${ss.learnSamples})`);
ok(cs.daysKept === ss.daysKept, 'mêmes jours résumés');
ok(cs.histMinutes === ss.histMinutes, `même profondeur d'historique (${cs.histMinutes} contre ${ss.histMinutes} min)`);

console.log(`\n${checks} vérifications, ${fails} échec(s)`);
process.exit(fails ? 1 : 0);
