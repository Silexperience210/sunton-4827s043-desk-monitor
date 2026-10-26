#!/usr/bin/env node
// ── VelohNav Ops en ligne : serveur ──────────────────────────────────
// Relève les flux Vel'OH! 24 h/24 avec le cœur du firmware (WebAssembly), garde ce qu'il apprend,
// sert le jumeau d'écran (l'UI de la carte, dans le navigateur) et l'état sur lequel les Sunton se
// resynchronisent au démarrage (après un week-end éteints, par exemple).
//
//   node online/server.mjs                       # relève, apprend, sert http://0.0.0.0:8790
//   node online/server.mjs --import <dossier>    # part de ce qu'une carte a déjà appris
//                                                # (vidage tools/carte_dump.py : vn_prof.bin…)
// Variables : voir online/lib/config.mjs et online/README.md. Aucune dépendance (Node ≥ 18).
import { createServer } from 'node:http';
import { readFileSync, existsSync, statSync } from 'node:fs';
import { join, extname, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gzipSync } from 'node:zlib';
import { performance } from 'node:perf_hooks';
import { CONFIG } from './lib/config.mjs';
import { loadEngine } from './public/vn-engine.js';
import { Collector, EventLog } from './lib/collector.mjs';
import { Store, STATE_FILES, CARD_FILES, luxParts } from './lib/store.mjs';
import { SyncHub, profHeader, daysHeader } from './lib/sync.mjs';
import { History, importArchive, snapshotDirs, dayOf } from './lib/history.mjs';
import { ForecastLog } from './lib/forecast.mjs';
import { Notifier } from './lib/notify.mjs';
import { NightBench } from './lib/bench.mjs';
import { Regulation } from './lib/regulation.mjs';
import { planTours, measuredSpeed, errorProfile } from './lib/planner.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const PUBLIC = join(HERE, 'public');
const started = Date.now();
const log = (m) => console.log(`${new Date().toISOString()} ${m}`);
const mono = () => 1000 + performance.now();
const nowS = () => Math.floor(Date.now() / 1000);

// ── Moteur ───────────────────────────────────────────────────────────
const CORE_WASM = readFileSync(join(PUBLIC, 'vn-core.wasm'));
const engine = await loadEngine(CORE_WASM, { onLog: (l) => log(`[moteur] ${l}`) });
engine.clock(nowS(), mono());
if (!engine.init()) { log('initialisation du moteur impossible'); process.exit(1); }
engine.settings({ refreshSec: CONFIG.refreshS });
const flushEngineLog = () => { const l = engine.logs(); if (l) for (const line of l.trim().split('\n')) log(line); };

const store = new Store(CONFIG.dataDir, log);
const events = new EventLog(CONFIG.eventsKept);
const history = new History(CONFIG.dataDir, log);
const forecasts = new ForecastLog(CONFIG.dataDir, history, log);
const scratchEngine = async () => loadEngine(CORE_WASM, { onLog: () => {} });
const notifier = new Notifier({ dataDir: CONFIG.dataDir, log });
const bench = new NightBench({ dataDir: CONFIG.dataDir, archiveDir: store.archiveDir, log });
const regulation = new Regulation({ dataDir: CONFIG.dataDir, history, log });

// ── Import d'un vidage de carte ──────────────────────────────────────
const argv = process.argv.slice(2);
const imp = argv.indexOf('--import');
if (imp >= 0) {
  const dir = argv[imp + 1];
  if (!dir || !existsSync(dir)) { log(`--import : dossier introuvable « ${dir} »`); process.exit(2); }
  if (store.has('vn_prof.bin') && !argv.includes('--force')) {
    log(`--import : ${store.dir} contient déjà un état appris ; ajouter --force pour le remplacer`);
    process.exit(2);
  }
  let n = 0;
  for (const f of CARD_FILES) {
    const p = join(dir, f);
    if (existsSync(p)) { engine.putFile('/' + f, readFileSync(p)); n++; }
  }
  engine.clock(nowS(), mono());
  const r = engine.load(false);
  flushEngineLog();
  if (!(r & 1) || !engine.save()) { log(`--import : rien de lisible dans ${dir} (${n} fichier(s))`); process.exit(1); }
  const p = profHeader(engine.getFile('/vn_prof.bin'));
  store.persistFrom(engine);
  log(`--import : ${n} fichier(s) relus · ${p.nSt} stations · ${p.learnSamples} relevés appris depuis ${new Date(p.learnSince * 1000).toISOString()} → ${store.dir}`);
  log('(l\'historique 24 h n\'est repris que s\'il a moins de 20 h, comme sur la carte)');
  if (!argv.includes('--serve')) process.exit(0);
}

