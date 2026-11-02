// ── Aides des tests du serveur : vrai processus, requêtes brutes, faux services locaux ─
// Tout reste sur 127.0.0.1 : le flux Vel'OH! et la météo pointent vers un port fermé (ou un faux flux
// local), aucune requête ne sort de la machine.
import { spawn } from 'node:child_process';
import { request, createServer } from 'node:http';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadEngine, FEED } from '../public/vn-engine.js';

const HERE = dirname(fileURLToPath(import.meta.url));
export const SERVER = join(HERE, '..', 'server.mjs');

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export async function freePort() {
  const s = createServer();
  await new Promise((r) => s.listen(0, '127.0.0.1', r));
  const { port } = s.address();
  await new Promise((r) => s.close(r));
  return port;
}

// Requête HTTP brute : en-têtes libres (Host, Origin…), corps texte ou octets
export function req(port, { method = 'GET', path = '/', headers = {}, body = null } = {}) {
  return new Promise((resolve, reject) => {
    const h = { host: `127.0.0.1:${port}`, ...headers };
    if (body !== null && h['content-length'] === undefined) h['content-length'] = Buffer.byteLength(body);
    const r = request({ host: '127.0.0.1', port, method, path, headers: h, setHost: false }, (res) => {
      const parts = [];
      res.on('data', (c) => parts.push(c));
      res.on('end', () => {
        const buf = Buffer.concat(parts);
        const text = buf.toString('utf8');
        let json = null;
        try { json = JSON.parse(text); } catch { /* pas du JSON */ }
        resolve({ status: res.statusCode, headers: res.headers, buf, text, json });
      });
    });
    r.on('error', reject);
    if (body !== null) r.write(body);
    r.end();
  });
}

// Vrai serveur (online/server.mjs) sur un port libre, données dans `data`
export async function startServer({ data, env = {}, args = [] }) {
  const port = await freePort();
  const proc = spawn(process.execPath, [SERVER, ...args], {
    env: { ...process.env, VN_DATA: data, VN_PORT: String(port), VN_HOST: '127.0.0.1', VN_TOKEN: '', VN_OPEN: '',
      VN_GBFS_BASE: 'http://127.0.0.1:9/gbfs', VN_WX_URL: 'http://127.0.0.1:9/wx', VN_BENCH_BIN: '', VN_ARCHIVE_HOURS: '', ...env },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let log = '';
  proc.stdout.on('data', (d) => { log += d; });
  proc.stderr.on('data', (d) => { log += d; });
  const exited = new Promise((r) => proc.on('exit', (code) => r(code)));
  const srv = { port, proc, exited, log: () => log,
    async stop() { if (proc.exitCode === null) proc.kill('SIGTERM'); return exited; } };
  const t1 = Date.now() + 20000;
  while (Date.now() < t1) {
    if (proc.exitCode !== null) throw new Error(`le serveur s'est arrêté (code ${proc.exitCode}) :\n${log}`);
    try { if ((await req(port, { path: '/api/summary' })).status === 200) return srv; } catch { /* pas encore */ }
    await sleep(100);
  }
  await srv.stop();
  throw new Error(`le serveur ne répond pas :\n${log}`);
}

// Faux service HTTP local : enregistre chaque requête, répond ce que `reply` renvoie
export async function fakeService(reply = () => ({ status: 200, body: '{}' })) {
  const got = [];
  const s = createServer((rq, rs) => {
    const parts = [];
    rq.on('data', (c) => parts.push(c));
    rq.on('end', () => {
      const x = { method: rq.method, url: rq.url, headers: rq.headers, body: Buffer.concat(parts).toString('utf8') };
      got.push(x);
      const r = reply(x) || {};
      rs.writeHead(r.status || 200, { 'Content-Type': r.type || 'application/json', ...(r.headers || {}) });
      rs.end(r.body ?? '');
    });
  });
  await new Promise((r) => s.listen(0, '127.0.0.1', r));
  return { got, port: s.address().port, url: `http://127.0.0.1:${s.address().port}`, close: () => new Promise((r) => s.close(r)) };
}

// ── État appris d'une carte simulée : le parc réel de docs/donnees, une marche aléatoire au pas de 5 min ─
// de `from` à `to` (epoch), sauvegardé par le moteur : { 'vn_prof.bin', 'vn_hist.bin', 'vn_days.bin', 'vn_live.bin', summary }
const D = join(HERE, '..', '..', 'docs', 'donnees');
export const CORE = readFileSync(join(HERE, '..', 'public', 'vn-core.wasm'));
export async function simulateCard({ from, to, seed = 7, stepS = 300 }) {
  const info = readFileSync(join(D, 'station_information.json'), 'utf8');
  const types = readFileSync(join(D, 'vehicle_types.json'), 'utf8');
  const st0 = JSON.parse(readFileSync(join(D, 'station_status.json'), 'utf8'));
  let s = seed;
  const rnd = () => { s = (s * 1103515245 + 12345) & 0x7fffffff; return s / 0x7fffffff; };
  const e = await loadEngine(CORE, { onLog: () => {} });
  e.clock(from, 1000); e.init(); e.settings({ refreshSec: 60 });
  e.ingest(FEED.TYPES, true, 200, types, 100, 0);
  e.ingest(FEED.INFO, true, 200, info, 100, 0);
  let ms = 1000;
  for (let t = from; t <= to; t += stepS) {
    for (const x of st0.data.stations) {
      const cap = x.num_vehicles_available + x.num_docks_available;
      const b = Math.max(0, Math.min(cap, x.num_vehicles_available + Math.round((rnd() - 0.5) * 2)));
      x.num_vehicles_available = b; x.num_docks_available = cap - b;
      if (x.vehicle_types_available) x.vehicle_types_available = [{ vehicle_type_id: 'electrical', count: b }];
    }
    st0.last_updated = new Date(t * 1000).toISOString();
    ms += stepS * 1000;
    e.clock(t, ms);
    e.ingest(FEED.STATUS, true, 200, JSON.stringify(st0), 100, 100);
  }
  e.clock(to, ms);
  e.save();
  const out = { engine: e, summary: e.summary() };
  for (const f of ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin', 'vn_live.bin']) out[f] = Buffer.from(e.getFile('/' + f));
  return out;
}

// Faux flux GBFS local (instantané docs/donnees, last_updated à l'heure) : `status` = nombre de station_status servis
export async function fakeGbfs() {
  const info = readFileSync(join(D, 'station_information.json'), 'utf8');
  const types = readFileSync(join(D, 'vehicle_types.json'), 'utf8');
  const st0 = JSON.parse(readFileSync(join(D, 'station_status.json'), 'utf8'));
  const g = await fakeService((x) => {
    if (x.url.includes('station_status')) {
      g.status++;
      st0.last_updated = new Date().toISOString();
      return { body: JSON.stringify(st0) };
    }
    if (x.url.includes('station_information')) return { body: info };
    if (x.url.includes('vehicle_types')) return { body: types };
    return { status: 404, body: '{}' };
  });
  g.status = 0;
  g.base = `${g.url}/gbfs`;
  return g;
}
