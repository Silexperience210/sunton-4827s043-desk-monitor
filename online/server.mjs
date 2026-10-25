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

// ── Reprise de l'état sur disque ─────────────────────────────────────
{
  const names = store.restoreInto(engine);
  engine.clock(nowS(), mono());
  const r = engine.load(true);
  flushEngineLog();
  const s = engine.summary();
  if (names.length) log(`[fs] état relu (${names.join(', ')}) : profils ${r & 1 ? 'oui' : 'non'}, état vivant ${r & 2 ? 'oui' : 'non'} · ${s.stations} stations · ${s.learnSamples} relevés appris · ${s.daysKept} jour(s) résumés`);
  else log(`[fs] aucun état dans ${store.dir} : apprentissage depuis zéro`);
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
    if (Date.now() - lastSave >= CONFIG.savePeriodS * 1000) saveNow('période');
    try { maybeArchive(); } catch (err) { log(`[archive] échec : ${err.message}`); }
  },
});
const hub = new SyncHub({ engine, store, events, cfg: CONFIG, log, beforeSnapshot: () => engine.clock(nowS(), mono()) });

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

function serverInfo() {
  return {
    name: CONFIG.name, uptimeS: Math.floor((Date.now() - started) / 1000), seq: events.seq, cycles: collector.cycles,
    refreshS: CONFIG.refreshS, lastSaveAt: lastSaveOk, archives: store.archives().length, dataDir: CONFIG.dataDir,
    tokenRequired: !!CONFIG.token, devices: Object.keys(hub.devices).length,
  };
}

const server = createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  const p = url.pathname;
  try {
    if (req.method === 'OPTIONS') return send(req, res, 204, '', 'text/plain', { 'Access-Control-Allow-Headers': 'Authorization, X-VN-Token, X-VN-Device, X-VN-Fw, X-VN-Local', 'Access-Control-Allow-Methods': 'GET, POST' });
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