// ── Import d'une archive d'instantanés dans l'historique long ────────
// node online/server.mjs --import-archive [dossier]   (défaut : <données>/archive ; vidages de cartes acceptés)
const ia = argv.indexOf('--import-archive');
if (ia >= 0) {
  const root = argv[ia + 1] && !argv[ia + 1].startsWith('--') ? argv[ia + 1] : store.archiveDir;
  const dirs = snapshotDirs(root);
  if (!dirs.length) { log(`--import-archive : aucun instantané (vn_hist.bin) sous ${root}`); process.exit(2); }
  await importArchive({ dirs, history, makeEngine: scratchEngine, log });
  if (!argv.includes('--serve')) process.exit(0);
}

// ── Reprise de l'état sur disque ─────────────────────────────────────
{
  const names = store.restoreInto(engine);
  engine.clock(nowS(), mono());
  const r = engine.load(true);
  flushEngineLog();
  const s = engine.summary();
  if (names.length) log(`[fs] état relu (${names.join(', ')}) : profils ${r & 1 ? 'oui' : 'non'}, état vivant ${r & 2 ? 'oui' : 'non'} · ${s.stations} stations · ${s.learnSamples} relevés appris · ${s.daysKept} jour(s) résumés`);
  else log(`[fs] aucun état dans ${store.dir} : apprentissage depuis zéro`);
  // l'anneau relu comble l'historique long depuis sa dernière tranche écrite
  const n = history.capture(engine);
  if (n) log(`[historique] ${n} tranche(s) reprises de l'anneau relu`);
}

// ── Sauvegarde, archive ──────────────────────────────────────────────
let lastSave = Date.now(), lastSaveOk = 0, cyclesSinceSave = 0;
function saveNow(why) {
  engine.clock(nowS(), mono());
  if (!engine.save()) return false;
  const bytes = store.persistFrom(engine);
  lastSave = Date.now(); lastSaveOk = nowS(); cyclesSinceSave = 0;
  log(`[fs] état sauvegardé (${why}, ${(bytes / 1024).toFixed(0)} Ko)`);
  return true;
}
let lastArchiveKey = '';
function maybeArchive() {
  if (!CONFIG.archiveHours.length) return;
  const t = nowS(), lp = luxParts(t), key = `${lp.y}${lp.mo}${lp.d}-${lp.h}`;
  if (!CONFIG.archiveHours.includes(lp.h) || lp.mi >= 10 || key === lastArchiveKey) return;
  const s = engine.summary();
  if (!s.lastUpdate) return;
  lastArchiveKey = key;
  engine.clock(t, mono());
  if (!engine.save()) return;
  const files = {};
  for (const f of CARD_FILES) { const b = engine.getFile('/' + f); if (b) files[f] = b; }
  files['vn_stations.csv'] = Buffer.from(engine.stationsCsv(), 'utf8');
  const dir = store.archive(engine, files, { t, fw: engine.version() });
  const pruned = store.pruneArchives(CONFIG.archiveKeepDays);
  log(`[archive] ${dir}${pruned ? ` · ${pruned} ancienne(s) supprimée(s)` : ''}`);
}

const collector = new Collector({
  engine, cfg: CONFIG, events, mono, log,
  onCycle: () => {
    flushEngineLog();
    cyclesSinceSave++;
    engine.clock(nowS(), mono());
    try { history.capture(engine); } catch (err) { log(`[historique] échec : ${err.message}`); }
    try { forecasts.capture(engine); } catch (err) { log(`[prévision] échec : ${err.message}`); }
    try { regulation.onCycle(engine.stations(), nowS()); } catch (err) { log(`[régulation] échec : ${err.message}`); }
    if (notifier.enabled) {
      notifier.evaluate({ stations: engine.stations(), summary: engine.summary(), alerts: engine.alerts(), refreshS: CONFIG.refreshS })
        .catch((err) => log(`[notif] échec : ${err.message}`));
    }
    if (Date.now() - lastSave >= CONFIG.savePeriodS * 1000) saveNow('période');
    try { maybeArchive(); } catch (err) { log(`[archive] échec : ${err.message}`); }
  },
});
const hub = new SyncHub({ engine, store, events, cfg: CONFIG, log, beforeSnapshot: () => engine.clock(nowS(), mono()) });

