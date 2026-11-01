// ── Aides des tests du serveur : vrai processus, requêtes brutes, faux services locaux ─
// Tout reste sur 127.0.0.1 : le flux Vel'OH! et la météo pointent vers un port fermé (ou un faux flux
// local), aucune requête ne sort de la machine.
import { spawn } from 'node:child_process';
import { request, createServer } from 'node:http';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

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
      rs.writeHead(r.status || 200, { 'Content-Type': r.type || 'application/json' });
      rs.end(r.body ?? '');
    });
  });
  await new Promise((r) => s.listen(0, '127.0.0.1', r));
  return { got, port: s.address().port, url: `http://127.0.0.1:${s.address().port}`, close: () => new Promise((r) => s.close(r)) };
}
