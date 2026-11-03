// ── Synchro de bout en bout : serveur en ligne ⇄ carte simulée ───────
// Sans Internet : un faux GBFS / Open-Meteo local (instantané docs/donnees), le vrai serveur
// (online/server.mjs) et la vraie synchro du firmware (src/core/sync.cpp, compilée dans sim/sync_sim
// avec curl pour transport). Vérifie :
//   1. une carte vierge reprend les trois fichiers du serveur et les relit (Model::load) ;
//   2. une carte plus apprise que le serveur lui envoie ses profils et ses jours, qu'il adopte
//      (rechargement, évènement « reset » pour les jumeaux) ;
//   3. la première carte, resynchronisée, reprend alors cet apprentissage ;
//   4. sans jeton, le serveur refuse (401) et la carte n'écrit rien.
// Prérequis : cd sim && make sync_sim.   Lancer : node online/test/sync.e2e.mjs
import { createServer } from 'node:http';
import { spawn, execFileSync } from 'node:child_process';
import { readFileSync, mkdtempSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadEngine, FEED } from '../public/vn-engine.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..', '..');
const D = join(ROOT, 'docs', 'donnees');
const SIM = join(ROOT, 'sim', 'sync_sim');
let fails = 0, checks = 0;
const ok = (c, msg) => { checks++; console.log(`  ${c ? 'ok   ' : 'ÉCHEC'} ${msg}`); if (!c) fails++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
if (!existsSync(SIM)) { console.log('sim/sync_sim absent : cd sim && make sync_sim'); process.exit(2); }

// ── Faux GBFS + météo ──
const info = readFileSync(join(D, 'station_information.json'), 'utf8');
const types = readFileSync(join(D, 'vehicle_types.json'), 'utf8');
const wx = readFileSync(join(D, 'weather.json'), 'utf8');
const st0 = JSON.parse(readFileSync(join(D, 'station_status.json'), 'utf8'));
let tick = 0;
function status(t) {
  tick++;
  for (const [i, s] of st0.data.stations.entries()) {
    const cap = s.num_vehicles_available + s.num_docks_available;
    const b = Math.max(0, Math.min(cap, s.num_vehicles_available + ((i + tick) % 3) - 1));
    s.num_vehicles_available = b; s.num_docks_available = cap - b;
    if (s.vehicle_types_available) s.vehicle_types_available = [{ vehicle_type_id: 'electrical', count: b }];
  }
  st0.last_updated = new Date(t * 1000).toISOString();
  return JSON.stringify(st0);
}
const mock = createServer((req, res) => {
  const body = req.url.includes('station_status') ? status(Math.floor(Date.now() / 1000))
    : req.url.includes('station_information') ? info : req.url.includes('vehicle_types') ? types : req.url.startsWith('/wx') ? wx : null;
  res.writeHead(body ? 200 : 404, { 'Content-Type': 'application/json' });
  res.end(body || '{}');
});
await new Promise((r) => mock.listen(0, '127.0.0.1', r));
const mockUrl = `http://127.0.0.1:${mock.address().port}`;

// ── Serveur en ligne (vrai processus) ──
const TOKEN = 'jeton-de-test';
const data = mkdtempSync(join(tmpdir(), 'vn-e2e-'));
const port = 18790 + Math.floor(Math.random() * 1000);
const srv = spawn(process.execPath, [join(ROOT, 'online', 'server.mjs')], {
  env: { ...process.env, VN_DATA: join(data, 'serveur'), VN_PORT: String(port), VN_HOST: '127.0.0.1', VN_TOKEN: TOKEN,
    VN_GBFS_BASE: `${mockUrl}/gbfs`, VN_WX_URL: `${mockUrl}/wx`, VN_ARCHIVE_HOURS: '' },
  stdio: ['ignore', 'pipe', 'pipe'],
});
let srvLog = '';
srv.stdout.on('data', (d) => { srvLog += d; });
srv.stderr.on('data', (d) => { srvLog += d; });
const base = `http://127.0.0.1:${port}`;
const api = async (p) => (await fetch(base + p, { headers: { Authorization: `Bearer ${TOKEN}` } })).json();
const cleanup = () => { srv.kill('SIGTERM'); mock.close(); };
process.on('exit', cleanup);

let s = null;
for (let k = 0; k < 60; k++) {
  await sleep(500);
  try { s = await (await fetch(`${base}/api/summary`)).json(); if (s.lastUpdate) break; } catch { /* démarrage */ }
}
console.log('serveur prêt');
ok(s && s.lastUpdate > 0 && s.stations === 143, `le serveur a relevé le faux flux (${s?.stations} stations)`);

function device(dir, token = TOKEN) {
  const out = execFileSync(SIM, [base, dir, ...(token ? [token] : [])], { encoding: 'utf8' });
  return Object.fromEntries(out.trim().split('\n').map((l) => { const i = l.indexOf(' '); return [l.slice(0, i), l.slice(i + 1)]; }));
}

console.log('1. carte vierge');
const devA = join(data, 'carteA');
mkdirSync(devA, { recursive: true });
let a = device(devA);
console.log(`   ${a.TEXT}`);
ok(a.RESULT === '3' && a.TOOK === '7', 'les trois fichiers adoptés');
ok(a.LOADED === '1' && +a.SAMPLES === s.learnSamples && +a.STATIONS === 143, `relus par Model::load : ${a.SAMPLES} relevés, ${a.STATIONS} stations`);

console.log('2. carte plus apprise que le serveur (trois jours)');
const devB = join(data, 'carteB');
mkdirSync(devB, { recursive: true });
{
  const e = await loadEngine(readFileSync(join(ROOT, 'online', 'public', 'vn-core.wasm')), { onLog: () => {} });
  const now = Math.floor(Date.now() / 1000);
  const t0 = now - 3 * 86400;
  e.clock(t0, 1000); e.init();
  e.ingest(FEED.TYPES, true, 200, types, 100, 0);
  e.ingest(FEED.INFO, true, 200, info, 100, 0);
  for (let t = t0; t < now - 120; t += 300) { e.clock(t, 1000); e.ingest(FEED.STATUS, true, 200, status(t), 100, 100); }
  e.clock(now - 120, 1000);
  e.save();
  for (const f of ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin']) writeFileSync(join(devB, f), e.getFile('/' + f));
  const es = e.summary();
  console.log(`   carte B : ${es.learnSamples} relevés, ${es.daysKept} jours`);
  const b = device(devB);
  console.log(`   ${b.TEXT}`);
  ok((+b.PUSHED & 1) && (+b.PUSHED & 4), 'profils et jours envoyés au serveur');
  await sleep(300);
  const s2 = await (await fetch(`${base}/api/summary`)).json();
  ok(s2.learnSamples >= es.learnSamples && s2.daysKept >= es.daysKept, `le serveur les a adoptés : ${s2.learnSamples} relevés, ${s2.daysKept} jours`);
  const ev = await api('/api/twin/events?since=0');
  ok(ev.reset || ev.events.some((x) => x.k === 'reset'), 'les jumeaux sont prévenus (évènement « reset »)');
  const devs = await api('/api/devices');
  ok(devs.some((d) => d.id === 'sunton-sim' && /adopté/.test(d.uploaded || '')), 'registre des écrans : envoi noté');

  console.log('3. la première carte reprend cet apprentissage');
  a = device(devA);
  console.log(`   ${a.TEXT}`);
  ok(+a.TOOK & 1, 'profils adoptés');
  ok(+a.SAMPLES >= es.learnSamples && +a.DAYS >= es.daysKept, `carte A : ${a.SAMPLES} relevés, ${a.DAYS} jours`);
}

console.log('4. sans jeton');
const devC = join(data, 'carteC');
mkdirSync(devC, { recursive: true });
const c = device(devC, '');
console.log(`   ${c.TEXT}`);
ok(c.RESULT === '-1' && /401/.test(c.TEXT) && c.LOADED === '0', 'refusé (401), rien écrit');

console.log(`\n${checks} vérifications, ${fails} échec(s)`);
if (fails) console.log(srvLog.split('\n').slice(-30).join('\n'));
cleanup();
process.exit(fails ? 1 : 0);