// ── Tâches de nuit et résumé du matin ────────────────────────────────
// À CONFIG.nightAt (heure de Luxembourg) : jours finis compressés, prévisions de la veille notées,
// banc natif sur l'archive. Le résumé du matin part à l'heure réglée dans les notifications.
let lastNightDay = '';
async function nightly(force = false) {
  const t = nowS(), lp = luxParts(t), day = `${lp.y}-${lp.mo}-${lp.d}`;
  const [hh, mm] = CONFIG.nightAt.split(':').map(Number);
  if (!force && (day === lastNightDay || lp.h * 60 + lp.mi < hh * 60 + mm)) return;
  lastNightDay = day;
  try { history.compressDone(day); forecasts.compressDone(day); } catch (err) { log(`[nuit] compression : ${err.message}`); }
  try { forecasts.scorePending(); } catch (err) { log(`[nuit] notes de prévision : ${err.message}`); }
  if (bench.available && !bench.read(day)) await bench.run(day).catch((err) => log(`[banc] échec : ${err.message}`));
}
async function morningDigest() {
  if (!notifier.digestDue()) return;
  engine.clock(nowS(), mono());
  const today = engine.localDay(nowS());
  try { forecasts.scorePending(); } catch { /* note partielle */ }
  const yday = dayOf(nowS() - 86400);
  const lp = luxParts(nowS());
  const label = new Intl.DateTimeFormat('fr-FR', { timeZone: 'Europe/Luxembourg', weekday: 'long', day: 'numeric', month: 'long' }).format(new Date());
  await notifier.sendDigest({ digest: engine.digest(today - 1), note: forecasts.readNote(yday), stations: engine.stations(),
    devices: Object.values(hub.devices), dateLabel: label || `${lp.d}/${lp.mo}` });
}
setInterval(() => {
  nightly().catch((err) => log(`[nuit] ${err.message}`));
  morningDigest().catch((err) => log(`[notif] résumé : ${err.message}`));
}, 30000).unref();
// au démarrage : ce qui n'a pas été noté pendant l'arrêt
setTimeout(() => { try { forecasts.scorePending(); history.compressDone(); forecasts.compressDone(); } catch (err) { log(`[prévision] ${err.message}`); } }, 5000).unref();

// ── Fichiers envoyés par une carte ───────────────────────────────────
// Contrôlés par le chemin de la carte (Model::load dans un moteur à part), puis adoptés seulement s'ils
// valent mieux que ceux du serveur : profils plus appris, ou plus de jours résumés. L'adoption recharge
// l'état du serveur (Model::load, comme un redémarrage) et demande aux jumeaux de le reprendre.
async function adoptUpload(name, body, crcHdr) {
  const crc = engine.crc32(body);
  if (crcHdr && parseInt(String(crcHdr), 16) >>> 0 !== crc) return { adopted: false, error: true, why: 'CRC32 faux' };
  // Un moteur neuf à chaque contrôle : rien ne reste d'un envoi précédent
  const scratch = await loadEngine(CORE_WASM, { onLog: () => {} });
  scratch.clock(nowS(), mono());
  scratch.init();
  scratch.putFile('/' + name, body);
  if (!(scratch.load(false) & 1)) return { adopted: false, error: true, why: 'fichier illisible par Model::load' };
  engine.clock(nowS(), mono());
  const cur = engine.summary();
  if (name === 'vn_prof.bin') {
    const h = profHeader(body);
    if (!(h.learnSamples > cur.learnSamples)) return { adopted: false, why: `serveur aussi appris (${cur.learnSamples} relevés contre ${h.learnSamples})` };
  } else {
    const h = daysHeader(body);
    if (!(h.nDays > cur.daysKept)) return { adopted: false, why: `serveur aussi complet (${cur.daysKept} jours contre ${h.nDays})` };
  }
  if (!engine.save()) return { adopted: false, error: true, why: 'état du serveur vide' };
  engine.putFile('/' + name, body);
  engine.load(true);
  flushEngineLog();
  saveNow(`adoption de ${name}`);
  hub.invalidate();                          // le prochain manifeste décrit l'état adopté
  collector.call({ k: 'reset', why: name });
  const after = engine.summary();
  return { adopted: true, why: name === 'vn_prof.bin' ? `profils adoptés : ${after.learnSamples} relevés` : `résumés adoptés : ${after.daysKept} jours` };
}
function readBody(req, max) {
  return new Promise((resolve, reject) => {
    const parts = []; let n = 0;
    req.on('data', (c) => { n += c.length; if (n > max) { req.destroy(); reject(new Error('corps trop gros')); } else parts.push(c); });
    req.on('end', () => resolve(Buffer.concat(parts)));
    req.on('error', reject);
  });
}

