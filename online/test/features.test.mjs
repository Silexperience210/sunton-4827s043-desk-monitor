// ── Historique long, erreur des prévisions, notifications, banc de nuit ─
// Sans réseau, horloge simulée : le parc de replication.test.mjs (instantané réel docs/donnees, marche
// aléatoire) passe par le moteur, l'historique (lib/history.mjs) et le journal des prévisions
// (lib/forecast.mjs) pendant 2 jours et quart au pas de 60 s. Tout est revérifié contre la VÉRITÉ du
// scénario (ce qui a été envoyé au moteur), pas contre le code testé :
//  1. chaque tranche écrite vaut le premier relevé reçu dans ses 5 min (la règle de l'anneau) ;
//  2. une prévision par quart d'heure de 6 h à 22 h, son relevé = la vérité ;
//  3. la note du jour : erreur de la persistance recalculée à la main, à l'identique ;
//  4. notifications (horloge et transport simulés) : rupture qui dure, regroupement, rétablie, plafond
//     horaire, heures calmes, flux muet, Sunton, résumé du matin ;
//  5. banc de nuit (si sim/bench_prevision est compilé) : archive simulée importée, banc lancé, rapport lu.
// node online/test/features.test.mjs
import { readFileSync, mkdtempSync, existsSync, readdirSync, rmSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { execFileSync, spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { cpSync } from 'node:fs';
import { loadEngine, FEED } from '../public/vn-engine.js';
import { History, importArchive, snapshotDirs, dayOf, parseDay } from '../lib/history.mjs';
import { ForecastLog, HK } from '../lib/forecast.mjs';
import { Notifier } from '../lib/notify.mjs';
import { NightBench, parseBenchReport } from '../lib/bench.mjs';
import { luxParts } from '../lib/store.mjs';
import { packRecords, unpackRecords, aggregate } from '../public/vn-series.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..', '..');
const D = join(ROOT, 'docs', 'donnees');
const PUB = join(HERE, '..', 'public');
let fails = 0, checks = 0;
const ok = (c, msg) => { checks++; if (!c) { fails++; console.log(`  ÉCHEC : ${msg}`); } };
const quiet = { onLog: () => {} };
const tmp = mkdtempSync(join(tmpdir(), 'vn-features-'));

// ── Parc simulé (graine fixe), comme replication.test.mjs ──
let seed = 4242;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
const info = readFileSync(join(D, 'station_information.json'), 'utf8');
const types = readFileSync(join(D, 'vehicle_types.json'), 'utf8');
const wx0 = JSON.parse(readFileSync(join(D, 'weather.json'), 'utf8'));
const st0 = JSON.parse(readFileSync(join(D, 'station_status.json'), 'utf8'));
const elecId = JSON.parse(types).data.vehicle_types.find((v) => /elec/i.test(v.propulsion_type || ''))?.vehicle_type_id;
function statusAt(t, truth) {
  const hour = luxParts(t).h;
  for (const s of st0.data.stations) {
    const cap = s.num_vehicles_available + s.num_docks_available;
    const drift = hour >= 7 && hour < 10 ? -0.5 : hour >= 16 && hour < 19 ? 0.5 : 0;
    let b = s.num_vehicles_available + Math.round((rnd() - 0.5) * 2 + drift * rnd());
    b = Math.max(0, Math.min(cap, b));
    s.num_vehicles_available = b;
    s.num_docks_available = cap - b;
    if (s.vehicle_types_available && elecId) s.vehicle_types_available = [{ vehicle_type_id: elecId, count: b }];
    truth[s.station_id] = b;
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

const core = readFileSync(join(PUB, 'vn-core.wasm'));
const e = await loadEngine(core, quiet);
const t0 = Date.UTC(2026, 2, 1, 23, 0, 0) / 1000;       // lundi 02/03/2026 minuit à Luxembourg (CET)
e.clock(t0, 1000); e.init(); e.settings({ refreshSec: 60 });
const DATA = join(tmp, 'serveur');
const hist = new History(DATA);
const fl = new ForecastLog(DATA, hist);
const truthBySlot = new Map();              // début de tranche → {sid: vélos du PREMIER relevé de la tranche}
const CYCLES = Math.round(2.25 * 1440);
let ms = 1000, wxAt = 0, captured = 0, logged = 0;
console.log('1. historique long : 2 jours et quart au pas de 60 s, tranches contre la vérité');
for (let k = 0; k < CYCLES; k++) {
  const t = t0 + k * 60;
  ms += 60000;
  e.clock(t, ms); e.setFetching(true);
  if (k === 0) e.ingest(FEED.TYPES, true, 200, types, 120, 0);
  if (k % 60 === 0) e.ingest(FEED.INFO, true, 200, info, 300, 0);
  e.clock(t + 1, ms);
  if (k % 89 === 17) { e.ingest(FEED.STATUS, false, -11, '', 46000, 46500); e.cycleEnd(false); }   // panne du flux
  else {
    const truth = {};
    const body = statusAt(t + 1, truth);
    e.ingest(FEED.STATUS, true, 200, body, 300, 900);
    const slot = (t + 1) - ((t + 1) % 300);
    if (!truthBySlot.has(slot)) truthBySlot.set(slot, truth);
  }
  e.clock(t + 2, ms); e.setFetching(false);
  if (!wxAt || t - wxAt >= 900) { wxAt = t; e.clock(t + 3, ms); e.ingest(FEED.WEATHER, true, 200, weatherAt(t), 200, 0); }
  e.clock(t + 4, ms);
  captured += hist.capture(e);
  if (fl.capture(e)) logged++;
}
const day1 = dayOf(t0), day2 = dayOf(t0 + 86400), day3 = dayOf(t0 + 2 * 86400);
const recs1 = hist.readDay(day1);
ok(recs1.length === 288, `${day1} : 288 tranches attendues, ${recs1.length} écrites`);
let bad = 0, cmp = 0;
for (const day of [day1, day2]) {
  for (const r of hist.readDay(day)) {
    const tr = truthBySlot.get(r.t);
    if (!tr) { bad++; continue; }
    r.st.forEach((s, i) => { cmp++; if (r.b[i] !== (tr[s[0]] ?? null)) bad++; });
  }
}
ok(bad === 0 && cmp > 70000, `chaque tranche = premier relevé de ses 5 min (${cmp} valeurs comparées, ${bad} écart(s))`);
const files = readdirSync(join(DATA, 'historique'));
ok(files.includes(`${day1}.jsonl.gz`) && !files.includes(`${day1}.jsonl`), 'le jour fini est compressé');
ok(files.includes(`${day3}.jsonl`), 'le jour en cours reste en clair');
const s60 = hist.series({ from: day1, to: day1, step: 60 });
ok(s60.t.length === 24, `série horaire : 24 points (${s60.t.length})`);
const depSum = recs1.reduce((a, r) => a + (r.n ? r.n[4] : 0), 0);
ok(s60.dep.reduce((a, b) => a + b, 0) === depSum, 'retraits horaires = somme des tranches');
ok(s60.cover.every((c) => c === 1), 'couverture complète');
const sidA = recs1[0].st[3][0];
const sSt = hist.series({ from: day1, to: day2, step: 15, sid: sidA });
ok(sSt.station && sSt.station.sid === sidA && sSt.bikes.length === 192 && sSt.bikes.every((v) => v !== null), 'série d\'une station au quart d\'heure');
const ds = hist.dailySummary(day1, day2);
ok(ds.length === 2 && ds[0].slots === 288 && ds[0].svcSlots === 180, 'résumé par jour tiré de l\'historique');
ok(hist.csv(day1, day1).trim().split('\n').length === 289, 'export CSV : une ligne par tranche');
ok(hist.at(recs1[100].t)?.t === recs1[100].t && hist.at(recs1[100].t + 7) === null, 'lecture d\'une tranche précise');
// redémarrage : rien n'est réécrit, l'anneau ne double rien
const hist2 = new History(DATA);
ok(hist2.lastT === hist.lastT, 'au redémarrage, la dernière tranche écrite est retrouvée');
ok(hist2.capture(e) === 0, 'l\'anneau relu ne réécrit aucune tranche');
ok(hist2.merge(recs1) === 0, 'fusion : les tranches déjà présentes ne rentrent pas deux fois');
const ring = e.slot(1);
const last = hist2.readDay(dayOf(ring.t)).find((r) => r.t === ring.t);
ok(last && JSON.stringify(last.b) === JSON.stringify(ring.b) && JSON.stringify(last.n) === JSON.stringify(ring.n), 'la dernière tranche écrite est celle de l\'anneau, octet pour octet');
const both = [...hist.readDay(day1), ...hist.readDay(day2)];
const round = unpackRecords(structuredClone(packRecords(both)));
ok(round.length === both.length && round.every((r, k) => JSON.stringify(r.b) === JSON.stringify(both[k].b) && JSON.stringify(r.n) === JSON.stringify(both[k].n)), 'tranches compactes de l\'aperçu : aller-retour exact');
ok(JSON.stringify(aggregate(round, { step: 180, sid: sidA })) === JSON.stringify(aggregate(both, { step: 180, sid: sidA })), 'mêmes courbes depuis les tranches compactes');
const d1 = aggregate(both, { step: 1440 });
ok(d1.t.length === 2 && luxParts(d1.t[0]).h === 0 && luxParts(d1.t[1]).h === 0, 'pas d\'un jour : de minuit à minuit, heure de Luxembourg');
console.log(`   ${captured} tranches écrites, ${(files.length)} fichiers`);

console.log('2. journal des prévisions : chaque quart d\'heure de 6 h à 22 h');
const p1 = fl.readDay(day1);
ok(p1.length === 64, `${day1} : 64 quarts d'heure attendus, ${p1.length} notés`);
ok(p1.every((p) => { const lp = luxParts(p.slot); return lp.mi % 15 === 0 && lp.h >= 6 && lp.h < 22; }), 'aux quarts d\'heure, de 6 h à 22 h');
let curBad = 0, curN = 0;
for (const p of p1) {
  const tr = truthBySlot.get(p.slot);
  p.st.forEach((v, i) => { if (!v) return; curN++; if (v[0] !== tr[p.sid[i]]) curBad++; });
}
ok(curN > 8000 && curBad === 0, `relevé de chaque prévision = la vérité (${curN} prévisions, ${curBad} écart(s))`);
ok(p1.every((p) => p.st.every((v) => !v || (v.length === 14 && v.every(Number.isInteger)))), 'prévisions arrondies, 14 valeurs par station');

console.log('3. note de la journée contre un calcul à la main');
const note = fl.score(day1);
ok(note.complete, 'journée complète (l\'historique couvre jusqu\'à 2 h le lendemain)');
// persistance à +60 min, recalculée depuis la vérité seule
let hs = 0, hn = 0, ms15 = 0, mn15 = 0;
for (const p of p1) {
  const later = truthBySlot.get(p.slot + 12 * 300), l15 = truthBySlot.get(p.slot + 3 * 300);
  p.st.forEach((v, i) => {
    if (!v) return;
    const sid = p.sid[i];
    if (later && later[sid] !== undefined) { hs += Math.abs(v[0] - later[sid]); hn++; }
    if (l15 && l15[sid] !== undefined) { ms15 += Math.abs(v[1] - l15[sid]); mn15++; }
  });
}
ok(note.per[2].n === hn && Math.abs(note.per[2].mae - hs / hn) < 1e-4, `persistance +60 : ${note.per[2].mae} contre ${(hs / hn).toFixed(4)} à la main (n ${note.per[2].n}/${hn})`);
ok(note.model[0].n === mn15 && Math.abs(note.model[0].mae - ms15 / mn15) < 1e-4, `modèle +15 : ${note.model[0].mae} contre ${(ms15 / mn15).toFixed(4)} à la main`);
ok(note.model.every((a) => a.n > 0 && Number.isFinite(a.mae)) && note.prof.every((a) => a.n > 0), 'les cinq horizons sont notés, trois colonnes');
ok(note.hours.length === 16 && note.stations.length > 100, 'erreur par heure et par station');
ok(note.alerts.emptyPred >= note.alerts.emptyHit && note.alerts.emptyReal >= note.alerts.emptyCaught, 'fiabilité des alertes « dans l\'heure » cohérente');
ok(fl.scorePending() >= 1 && fl.readNote(day1)?.complete, 'note définitive écrite');
const rep = fl.report(14);
ok(rep.days.length >= 2 && rep.total.per[2].n >= hn && rep.hardest.length > 0, 'rapport sur plusieurs jours');
ok(rep.days[0].model.length === HK.length, 'rapport : cinq horizons par jour');
console.log(`   MAE +60 : modèle ${note.model[2].mae} · persistance ${note.per[2].mae} · profil seul ${note.prof[2].mae} (${note.model[2].n} prévisions)`);

console.log('4. notifications : horloge et transport simulés');
let now = Date.UTC(2026, 2, 2, 9, 0, 0) / 1000;           // 10 h à Luxembourg
const posts = [];
const fakeFetch = async (url, opt) => { posts.push({ url, headers: opt.headers, body: JSON.parse(opt.body) }); return { ok: true, status: 200, text: async () => '' }; };
const NDIR = join(tmp, 'notif');
const n = new Notifier({ dataDir: (await import('node:fs')).mkdirSync(NDIR, { recursive: true }) || NDIR, fetchImpl: fakeFetch, now: () => now });
ok(!n.enabled, 'sans destination, rien n\'est envoyé');
n.update({ url: 'https://ntfy.sh/velohnav-essai', token: 'tk_secret', minEpisode: 15, maxPerHour: 3, hours: '7-22', link: 'http://umbrel.local:8790' });
ok(n.enabled && !JSON.stringify(n.publicConfig()).includes('tk_secret') && n.publicConfig().tokenSet, 'le jeton ntfy n\'est jamais renvoyé');
const tg = n.ntfyTarget('http://umbrel.local:2586/velo');
ok(tg.root === 'http://umbrel.local:2586/' && tg.topic === 'velo', 'cible ntfy auto-hébergée');
let threw = false; try { n.update({ url: 'ntfy.sh/x' }); } catch { threw = true; }
ok(threw, 'adresse sans http(s) refusée');
const mk = (sid, name, status, extra = {}) => ({ sid, name, active: true, live: true, status, bikes: status === 0 ? 0 : 8, docks: status === 4 ? 0 : 9,
  emptySince: 0, fullSince: 0, need: status === 0 ? 6 : status === 4 ? -5 : 0, fc60: 1, etaEmpty: -1, etaFull: -1, ...extra });
const sum = (age = 30) => ({ lastUpdate: now - age, fetchErrors: 0, feedFrozen: false });
let S = [mk('1', 'Gare Centrale', 0, { emptySince: now - 3600 }), mk('2', 'Hamilius', 2), mk('3', 'Knuedler', 2)];
await n.evaluate({ stations: S, summary: sum() });
ok(posts.length === 0, 'premier passage : les ruptures déjà en cours ne sont pas annoncées');
S[1] = mk('2', 'Hamilius', 0, { emptySince: now });
await n.evaluate({ stations: S, summary: sum() });
now += 10 * 60; await n.evaluate({ stations: S, summary: sum() });
ok(posts.length === 0, 'vide depuis 10 min : pas encore');
now += 6 * 60; await n.evaluate({ stations: S, summary: sum() });
ok(posts.length === 1 && posts[0].body.title === 'Station vide : Hamilius' && posts[0].body.priority === 4, 'vide depuis 16 min : une notification');
ok(posts[0].url === 'https://ntfy.sh/' && posts[0].body.topic === 'velohnav-essai' && posts[0].headers.Authorization === 'Bearer tk_secret', 'publication JSON ntfy, sujet et jeton');
ok(posts[0].body.click === 'http://umbrel.local:8790' && /apporter 6/.test(posts[0].body.message), 'lien vers la page et besoin indiqué');
now += 5 * 60; await n.evaluate({ stations: S, summary: sum() });
ok(posts.length === 1, 'pas de répétition pendant la même rupture');
S[1] = mk('2', 'Hamilius', 2);
now += 60; await n.evaluate({ stations: S, summary: sum() });
ok(posts.length === 2 && posts[1].body.title === 'Rétablie : Hamilius' && posts[1].body.priority === 2, 'rétablie, en priorité basse');
S[1] = mk('2', 'Hamilius', 4, { fullSince: now }); S[2] = mk('3', 'Knuedler', 0, { emptySince: now });
now += 20 * 60; await n.evaluate({ stations: S, summary: sum() });
ok(posts.length === 3 && posts[2].body.title === '2 stations en rupture' && posts[2].body.message.split('\n').length === 2, 'deux ruptures du même cycle : un seul message');
// plafond horaire (3) atteint : la suivante attend
S.push(mk('4', 'Glacis', 0, { emptySince: now }));
now += 16 * 60; await n.evaluate({ stations: S, summary: sum() });
ok(posts.length === 3, 'plafond horaire : la rupture suivante attend');
now += 45 * 60; await n.evaluate({ stations: S, summary: sum() });
ok(posts.length === 4 && /Glacis/.test(posts[3].body.title + posts[3].body.message), 'plafond libéré : la rupture en attente part');
// heures calmes
now = Date.UTC(2026, 2, 2, 22, 30, 0) / 1000;             // 23 h 30
S.push(mk('5', 'Bonnevoie', 0, { emptySince: now - 20 * 60 }));
await n.evaluate({ stations: S, summary: sum() });
ok(posts.length === 4, 'hors des heures réglées : rien');
// flux muet puis rétabli (envoyé même la nuit)
await n.evaluate({ stations: S, summary: sum(900) });
ok(posts.length === 5 && posts[4].body.title === 'Flux Vel\'OH! muet', 'flux muet signalé');
await n.evaluate({ stations: S, summary: sum(900) });
ok(posts.length === 5, 'flux muet : une seule fois');
await n.evaluate({ stations: S, summary: sum(20) });
ok(posts.length === 6 && posts[5].body.title === 'Flux Vel\'OH! rétabli', 'flux rétabli signalé');
ok((await n.device({ id: 'a1', took: '' })) === null && posts.length === 6, 'synchro sans reprise : rien');
await n.device({ id: 'a1', name: 'Bureau', took: 'profils, historique', fw: '1.3.0' });
ok(posts.length === 7 && posts[6].body.title === 'Sunton resynchronisé : Bureau', 'Sunton resynchronisé');
// résumé du matin
n.update({ digest: '07:30' });
now = Date.UTC(2026, 2, 3, 6, 29, 0) / 1000;              // 7 h 29
ok(!n.digestDue(), 'résumé : pas avant l\'heure');
now += 60;
ok(n.digestDue(), 'résumé : à l\'heure');
e.clock(t0 + 2 * 86400 + 6 * 3600, ms);
const dg = e.digest(e.localDay(t0));
ok(dg.recorded && /%/.test(dg.availText), 'résumé de la veille par le moteur (vue Service)');
const msg = await n.sendDigest({ digest: dg, note, stations: S, devices: [{ id: 'a1', name: 'Bureau', lastReport: now - 3600 }], dateLabel: 'mardi 3 mars' });
ok(posts.length === 8 && posts[7].body.title === 'VelohNav Ops · mardi 3 mars', 'résumé envoyé');
ok(/disponible/.test(msg.message) && /Prévision à \+1 h/.test(msg.message) && /En ce moment/.test(msg.message) && /Bureau/.test(msg.message), 'résumé : veille, prévision notée, état, écrans');
ok(!n.digestDue(), 'un seul résumé par jour');
const n2 = new Notifier({ dataDir: NDIR, fetchImpl: fakeFetch, now: () => now });
ok(n2.enabled && n2.recent.length === 8 && !n2.digestDue(), 'réglages et journal relus au redémarrage');

console.log('5. banc de nuit sur une archive simulée');
const BIN = join(ROOT, 'sim', 'bench_prevision');
if (!existsSync(BIN)) console.log('   (sim/bench_prevision absent : make -C sim bench_prevision — étape sautée)');
else {
  const W = join(tmp, 'monde');
  execFileSync(BIN, ['--jours', '4', '--court', '--exporter', W], { cwd: join(ROOT, 'sim'), stdio: 'ignore' });
  const dirs = snapshotDirs(join(W, 'archive'));
  ok(dirs.length === 4, `archive simulée : 4 instantanés (${dirs.length})`);
  const H3 = new History(join(tmp, 'banc'));
  const r = await importArchive({ dirs, history: H3, makeEngine: () => loadEngine(core, quiet) });
  ok(r.snaps === 4 && r.added >= 3 * 287 + 60, `archive versée dans l'historique (${r.added} tranches)`);
  const d0 = H3.list();
  ok(d0.length >= 4 && d0.every((x) => x.gz), 'jours de l\'archive : compressés');
  const again = await importArchive({ dirs, history: H3, makeEngine: () => loadEngine(core, quiet) });
  ok(again.added === 0, 'réimport : aucune tranche en double');
  const nb = new NightBench({ dataDir: join(tmp, 'banc'), archiveDir: join(W, 'archive'), bin: BIN, cwd: join(ROOT, 'sim') });
  const j = await nb.run('2026-03-06');
  ok(j.ok && j.code === 0, `banc lancé (code ${j.code})`);
  ok(j.rows.length >= 3 && j.rows[0].name === 'tous les jours' && j.rows[0].h.length === 5 && j.rows[0].h[2].every(Number.isFinite), 'tableau des MAE lu');
  ok(j.evalDays >= 2 && j.perDay.length >= 3 && j.verdicts.length >= 2 && j.span?.coverPct >= 95, 'jours, verdicts et couverture lus');
  ok(nb.latest()?.day === '2026-03-06' && /## 1\. Prévision/.test(nb.markdown('2026-03-06')), 'rapport Markdown et extrait gardés');
  console.log(`   banc : MAE +60 modèle ${j.rows[0].h[2][0]} / persistance ${j.rows[0].h[2][1]} sur ${j.evalDays} jours, ${j.durationS} s`);

  console.log('6. le vrai serveur : import automatique, API, jeton, notification de test vers un faux ntfy, banc à la demande');
  const got = [];
  const ntfy = createServer((req, res) => { let b = ''; req.on('data', (c) => (b += c)); req.on('end', () => { got.push({ url: req.url, auth: req.headers.authorization, body: JSON.parse(b || '{}') }); res.end('{}'); }); });
  await new Promise((r) => ntfy.listen(0, '127.0.0.1', r));
  const nport = ntfy.address().port;
  const SD = join(tmp, 'srv');
  cpSync(join(W, 'archive'), join(SD, 'archive'), { recursive: true });
  const port = 18790 + Math.floor(Math.random() * 1000);
  const srv = spawn(process.execPath, [join(HERE, '..', 'server.mjs')], { env: { ...process.env, VN_DATA: SD, VN_PORT: String(port), VN_HOST: '127.0.0.1', VN_TOKEN: 'jeton-test',
    VN_GBFS_BASE: 'http://127.0.0.1:9/gbfs', VN_WX_URL: 'http://127.0.0.1:9/wx', VN_BENCH_BIN: BIN, VN_BENCH_CWD: join(ROOT, 'sim'), VN_NIGHT_AT: '23:59' }, stdio: ['ignore', 'pipe', 'pipe'] });
  let slog = '';
  srv.stdout.on('data', (d) => { slog += d; }); srv.stderr.on('data', (d) => { slog += d; });
  const base = `http://127.0.0.1:${port}`;
  const H = { Authorization: 'Bearer jeton-test' };
  const call = async (path, opt = {}) => { const r = await fetch(base + path, { ...opt, headers: { ...H, ...(opt.headers || {}) } }); return { status: r.status, type: r.headers.get('content-type') || '', text: await r.text() }; };
  const until = async (fn, ms = 20000) => { const t1 = Date.now() + ms; while (Date.now() < t1) { try { if (await fn()) return true; } catch { /* pas encore */ } await new Promise((r) => setTimeout(r, 250)); } return false; };
  try {
    ok(await until(async () => JSON.parse((await call('/api/history')).text).days >= 4), 'archive versée dans l\'historique au premier démarrage');
    ok((await fetch(base + '/api/history')).status === 401, 'API protégée par le jeton');
    const ser = JSON.parse((await call('/api/history/series?days=28&step=60')).text);
    ok(ser.t.length >= 72 && ser.avail.some((v) => v > 0), `série horaire servie (${ser.t.length} points)`);
    const dly = JSON.parse((await call('/api/history/daily?days=28')).text);
    ok(dly.days.length >= 4, 'résumés par jour servis');
    const csvR = await call('/api/history/export.csv?days=28');
    ok(csvR.status === 200 && /text\/csv/.test(csvR.type) && csvR.text.split('\n').length > 900, 'export CSV');
    const bad = await call('/api/notify', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ url: 'ftp://x/y' }) });
    ok(bad.status === 400, 'adresse invalide refusée');
    const set = await call('/api/notify', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ url: `http://127.0.0.1:${nport}/velo-test`, token: 'tk_abc', digest: '' }) });
    const st = JSON.parse(set.text);
    ok(set.status === 200 && st.enabled && st.config.tokenSet && !set.text.includes('tk_abc'), 'réglages enregistrés, jeton ntfy masqué');
    const tr = JSON.parse((await call('/api/notify/test', { method: 'POST' })).text);
    ok(tr.ok && got.length === 1 && got[0].url === '/' && got[0].body.topic === 'velo-test' && got[0].auth === 'Bearer tk_abc' && got[0].body.title === 'VelohNav Ops : test', 'message de test reçu par ntfy (JSON, sujet, jeton)');
    const dg = JSON.parse((await call('/api/notify/digest', { method: 'POST' })).text);
    ok(dg.preview && /VelohNav Ops/.test(dg.message.title) && got.length === 1, 'aperçu du résumé du matin, sans envoi');
    ok(JSON.parse((await call('/api/notify')).text).recent.length === 1, 'journal des messages');
    ok(JSON.parse((await call('/api/bench/run', { method: 'POST' })).text).started, 'banc lancé à la demande');
    ok(await until(async () => JSON.parse((await call('/api/bench')).text).latest?.ok === true, 30000), 'rapport du banc publié');
    const md = await call('/api/bench/report.md');
    ok(md.status === 200 && /## 1\. Prévision/.test(md.text), 'rapport Markdown servi');
    const fc = JSON.parse((await call('/api/forecast?days=7')).text);
    ok(Array.isArray(fc.days) && fc.horizons.length === 5, 'rapport des prévisions servi');
  } finally {
    srv.kill('SIGTERM');
    await new Promise((r) => srv.on('exit', r));
    ntfy.close();
  }
  ok(/sauvegarde avant arrêt/.test(slog), 'arrêt propre');
  if (fails) console.log(slog.split('\n').slice(-15).join('\n'));
}
ok(parseBenchReport('# Banc sur données réelles — AUCUNE donnée exploitable\n').empty, 'rapport vide reconnu');

rmSync(tmp, { recursive: true, force: true });
console.log(`\n${checks - fails}/${checks} vérifications réussies${fails ? ` — ${fails} ÉCHEC(S)` : ''}`);
process.exit(fails ? 1 : 0);
