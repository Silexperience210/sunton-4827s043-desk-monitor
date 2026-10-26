// ── Régulation : déclarations, recoupement avec le flux, plan de tournée ─
// Règle vérifiée ici avant tout : une intervention n'existe que DÉCLARÉE (un groupe de neuf cyclistes qui
// rend ses vélos d'un coup ne crée rien), le flux ne fait que recouper, et les deux restent côte à côte.
//  1. points collés (jamais de géocodage), réglages, tournées, chargement à bord par catégorie ;
//  2. déclarations : idempotentes (file hors connexion), bornes du chargement, règles station / atelier ;
//  3. recoupement : concordant, écart, non visible, hors flux, en attente — sur un journal à la minute bruité ;
//  4. indicateurs : délai de traitement d'une station vide (déclaré + mesuré), rien sans déclaration ;
//  5. plan : capacité et chargement respectés, marge d'erreur, deux camions, stabilité du prochain arrêt ;
//  6. le vrai serveur en HTTP : réglages, tournée, plan, déclaration, doublon, refus, fin.
// node online/test/regulation.test.mjs
import { mkdtempSync, rmSync, cpSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { spawn } from 'node:child_process';
import { Regulation, MinuteLog, checkEvent, parsePoint, haversineKm, CFG_DEFAULT } from '../lib/regulation.mjs';
import { planTours, measuredSpeed, errorProfile } from '../lib/planner.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
let fails = 0, checks = 0;
const ok = (c, msg) => { checks++; if (!c) { fails++; console.log(`  ÉCHEC : ${msg}`); } };
const throws = (fn, re, msg) => { try { fn(); ok(false, `${msg} (aucune erreur)`); } catch (e) { ok(!re || re.test(e.message), `${msg} : « ${e.message} »`); } };
const tmp = mkdtempSync(join(tmpdir(), 'vn-reg-'));
let clock = Date.UTC(2026, 10, 3, 8, 0, 0) / 1000;        // mardi 03/11/2026, 9 h à Luxembourg
const now = () => clock;

console.log('1. points, réglages, tournées');
ok(JSON.stringify(parsePoint('49.6106, 6.0507')) === '{"lat":49.6106,"lon":6.0507}', 'coordonnées collées');
ok(parsePoint('https://www.openstreetmap.org/#map=19/49.61071/6.05072')?.lat === 49.61071, 'lien OpenStreetMap');
ok(parsePoint('https://www.google.com/maps/@49.6107,6.0507,17z')?.lon === 6.0507, 'lien Google Maps avec coordonnées');
ok(parsePoint('rue des Romains, Bertrange') === null, 'une adresse seule n\'est PAS géocodée');
ok(parsePoint('48.85, 2.35') === null, 'point hors du Luxembourg refusé');
const reg = new Regulation({ dataDir: join(tmp, 'a'), now });
throws(() => reg.updateConfig({ atelier: { point: 'rue des Romains' } }), /non reconnu/, 'atelier sans coordonnées refusé');
reg.updateConfig({ trucks: [{ id: 'c1', name: 'Petit', capacity: 10 }, { id: 'c2', name: 'Grand', capacity: 25 }, { id: 'x', name: 'Faux', capacity: 0 }],
  regulators: [{ id: 'r1', name: 'Paul' }, { id: 'r2', name: '' }], atelier: { point: '49.6106, 6.0507' }, atelierStock: { ok: 8 } });
ok(reg.cfg.trucks.length === 2 && reg.cfg.regulators.length === 1, 'camions sans capacité et régulateurs sans nom écartés');
ok(reg.cfg.atelier.lat === 49.6106 && reg.cfg.atelier.address === 'rue des Romains, Bertrange', 'atelier : point déclaré, adresse gardée');
throws(() => reg.startTour({ device: 'd1', truck: 'c1', regulator: 'r1', load: { ok: 9, hs: 2 } }), /capacité/, 'chargement de départ au-delà de la capacité');
throws(() => reg.startTour({ device: 'd1', truck: 'zz', regulator: 'r1', load: {} }), /camion inconnu/, 'camion inconnu');
const t1 = reg.startTour({ id: 'tour-0001', device: 'd1', truck: 'c2', regulator: 'r1', load: { ok: 6, hs: 0 }, at: clock }).tour;
ok(t1.capacity === 25 && t1.regulatorName === 'Paul', 'tournée : camion du jour et régulateur');
ok(reg.startTour({ id: 'tour-0001', device: 'd1', truck: 'c2', regulator: 'r1', load: { ok: 6 } }).duplicate, 'tournée renvoyée : pas de doublon');
ok(reg.openTourOf('d1')?.id === 'tour-0001', 'tournée ouverte de l\'appareil');

console.log('2. déclarations');
const D = (o) => reg.declare({ tour: 'tour-0001', at: clock, ...o }, 'd1');
throws(() => D({ id: 'e-0001', kind: 'station', sid: '14', dropped: { ok: 7 } }), /négatif/, 'déposer plus que ce qu\'on a à bord');
throws(() => D({ id: 'e-0002', kind: 'station', sid: '14', dropped: { hs: 1 } }), /hors service en station/, 'pas de dépose de vélo HS en station');
throws(() => D({ id: 'e-0003', kind: 'atelier', took: { hs: 1 } }), /hors service à l'atelier/, 'pas de reprise de vélo HS à l\'atelier');
throws(() => D({ id: 'e-0004', kind: 'station', sid: '20', took: { ok: 18, hs: 2 } }), /capacité/, 'prendre au-delà de la capacité');
throws(() => D({ id: 'x', kind: 'station', sid: '20' }), /identifiant/, 'identifiant de déclaration trop court');
const r1 = D({ id: 'e-0010', kind: 'station', sid: '14', name: 'Knuedler', dropped: { ok: 6 } });
ok(r1.ok && r1.load.ok === 0, 'dépose déclarée : à bord 0');
ok(D({ id: 'e-0010', kind: 'station', sid: '14', dropped: { ok: 6 } }).duplicate && reg.loadOf('tour-0001').ok === 0, 'même déclaration renvoyée (file hors connexion) : comptée une fois');
clock += 600;
const r2 = D({ id: 'e-0011', kind: 'station', sid: '20', name: 'Glacis', took: { ok: 12, hs: 3 } });
ok(r2.load.ok === 12 && r2.load.hs === 3, 'prise + vélos HS : chargement par catégorie');
clock += 900;
const r3 = D({ id: 'e-0012', kind: 'atelier', dropped: { hs: 3 }, took: { ok: 5 } });
ok(r3.load.ok === 17 && r3.load.hs === 0 && reg.cfg.atelierStock.ok === 3, 'atelier : HS déchargés, neufs repris, stock déclaré décompté');
const r4 = D({ id: 'e-0013', kind: 'station', sid: '33', impossible: 'station inaccessible (travaux)' });
ok(r4.ok && r4.load.ok === 17, 'arrêt impossible : déclaré, sans effet sur le chargement');
throws(() => reg.declare({ id: 'e-0014', tour: 'tour-0001', kind: 'station', sid: '1', at: clock + 3600, dropped: { ok: 1 } }, 'd1'), /futur/, 'heure de l\'appareil dans le futur refusée');
const reg2 = new Regulation({ dataDir: join(tmp, 'a'), now });
ok(reg2.events.size === 4 && reg2.loadOf('tour-0001').ok === 17 && reg2.cfg.atelierStock.ok === 3, 'redémarrage : registre et chargement relus');

console.log('3. recoupement avec le flux (journal à la minute, clients qui bougent la station)');
let seed = 7;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
const ml = new MinuteLog();
const T = clock;
let b14 = 0, b20 = 15, h20 = 3, b9 = 5;
for (let t = T - 40 * 60; t <= T + 20 * 60; t += 60) {
  // Knuedler (14) : vide, dépose de 6 vélos de T−4 à T−2 min, puis des clients
  if (t >= T - 4 * 60 && t <= T - 2 * 60) b14 += 2;
  if (t > T && rnd() < 0.3) b14 = Math.max(0, b14 - 1);
  // Glacis (20) : clients au hasard, prise de 12 + 3 HS à T + 10 min ... déclarée 12, mais seulement 4 vus
  b20 = Math.max(0, Math.min(20, b20 + (rnd() < 0.2 ? -1 : rnd() < 0.2 ? 1 : 0)));
  // Station 9 : un GROUPE de neuf cyclistes rend ses vélos d'un coup (aucune déclaration)
  if (t === T - 10 * 60) b9 += 9;
  ml.push(t, [{ sid: '14', live: true, bikes: b14, bikesDis: 0 }, { sid: '20', live: true, bikes: b20, bikesDis: h20 }, { sid: '9', live: true, bikes: b9, bikesDis: 0 }]);
}
const ev = (o) => ({ kind: 'station', at: T, took: { ok: 0, hs: 0 }, dropped: { ok: 0, hs: 0 }, ...o });
ok(checkEvent(ev({ sid: '14', dropped: { ok: 6 } }), { minuteLog: ml, now: T + 5 * 60 }) === null, 'avant la fin de la fenêtre : en attente');
const c1 = checkEvent(ev({ sid: '14', dropped: { ok: 6 } }), { minuteLog: ml, now: T + 12 * 60 });
ok(c1.verdict === 'concordant' && c1.observed.ok === 6 && c1.declared.ok === 6, `dépose de 6 vue dans le flux : concordant (observé ${c1.observed?.ok})`);
const c2 = checkEvent(ev({ sid: '14', dropped: { ok: 12 } }), { minuteLog: ml, now: T + 12 * 60 });
ok(c2.verdict === 'ecart' && c2.declared.ok === 12 && c2.observed.ok === 6, 'déclaré 12, observé 6 : écart, les deux nombres rendus');
const c3 = checkEvent(ev({ sid: '99', dropped: { ok: 3 } }), { minuteLog: ml, now: T + 12 * 60 });
ok(c3.verdict === 'non-visible', 'station absente du flux : non visible (rien d\'inventé)');
ok(checkEvent({ kind: 'atelier', at: T, took: { ok: 5 }, dropped: { hs: 3 } }, { minuteLog: ml, now: T + 3600 }).verdict === 'hors-flux', 'atelier : hors flux');
ok(checkEvent(ev({ sid: '14', impossible: 'fermée' }), { minuteLog: ml, now: T + 3600 }).verdict === 'hors-flux', 'arrêt impossible : rien à recouper');
// le groupe de neuf : aucune intervention n'existe tant que personne ne la déclare
const before9 = [...reg.events.values()].filter((e) => e.sid === '9').length;
reg.onCycle([{ sid: '9', live: true, bikes: 14, bikesDis: 0 }], T + 60);
ok([...reg.events.values()].filter((e) => e.sid === '9').length === before9 && before9 === 0, 'neuf vélos rendus d\'un coup : aucune intervention créée (rien n\'est déduit du flux)');

console.log('4. indicateurs : délai de traitement d\'une station vide (historique au pas de 5 min)');
const fakeHist = {
  at(t) {   // Knuedler vide de T−40 min à T, puis 6 vélos
    if (t % 300) return null;
    const empty = t >= T - 40 * 60 && t < T;
    return { t, st: [['14', 'Knuedler', 20, 0, 0]], b: [empty ? 0 : 6] };
  },
};
const reg3 = new Regulation({ dataDir: join(tmp, 'b'), history: fakeHist, now });
reg3.updateConfig({ trucks: [{ id: 'c1', name: 'C', capacity: 20 }], regulators: [{ id: 'r1', name: 'Paul' }] });
reg3.startTour({ id: 'tour-0002', device: 'd2', truck: 'c1', regulator: 'r1', load: { ok: 10 }, at: T - 3600 });
reg3.declare({ id: 'e-0100', tour: 'tour-0002', kind: 'station', sid: '14', name: 'Knuedler', at: T, dropped: { ok: 6 } }, 'd2');
const k = reg3.kpi('2026-11-01', '2026-11-30');
ok(k.delays.n === 1 && k.delays.list[0].minutes >= 40 && k.delays.list[0].minutes <= 45, `délai mesuré : vide depuis ${k.delays.list[0]?.minutes} min à la dépose`);
ok(k.days.length === 1 && k.days[0].dropOk === 6 && k.days[0].pending === 1, 'jour : vélos déposés (déclaré), recoupement en attente');
ok(/déclaré/.test(k.classes.bikes) && /mesuré/.test(k.classes.delays), 'chaque indicateur dit sa classe');

console.log('5. plan de tournée');
// petit monde : A se vide (vide dans 20 min), B réserve pleine, C pleine maintenant, D tranquille
const mk = (sid, lat, lon, cap, curve, extra = {}) => ({ st: { sid, name: `S${sid}`, lat, lon, active: true, live: true, status: 2, ...extra }, c: [cap, Math.round(curve[0]), cap - Math.round(curve[0]), extra.dis || 0, 2, ...curve] });
const lin = (a, b) => Array.from({ length: 49 }, (_, k) => +(a + (b - a) * Math.min(1, k / 24)).toFixed(1));
const W = [
  mk('A', 49.6100, 6.1300, 20, lin(4, 0)),
  mk('B', 49.6110, 6.1320, 20, lin(18, 18)),
  mk('C', 49.6120, 6.1280, 15, lin(15, 15), { dis: 2 }),
  mk('D', 49.6000, 6.1000, 20, lin(10, 10)),
];
const stations = W.map((w) => w.st), curves = { t: clock, st: W.map((w) => w.c) };
const cfg = structuredClone(CFG_DEFAULT);
cfg.atelier = { name: 'Atelier', lat: 49.6106, lon: 6.0507 };
const pl = planTours({ now: clock, curves, stations, cfg, tours: [{ id: 'T1', truckName: 'Grand', capacity: 25, load: { ok: 0, hs: 0 }, position: { lat: 49.611, lon: 6.131 } }] });
const s1 = pl.plans[0].stops;
ok(s1.length >= 2 && s1.some((s) => s.sid === 'A' && s.drop.ok >= 2), `la station qui se vide reçoit au moins 2 vélos (marge d'erreur) : ${s1.map((s) => `${s.sid}:${s.take.ok ? '−' + s.take.ok : '+' + s.drop.ok}`).join(' ')}`);
const firstTake = s1.findIndex((s) => s.take.ok > 0), aIdx = s1.findIndex((s) => s.sid === 'A');
ok(firstTake >= 0 && firstTake < aIdx, 'camion vide : il prend d\'abord (réserve ou station pleine), dépose ensuite');
ok(s1.every((s) => s.loadAfter.ok >= 0 && s.loadAfter.hs >= 0 && s.loadAfter.ok + s.loadAfter.hs <= 25), 'chargement jamais négatif ni au-delà de la capacité');
ok(!s1.some((s) => s.sid === 'D'), 'la station tranquille n\'est pas visitée');
ok(s1.every((s) => s.kind !== 'station' || s.drop.hs === 0), 'jamais de vélo HS déposé en station');
ok(s1.find((s) => s.sid === 'C')?.take.hs === 2 || !s1.some((s) => s.sid === 'C'), 'à une station visitée, les vélos HS sont ramassés au passage');
ok(/ESTIMÉES/.test(pl.method) && /banc simulé/.test(pl.error), 'le plan dit qu\'il est estimé, et d\'où vient sa marge');
// deux camions : jamais la même station
const pl2 = planTours({ now: clock, curves, stations, cfg, tours: [
  { id: 'T1', truckName: 'Grand', capacity: 25, load: { ok: 10, hs: 0 }, position: null },
  { id: 'T2', truckName: 'Petit', capacity: 10, load: { ok: 0, hs: 0 }, position: null }] });
const sA = new Set(pl2.plans[0].stops.map((s) => s.sid)), sB = pl2.plans[1].stops.map((s) => s.sid).filter(Boolean);
ok(sB.every((x) => !sA.has(x)), 'deux camions : pas deux fois la même station');
// stabilité : le prochain arrêt annoncé ne change pas pour un gain marginal
const prev = new Map([['T1', { stops: [{ kind: 'station', sid: s1[0].sid }] }]]);
const again = planTours({ now: clock + 60, curves, stations, cfg, previous: prev, tours: [{ id: 'T1', truckName: 'Grand', capacity: 25, load: { ok: 0, hs: 0 }, position: { lat: 49.611, lon: 6.131 } }] });
ok(again.plans[0].stops[0].sid === s1[0].sid, 'prochain arrêt stable d\'un calcul à l\'autre');
// une station tout juste servie (déclarée il y a moins de 30 min) n'est pas reproposée,
// même si le flux public ne montre pas encore la dépose
const ex = planTours({ now: clock, curves, stations, cfg, exclude: new Set(['A']), tours: [{ id: 'T1', truckName: 'Grand', capacity: 25, load: { ok: 12, hs: 0 }, position: { lat: 49.611, lon: 6.131 } }] });
ok(!ex.plans[0].stops.some((s) => s.sid === 'A'), 'station déclarée servie il y a moins de 30 min : pas reproposée');
// marge : avec l'erreur mesurée (plus grande), la dépose grossit
const bigErr = errorProfile({ total: { model: [0.8, 1.4, 2.4, 3.2, 4.0].map((mae) => ({ n: 900, mae })) } });
ok(bigErr.measured && /mesurée/.test(bigErr.source), 'erreur mesurée utilisée dès 500 prévisions');
const partial = errorProfile({ total: { model: [[1136, 0.69], [994, 1.11], [710, 1.77], [142, 2.35], [0, null]].map(([n, mae]) => ({ n, mae })),
  per: [[1136, 0.59], [994, 1.02], [710, 1.73], [142, 2.35], [0, null]].map(([n, mae]) => ({ n, mae })) } });
ok(partial.measured && /\+2 h, \+4 h : banc simulé/.test(partial.source), `horizons peu mesurés : banc simulé, et c'est dit (${partial.source})`);
ok(partial.beats === false && /ne fait pas mieux/.test(partial.caution), 'prévision qui ne bat pas la persistance : avertissement dans le plan');
const pl3 = planTours({ now: clock, curves, stations, cfg, error: bigErr, tours: [{ id: 'T1', truckName: 'Grand', capacity: 25, load: { ok: 12, hs: 0 }, position: { lat: 49.611, lon: 6.131 } }] });
const dropSmall = planTours({ now: clock, curves, stations, cfg, tours: [{ id: 'T1', truckName: 'Grand', capacity: 25, load: { ok: 12, hs: 0 }, position: { lat: 49.611, lon: 6.131 } }] }).plans[0].stops.find((s) => s.sid === 'A')?.drop.ok;
const dropBig = pl3.plans[0].stops.find((s) => s.sid === 'A')?.drop.ok;
ok(dropBig >= dropSmall, `marge plus grande → dépose plus grande (${dropSmall} → ${dropBig})`);
// atelier : un camion chargé de HS y passe
const pl4 = planTours({ now: clock, curves, stations, cfg, tours: [{ id: 'T1', truckName: 'Petit', capacity: 10, load: { ok: 0, hs: 9 }, position: null }] });
ok(pl4.plans[0].stops.length === 0 || pl4.plans[0].stops.some((s) => s.kind === 'atelier') || pl4.plans[0].stops.every((s) => s.loadAfter.ok + s.loadAfter.hs <= 10), 'camion plein de vélos HS : capacité respectée (atelier si utile)');
// vitesse mesurée seulement avec assez de trajets déclarés
ok(!measuredSpeed([{ km: 2, min: 12, bikes: 6 }], cfg.plan).measured, 'moins de 20 trajets : vitesse supposée');
const many = Array.from({ length: 25 }, () => ({ km: 2, min: 4 + 3 + 9.3, bikes: 6 }));
const sp = measuredSpeed(many, cfg.plan);
ok(sp.measured && Math.abs(sp.kmh - 2 * 1.35 / (9.3 / 60)) < 0.5, `25 trajets : vitesse mesurée ${sp.kmh} km/h`);
ok(Math.abs(haversineKm({ lat: 49.6, lon: 6.1 }, { lat: 49.61, lon: 6.1 }) - 1.112) < 0.01, 'distance à vol d\'oiseau');

console.log('6. le vrai serveur en HTTP');
const SD = join(tmp, 'srv');
const archive = '/tmp/claude-0/simx/archive';
if (existsSync(archive)) cpSync(archive, join(SD, 'archive'), { recursive: true });
const port = 19790 + Math.floor(Math.random() * 500);
const srv = spawn(process.execPath, [join(HERE, '..', 'server.mjs')], { env: { ...process.env, VN_DATA: SD, VN_PORT: String(port), VN_HOST: '127.0.0.1', VN_TOKEN: 'jt',
  VN_GBFS_BASE: 'http://127.0.0.1:9/gbfs', VN_WX_URL: 'http://127.0.0.1:9/wx', VN_NIGHT_AT: '23:59' }, stdio: ['ignore', 'pipe', 'pipe'] });
let slog = ''; srv.stdout.on('data', (d) => (slog += d)); srv.stderr.on('data', (d) => (slog += d));
const base = `http://127.0.0.1:${port}`;
const call = async (path, body, dev = 'sunton-camion') => {
  const r = await fetch(base + path, { method: body ? 'POST' : 'GET', headers: { Authorization: 'Bearer jt', 'X-VN-Device': dev, ...(body ? { 'Content-Type': 'application/json' } : {}) }, body: body ? JSON.stringify(body) : undefined });
  return { status: r.status, j: await r.json().catch(() => ({})) };
};
const until = async (fn, ms = 15000) => { const t1 = Date.now() + ms; while (Date.now() < t1) { try { if (await fn()) return true; } catch { /* pas prêt */ } await new Promise((r) => setTimeout(r, 200)); } return false; };
try {
  ok(await until(async () => (await call('/reg/config')).status === 200), 'serveur prêt');
  ok((await fetch(base + '/reg/config')).status === 401, '/reg protégé par le jeton');
  const cf = await call('/reg/config', { trucks: [{ id: 'c1', name: 'Kangoo', capacity: 10 }, { id: 'c2', name: 'Master', capacity: 25 }], regulators: [{ id: 'r1', name: 'Paul' }], atelier: { point: '49.6106, 6.0507' } });
  ok(cf.status === 200 && cf.j.trucks.length === 2 && cf.j.atelier.lat === 49.6106, 'réglages : camions, régulateur, atelier');
  const t0 = await call('/reg/tour');
  ok(t0.status === 200 && t0.j.tour === null && t0.j.trucks.length === 2, 'appareil sans tournée : listes pour commencer');
  const bad = await call('/reg/tour/start', { truck: 'c1', regulator: 'r1', load: { ok: 11 } });
  ok(bad.status === 400 && /capacité/.test(bad.j.error), 'départ au-delà de la capacité refusé');
  const st = await call('/reg/tour/start', { id: 'tour-http-1', truck: 'c2', regulator: 'r1', load: { ok: 8, hs: 0 }, at: Math.floor(Date.now() / 1000) });
  ok(st.status === 200 && st.j.tour.capacity === 25 && st.j.load.ok === 8, 'tournée commencée');
  const tt = await call('/reg/tour');
  ok(tt.j.tour?.id === 'tour-http-1' && tt.j.plan && Array.isArray(tt.j.plan.stops) && /ESTIMÉES/.test(tt.j.method), 'l\'appareil reçoit sa tournée, son chargement et son plan');
  const e1 = await call('/reg/event', { id: 'ev-http-0001', tour: 'tour-http-1', kind: 'station', sid: '1001', name: 'Station 1001', at: Math.floor(Date.now() / 1000), dropped: { ok: 5 } });
  ok(e1.status === 200 && e1.j.load.ok === 3, 'déclaration acceptée, chargement à jour');
  const e1b = await call('/reg/event', { id: 'ev-http-0001', tour: 'tour-http-1', kind: 'station', sid: '1001', dropped: { ok: 5 } });
  ok(e1b.j.duplicate && e1b.j.load.ok === 3, 'renvoi de la même déclaration : doublon reconnu');
  const e2 = await call('/reg/event', { id: 'ev-http-0002', tour: 'tour-http-1', kind: 'station', sid: '1002', at: Math.floor(Date.now() / 1000), dropped: { ok: 9 } });
  ok(e2.status === 409 && /négatif/.test(e2.j.error), 'dépose impossible (pas assez à bord) refusée avec la raison');
  const other = await call('/reg/event', { id: 'ev-http-0003', tour: 'tour-http-1', kind: 'atelier', at: Math.floor(Date.now() / 1000), dropped: { hs: 0 }, took: { ok: 2 } }, 'telephone-paul');
  ok(other.status === 200 && other.j.event.device === 'telephone-paul', 'le téléphone de secours déclare dans la même tournée');
  const stt = await call('/reg/state?days=7');
  ok(stt.j.open.length === 1 && stt.j.events.length === 2 && stt.j.kpi.days.length >= 1, 'tableau de bord : tournée ouverte, déclarations, indicateurs');
  const end = await call('/reg/tour/end', { tour: 'tour-http-1', load: { ok: 5, hs: 0 } });
  ok(end.j.tour.endedAt > 0 && end.j.tour.loadEndComputed.ok === 5, 'fin de tournée : chargement déclaré et calculé côte à côte');
  ok((await call('/reg/tour')).j.tour === null, 'plus de tournée ouverte');
} finally {
  srv.kill('SIGTERM');
  await new Promise((r) => srv.on('exit', r));
}
if (fails) console.log(slog.split('\n').slice(-20).join('\n'));
rmSync(tmp, { recursive: true, force: true });
console.log(`\n${checks - fails}/${checks} vérifications réussies${fails ? ` — ${fails} ÉCHEC(S)` : ''}`);
process.exit(fails ? 1 : 0);