// ── HTTP ─────────────────────────────────────────────────────────────
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.wasm': 'application/wasm', '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml', '.png': 'image/png', '.ico': 'image/x-icon', '.webmanifest': 'application/manifest+json' };
const gzCache = new Map();

function send(req, res, code, body, type, extra = {}) {
  const buf = Buffer.isBuffer(body) ? body : Buffer.from(typeof body === 'string' ? body : JSON.stringify(body));
  const headers = { 'Content-Type': type || 'application/json; charset=utf-8', 'Cache-Control': 'no-store',
    'Access-Control-Allow-Origin': '*', 'Access-Control-Expose-Headers': 'X-VN-CRC32, X-VN-Snapshot', ...extra };
  const gz = buf.length > 1024 && /\bgzip\b/.test(req.headers['accept-encoding'] || '') && !/^application\/octet/.test(headers['Content-Type']);
  const out = gz ? gzipSync(buf) : buf;
  if (gz) headers['Content-Encoding'] = 'gzip';
  headers['Content-Length'] = out.length;
  res.writeHead(code, headers);
  res.end(req.method === 'HEAD' ? undefined : out);
}
const sendJson = (req, res, obj, code = 200) => send(req, res, code, JSON.stringify(obj));

function authorized(req, url) {
  if (!CONFIG.token) return true;
  const h = req.headers.authorization || '';
  return h === `Bearer ${CONFIG.token}` || req.headers['x-vn-token'] === CONFIG.token || url.searchParams.get('token') === CONFIG.token;
}

function serveStatic(req, res, path) {
  const rel = path === '/' ? 'index.html' : path.replace(/^\/+/, '');
  if (rel.includes('..')) return send(req, res, 400, 'chemin refusé', 'text/plain');
  const file = join(PUBLIC, rel);
  if (!existsSync(file) || !statSync(file).isFile()) return send(req, res, 404, 'introuvable', 'text/plain; charset=utf-8');
  const type = MIME[extname(file)] || 'application/octet-stream';
  const st = statSync(file);
  const key = `${file}:${st.mtimeMs}`;
  const acceptGz = /\bgzip\b/.test(req.headers['accept-encoding'] || '');
  let body = readFileSync(file), enc = null;
  if (acceptGz && /javascript|css|html|wasm|json|svg/.test(type)) {
    if (!gzCache.has(key)) gzCache.set(key, gzipSync(body, { level: 9 }));
    body = gzCache.get(key); enc = 'gzip';
  }
  res.writeHead(200, { 'Content-Type': type, 'Content-Length': body.length, 'Cache-Control': rel === 'index.html' ? 'no-cache' : 'max-age=300',
    ...(enc ? { 'Content-Encoding': enc } : {}) });
  res.end(req.method === 'HEAD' ? undefined : body);
}

function baseUrl(req) {
  const proto = req.headers['x-forwarded-proto'] || 'http';
  return `${proto}://${req.headers['x-forwarded-host'] || req.headers.host}`;
}

// ── Plans de tournée (lib/planner.mjs) : recalculés au plus toutes les 30 s, et après chaque déclaration ──
let planCache = null, errCache = null;
const prevPlans = new Map();
function computePlans(force = false) {
  const t = nowS();
  if (!force && planCache && t - planCache.at < 30) return planCache;
  engine.clock(t, mono());
  const stations = engine.stations();
  const coords = new Map(stations.map((s) => [s.sid, { lat: s.lat, lon: s.lon }]));
  const open = regulation.state().open;
  if (!errCache || t - errCache.at > 600) errCache = { at: t, v: errorProfile(forecasts.report(14)) };
  const tours = open.map((o) => ({ id: o.id, truckName: o.truckName, capacity: o.capacity, load: o.load, position: regulation.positionOf(o.id, coords) }));
  const speed = measuredSpeed(regulation.travelSamples(coords), regulation.cfg.plan);
  planCache = open.length ? planTours({ now: t, curves: engine.curves(), stations, tours, cfg: regulation.cfg, speed, previous: prevPlans, error: errCache.v, exclude: regulation.recentStops(t) })
    : { at: t, plans: [], atelierSet: Number.isFinite(regulation.cfg.atelier.lat), error: errCache.v.source };
  for (const p of planCache.plans) prevPlans.set(p.tour, p);
  return planCache;
}
const deviceOf = (req) => String(req.headers['x-vn-device'] || 'inconnu').replace(/[^\w.-]/g, '').slice(0, 40) || 'inconnu';
async function jsonBody(req) {
  try { return JSON.parse((await readBody(req, 256 << 10)).toString('utf8') || '{}'); } catch { return null; }
}

