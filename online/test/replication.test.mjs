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
import { site } from './harnais.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..', '..');
const D = join(ROOT, 'docs', 'donnees');
const PUB = join(HERE, '..', 'public');
let fails = 0, checks = 0;
const ok = (c, msg) => { checks++; site(); if (!c) { fails++; console.log(`  ÉCHEC : ${msg}`); } };
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

console.log('3. témoin « couple de la classe » : aligné sur les prévisions, identique sur le serveur et le jumeau');
{
  const fs = server.forecast(), ft = twin.forecast();
  ok(JSON.stringify(fs) === JSON.stringify(ft), 'prévisions notées et témoin identiques sur le serveur et le jumeau');
  ok(Array.isArray(fs.cl) && fs.cl.length === fs.st.length && fs.st.every((v, i) => (v === null) === (fs.cl[i] === null)),
    `un témoin par station prévue, null aux mêmes places (${fs.cl?.length} pour ${fs.st.length})`);
  const rows = fs.st.map((v, i) => [v, fs.cl[i]]).filter(([v]) => v);
  ok(rows.length > 100 && rows.every(([v, c]) => c.length === 3 && c.every((x) => Number.isInteger(x) && x >= 0 && x <= v[13])),
    `témoin : trois entiers par station (+60, +120, +240 min), entre 0 et la capacité (${rows.length} stations)`);
  const differ = rows.filter(([v, c]) => c[0] !== v[3] || c[1] !== v[4] || c[2] !== v[5]).length;
  console.log(`   ${differ} station(s) sur ${rows.length} où le couple par station change un chiffre affiché`);
  ok(differ < rows.length, 'le témoin reprend le chiffre du modèle là où le couple ne change rien');
}

console.log('4. chemin de la carte : Model::load sur les seuls fichiers de la carte');
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

console.log('5. état vivant : prévisions d\'apprentissage en attente gardées, et fichier d\'une version 1 encore relu');
{
  // En-tête LiveHdr : version à l'octet 4, nSt à 6, taille d'une prévision en attente à 18 (0 en version 1).
  // Le bloc des prévisions en attente est en fin de fichier : 17 dates, 17 classes, la tête, puis 17 lots × nSt.
  const live = a2['/vn_live.bin'];
  const dv = new DataView(live.buffer, live.byteOffset, live.byteLength);
  const nSt = dv.getUint16(6, true), szIssue = dv.getUint16(18, true), PEND = 17;
  ok(dv.getUint16(4, true) === 2 && szIssue > 0, `état vivant en version 2 avec les prévisions en attente (version ${dv.getUint16(4, true)}, ${szIssue} o)`);
  const tail = PEND * 8 + PEND + 4 + szIssue * PEND * nSt;
  const pendT = [];
  for (let k = 0; k < PEND; k++) pendT.push(Number(dv.getBigInt64(live.length - tail + k * 8, true)));
  ok(pendT.filter((t) => t > tEnd - 4 * 3600 - 900 && t <= tEnd).length >= 1, 'des lots émis dans les 4 dernières heures sont dans le fichier');
  // version 1 fabriquée : même fichier sans le bloc, version et taille remises comme en 1.4.0
  const v1 = Buffer.from(live.subarray(0, live.length - tail));
  v1.writeUInt16LE(1, 4); v1.writeUInt16LE(0, 18);
  const old = await loadEngine(core, quiet);
  old.clock(tEnd, ms); old.init(); old.settings({ refreshSec: 60 });
  for (const f of ['/vn_prof.bin', '/vn_hist.bin', '/vn_days.bin']) old.putFile(f, a2[f]);
  old.putFile('/vn_live.bin', v1);
  ok(old.load(true) === 3, 'un état vivant de version 1 (sans prévisions en attente) est encore relu après la mise à jour');
  ok(old.summary().totals.bikes === server.summary().totals.bikes, 'et rend les mêmes totaux');
  // fichier amputé : refusé (taille), les trois fichiers de la carte restent relus
  const cut = await loadEngine(core, quiet);
  cut.clock(tEnd, ms); cut.init(); cut.settings({ refreshSec: 60 });
  for (const f of ['/vn_prof.bin', '/vn_hist.bin', '/vn_days.bin']) cut.putFile(f, a2[f]);
  cut.putFile('/vn_live.bin', Buffer.from(live.subarray(0, live.length - 7)));
  ok(cut.load(true) === 1, 'un état vivant amputé est refusé, les fichiers de la carte restent relus');
}

console.log(`\n${checks} vérifications, ${fails} échec(s)`);
process.exit(fails ? 1 : 0);