// ?from=AAAA-MM-JJ&to=AAAA-MM-JJ ou ?days=N (défaut : les 7 derniers jours de l'historique)
function dayRange(url) {
  const re = /^\d{4}-\d{2}-\d{2}$/;
  const last = history.range().to || dayOf(nowS());
  let to = re.test(url.searchParams.get('to') || '') ? url.searchParams.get('to') : last;
  const n = Math.min(400, Math.max(1, parseInt(url.searchParams.get('days') || '7', 10) || 7));
  const back = new Date(`${to}T12:00:00Z`); back.setUTCDate(back.getUTCDate() - (n - 1));
  let from = re.test(url.searchParams.get('from') || '') ? url.searchParams.get('from') : back.toISOString().slice(0, 10);
  if (from > to) [from, to] = [to, from];
  return { from, to };
}

function serverInfo() {
  return {
    name: CONFIG.name, uptimeS: Math.floor((Date.now() - started) / 1000), seq: events.seq, cycles: collector.cycles,
    refreshS: CONFIG.refreshS, lastSaveAt: lastSaveOk, archives: store.archives().length, dataDir: CONFIG.dataDir,
    tokenRequired: !!CONFIG.token, devices: Object.keys(hub.devices).length,
    history: history.range(), forecastsLogged: forecasts.lastSlot, notify: notifier.enabled, bench: bench.available,
  };
}

const server = createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  const p = url.pathname;
  try {
    if (req.method === 'OPTIONS') return send(req, res, 204, '', 'text/plain', { 'Access-Control-Allow-Headers': 'Authorization, Content-Type, X-VN-Token, X-VN-Device, X-VN-Fw, X-VN-Local', 'Access-Control-Allow-Methods': 'GET, POST' });
    if (p === '/healthz') {
      const s = engine.summary();
      const fresh = s.lastUpdate && nowS() - s.lastUpdate < 3 * CONFIG.refreshS + 60;
      return send(req, res, fresh ? 200 : 503, fresh ? 'ok' : 'flux en retard', 'text/plain; charset=utf-8');
    }
    // ── API publique (lecture) ──
    if (p === '/api/summary') { engine.clock(nowS(), mono()); return sendJson(req, res, { ...engine.summary(), server: serverInfo() }); }
    if (p === '/api/ring') return sendJson(req, res, engine.ring());

    // ── À partir d'ici : jeton si VN_TOKEN est défini ──
    if ((p.startsWith('/api/') || p.startsWith('/sync/')) && !authorized(req, url)) return sendJson(req, res, { error: 'jeton requis' }, 401);

    if (p === '/api/devices') return sendJson(req, res, Object.values(hub.devices).sort((a, b) => b.last - a.last));
    if (p === '/api/archive') return sendJson(req, res, store.archives());
    if (p === '/api/refresh' && req.method === 'POST') { collector.refreshReq = true; return sendJson(req, res, { ok: true }); }

    // ── Historique long (lib/history.mjs) ──
    if (p === '/api/history') return sendJson(req, res, { ...history.range(), list: history.list() });
    if (p === '/api/history/series' || p === '/api/history/daily' || p === '/api/history/export.csv') {
      const { from, to } = dayRange(url);
      if (p === '/api/history/series') {
        const step = [5, 15, 30, 60, 180, 1440].includes(+url.searchParams.get('step')) ? +url.searchParams.get('step') : 60;
        return sendJson(req, res, history.series({ from, to, step, sid: url.searchParams.get('sid') || null }));
      }
      if (p === '/api/history/daily') {
        engine.clock(nowS(), mono());
        return sendJson(req, res, { from, to, days: history.dailySummary(from, to), service: engine.days().filter((d) => d.date >= from && d.date <= to) });
      }
      const per = url.searchParams.get('stations') === '1';
      return send(req, res, 200, history.csv(from, to, per), 'text/csv; charset=utf-8',
        { 'Content-Disposition': `attachment; filename="velohnav-${per ? 'stations' : 'reseau'}-${from}_${to}.csv"` });
    }
    if (p === '/api/stations') { engine.clock(nowS(), mono()); return sendJson(req, res, engine.stations()); }

    // ── Erreur des prévisions (lib/forecast.mjs) et banc de nuit (lib/bench.mjs) ──
    if (p === '/api/forecast') {
      const days = Math.min(90, Math.max(1, parseInt(url.searchParams.get('days') || '14', 10) || 14));
      const r = forecasts.report(days);
      const names = Object.fromEntries(engine.stations().map((x) => [x.sid, x.name]));
      for (const x of [...r.hardest, ...r.easiest]) x.name = names[x.sid] || x.sid;
      return sendJson(req, res, r);
    }
    if (p === '/api/bench') {
      const list = bench.reports();
      return sendJson(req, res, { available: bench.available, running: bench.running, nightAt: CONFIG.nightAt, reports: list, latest: bench.latest(), inputs: bench.inputs().why || null });
    }
    if (p === '/api/bench/report.md') {
      const day = url.searchParams.get('day') || bench.reports().at(-1);
      const md = day && /^\d{4}-\d{2}-\d{2}$/.test(day) ? bench.markdown(day) : null;
      return md ? send(req, res, 200, md, 'text/markdown; charset=utf-8') : sendJson(req, res, { error: 'aucun rapport' }, 404);
    }
    if (p === '/api/bench/run' && req.method === 'POST') {
      if (!bench.available) return sendJson(req, res, { ok: false, why: 'banc absent (VN_BENCH_BIN)' }, 400);
      if (bench.running) return sendJson(req, res, { ok: false, why: 'banc déjà en cours' }, 409);
      bench.run(dayOf(nowS())).catch((err) => log(`[banc] échec : ${err.message}`));
      return sendJson(req, res, { ok: true, started: true });
    }

    // ── Notifications (lib/notify.mjs) ──
    if (p === '/api/notify' && req.method === 'GET') return sendJson(req, res, notifier.status());
    if (p === '/api/notify' && req.method === 'POST') {
      let patch;
      try { patch = JSON.parse((await readBody(req, 64 << 10)).toString('utf8') || '{}'); } catch { return sendJson(req, res, { error: 'JSON attendu' }, 400); }
      try { notifier.update(patch); } catch (err) { return sendJson(req, res, { error: err.message }, 400); }
      log('[notif] réglages modifiés depuis la page');
      return sendJson(req, res, notifier.status());
    }
    if (p === '/api/notify/test' && req.method === 'POST') return sendJson(req, res, await notifier.test());
    if (p === '/api/notify/digest' && req.method === 'POST') {
      engine.clock(nowS(), mono());
      const today = engine.localDay(nowS());
      const label = new Intl.DateTimeFormat('fr-FR', { timeZone: 'Europe/Luxembourg', weekday: 'long', day: 'numeric', month: 'long' }).format(new Date());
      const msg = notifier.buildDigest({ digest: engine.digest(today - 1), note: forecasts.readNote(dayOf(nowS() - 86400)) || forecasts.score(dayOf(nowS() - 86400)),
        stations: engine.stations(), devices: Object.values(hub.devices), dateLabel: label });
      const r = url.searchParams.get('send') === '1' ? await notifier.send(msg, { force: true, kind: 'resume' }) : { ok: true, preview: true };
      return sendJson(req, res, { ...r, message: msg });
    }

    // ── Jumeau d'écran : instantané + évènements ──
    if (p === '/api/twin/snapshot') {
      const s = hub.snapshot(0);
      if (!s) return sendJson(req, res, { error: 'aucun relevé pour l\'instant', retryS: 10 }, 503);
      const files = {};
      for (const [n, b] of Object.entries(s.files)) files[n] = b.toString('base64');
      return sendJson(req, res, { seq: s.seq, t: s.t, now: Date.now() / 1000, fw: engine.version(), name: CONFIG.name,
        refreshS: CONFIG.refreshS, fetching: collector.busy, files });
    }
    if (p === '/api/twin/events') {
      const since = parseInt(url.searchParams.get('since') || '0', 10);
      let ev = events.after(since);
      if (ev && ev.length === 0) { await events.wait(25000); ev = events.after(since); }
      if (ev === null) return sendJson(req, res, { reset: true, seq: events.seq, now: Date.now() / 1000 });
      return sendJson(req, res, { seq: events.seq, now: Date.now() / 1000, events: ev });
    }

    // ── Synchronisation des Sunton ──
    if (p === '/sync/manifest') {
      const s = hub.snapshot(60);
      if (!s) return sendJson(req, res, { error: 'aucun relevé pour l\'instant' }, 503);
      const m = hub.manifest(s, baseUrl(req));
      hub.seen(req, { lastManifest: nowS(), snapshot: s.id });
      return sendJson(req, res, m);
    }
    // Une carte envoie ce qu'elle a appris de plus que le serveur (core/sync : « partager l'apprentissage »)
    const up = p.match(/^\/sync\/([a-z0-9]+)\/upload\/(vn_prof\.bin|vn_days\.bin)$/);
    if (up && req.method === 'POST') {
      const body = await readBody(req, 4 << 20);
      const r = await adoptUpload(up[2], body, req.headers['x-vn-crc32']);
      const d = hub.seen(req, { lastUpload: nowS(), uploaded: `${up[2]} : ${r.adopted ? 'adopté' : 'gardé celui du serveur'}` });
      log(`[sync] ${d.id} envoie ${up[2]} (${(body.length / 1024).toFixed(0)} Ko) : ${r.adopted ? 'ADOPTÉ' : 'refusé'} — ${r.why}`);
      return sendJson(req, res, r, r.error ? 400 : 200);
    }
    const mf = p.match(/^\/sync\/([a-z0-9]+)\/(vn_prof\.bin|vn_hist\.bin|vn_days\.bin|report)$/);
    if (mf) {
      const s = hub.byId(mf[1]);
      if (mf[2] === 'report') {
        const d = hub.seen(req, { lastReport: nowS(), took: url.searchParams.get('took') || '', result: url.searchParams.get('result') || '' });
        log(`[sync] ${d.id} (${d.fw || '?'}) : ${d.result || '?'} · adopté : ${d.took || 'rien'}`);
        notifier.device(d).catch(() => {});
        return sendJson(req, res, { ok: true });
      }
      if (!s || !s.files[mf[2]]) return sendJson(req, res, { error: 'instantané expiré : relire /sync/manifest' }, 410);
      hub.seen(req, { lastFile: nowS() });
      return send(req, res, 200, s.files[mf[2]], 'application/octet-stream',
        { 'X-VN-CRC32': s.crc[mf[2]].toString(16).toUpperCase().padStart(8, '0'), 'X-VN-Snapshot': s.id,
          'Content-Disposition': `attachment; filename="${mf[2]}"` });
    }
    // Derniers fichiers de la carte, pour le banc ou un flash manuel
    const ex = p.match(/^\/api\/export\/(vn_prof\.bin|vn_hist\.bin|vn_days\.bin|vn_stations\.csv)$/);
    if (ex) {
      const s = hub.snapshot(60);
      if (!s) return sendJson(req, res, { error: 'aucun relevé pour l\'instant' }, 503);
      const body = ex[1] === 'vn_stations.csv' ? Buffer.from(engine.stationsCsv(), 'utf8') : s.files[ex[1]];
      return send(req, res, 200, body, ex[1].endsWith('.csv') ? 'text/csv; charset=utf-8' : 'application/octet-stream',
        { 'Content-Disposition': `attachment; filename="${ex[1]}"` });
    }
    // ── Régulation : tournées, déclarations, plans (lib/regulation.mjs, lib/planner.mjs) ──
    if (p.startsWith('/reg/')) {
      if (!authorized(req, url)) return sendJson(req, res, { error: 'jeton requis' }, 401);
      const dev = deviceOf(req);
      const fail = (err) => sendJson(req, res, { ok: false, error: err.message }, err.code || 400);
      if (p === '/reg/config' && req.method === 'GET') return sendJson(req, res, regulation.publicConfig());
      if (p === '/reg/config' && req.method === 'POST') {
        const b = await jsonBody(req);
        if (!b) return sendJson(req, res, { error: 'JSON attendu' }, 400);
        try { regulation.updateConfig(b); planCache = null; log('[régulation] réglages modifiés'); return sendJson(req, res, regulation.publicConfig()); } catch (err) { return fail(err); }
      }
      // Ce dont l'appareil du régulateur a besoin, en un appel : sa tournée ouverte, son chargement, son plan
      // (?tour= : un second appareil — le téléphone de secours — rejoint la tournée du camion)
      if (p === '/reg/tour' && req.method === 'GET') {
        const tid = url.searchParams.get('tour');
        const asked = tid ? regulation.tour(tid) : null;
        const tour = asked ? (asked.endedAt ? null : asked) : regulation.openTourOf(dev);
        const cfg = regulation.publicConfig();
        const openTours = regulation.state().open.map((o) => ({ id: o.id, truckName: o.truckName, regulatorName: o.regulatorName, device: o.device, startedAt: o.startedAt }));
        const base = { device: dev, trucks: cfg.trucks, regulators: cfg.regulators, atelier: cfg.atelier, atelierStock: cfg.atelierStock, openTours, now: nowS() };
        if (!tour) return sendJson(req, res, { ...base, tour: null });
        const plans = computePlans();
        const plan = plans.plans.find((x) => x.tour === tour.id) || null;
        const events = regulation.tourEvents(tour.id).slice(-8).reverse();
        return sendJson(req, res, { ...base, tour, load: regulation.loadOf(tour.id), plan, method: plans.method, events });
      }
      if (p === '/reg/tour/start' && req.method === 'POST') {
        const b = await jsonBody(req);
        if (!b) return sendJson(req, res, { error: 'JSON attendu' }, 400);
        try { const r = regulation.startTour({ ...b, device: dev }); planCache = null; return sendJson(req, res, { ok: true, ...r, load: regulation.loadOf(r.tour.id) }); } catch (err) { return fail(err); }
      }
      if (p === '/reg/tour/end' && req.method === 'POST') {
        const b = await jsonBody(req);
        if (!b) return sendJson(req, res, { error: 'JSON attendu' }, 400);
        try { const r = regulation.endTour(b); planCache = null; prevPlans.delete(b.tour); return sendJson(req, res, { ok: true, ...r }); } catch (err) { return fail(err); }
      }
      if (p === '/reg/event' && req.method === 'POST') {
        const b = await jsonBody(req);
        if (!b) return sendJson(req, res, { error: 'JSON attendu' }, 400);
        engine.clock(nowS(), mono());
        const st = b.sid ? engine.stations().find((x) => x.sid === String(b.sid)) : null;
        const state = st ? { bikes: st.bikes, docks: st.docks, dis: st.bikesDis, status: st.status, at: nowS() } : null;
        try {
          const r = regulation.declare(b, dev, state);
          if (!r.duplicate) {
            planCache = null;
            const e = r.event;
            log(`[régulation] ${regulation.tour(e.tour)?.regulatorName || e.regulator} · ${e.kind === 'atelier' ? 'atelier' : `${e.name || e.sid}`} : ${e.impossible ? `impossible (${e.impossible})` : `pris ${e.took.ok}+${e.took.hs} HS, déposé ${e.dropped.ok}${e.dropped.hs ? `+${e.dropped.hs} HS` : ''}`} · à bord ${r.load.ok}+${r.load.hs}`);
          }
          return sendJson(req, res, r);
        } catch (err) { return fail(err); }
      }
      if (p === '/reg/plan') return sendJson(req, res, computePlans(url.searchParams.get('force') === '1'));
      if (p === '/reg/state') {
        const st = regulation.state();
        // période qui finit aujourd'hui (pas au dernier jour de l'historique)
        const today = dayOf(nowS());
        const n = Math.min(400, Math.max(1, parseInt(url.searchParams.get('days') || '7', 10) || 7));
        const back = new Date(`${today}T12:00:00Z`); back.setUTCDate(back.getUTCDate() - (n - 1));
        const from = back.toISOString().slice(0, 10), to = today;
        return sendJson(req, res, { ...st, plans: computePlans(), events: regulation.eventsBetween(from, to).slice(0, 300), kpi: regulation.kpi(from, to) });
      }
      return sendJson(req, res, { error: 'introuvable' }, 404);
    }
    if (p.startsWith('/api/') || p.startsWith('/sync/')) return sendJson(req, res, { error: 'introuvable' }, 404);
    return serveStatic(req, res, p);
  } catch (err) {
    log(`[http] ${req.method} ${p} : ${err.stack || err}`);
    if (!res.headersSent) sendJson(req, res, { error: 'erreur interne' }, 500);
  }
});
server.requestTimeout = 60000;
server.headersTimeout = 30000;

server.listen(CONFIG.port, CONFIG.host, () => {
  log(`VelohNav Ops en ligne ${engine.version()} · http://${CONFIG.host}:${CONFIG.port} · données ${CONFIG.dataDir}` +
      `${CONFIG.token ? ' · jeton requis pour /sync et /api' : ''}`);
  collector.start();
  // premier démarrage avec une archive d'instantanés mais sans historique long : l'archive le remplit
  if (!history.files().length && store.archives().length) {
    importArchive({ dirs: snapshotDirs(store.archiveDir), history, makeEngine: scratchEngine, log })
      .catch((err) => log(`[historique] import de l'archive : ${err.message}`));
  }
});

let stopping = false;
function shutdown(sig) {
  if (stopping) return;
  stopping = true;
  log(`${sig} : sauvegarde avant arrêt`);
  collector.stop();
  try { saveNow('arrêt'); } catch (err) { log(`[fs] échec à l'arrêt : ${err.message}`); }
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(0), 3000).unref();
}
process.on('SIGINT', () => shutdown('SIGINT'));
process.on('SIGTERM', () => shutdown('SIGTERM'));
