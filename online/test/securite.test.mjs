// ── Sécurité du serveur : accès, CSRF, envois des cartes, notifications ─
// Vrais processus serveur (online/server.mjs) sur 127.0.0.1, flux et météo vers un port fermé, faux
// services locaux : rien ne sort de la machine. Chaque section rejoue un constat de la revue du 01/11/2026.
//  B1. écritures ouvertes sans jeton (défaut) et CSRF : le POST « simple » d'une page tierce doit être refusé ;
//      lectures publiques, synchro des cartes en lecture, jeton faux ou dans l'URL, banc et relevé sans jeton.
// node online/test/securite.test.mjs
import { mkdtempSync, rmSync, writeFileSync, chmodSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { req, startServer } from './aides.mjs';

let fails = 0, checks = 0;
const ok = (c, msg) => { checks++; if (!c) { fails++; console.log(`  ÉCHEC : ${msg}`); } };
const tmp = mkdtempSync(join(tmpdir(), 'vn-securite-'));
const servers = [];
const start = async (name, env) => { const s = await startServer({ data: join(tmp, name), env }); servers.push(s); return s; };

// Banc factice : présent (le serveur le croit disponible) ; l'archive vide le fait répondre sans le lancer
const fakeBench = join(tmp, 'banc.sh');
writeFileSync(fakeBench, '#!/bin/sh\nexit 0\n');
chmodSync(fakeBench, 0o755);

const LAN = 'velohnav.lan:8790';            // un hôte du réseau local, pas 127.0.0.1
const EVIL = 'http://evil.example';

try {
  console.log('B1. écritures sans jeton (défaut) : fermées hors requête locale, CSRF refusé');
  const s0 = await start('sans-jeton', { VN_BENCH_BIN: fakeBench });
  const P = s0.port;
  const csrf = await req(P, { method: 'POST', path: '/api/notify', headers: { host: LAN, origin: EVIL, 'content-type': 'text/plain' },
    body: JSON.stringify({ webhook: 'http://127.0.0.1:9/interne/admin' }) });
  ok([401, 403, 415].includes(csrf.status), `POST /api/notify en text/plain depuis une page tierce : ${csrf.status} (401/403/415 attendu)`);
  const runs = await Promise.all([1, 2, 3].map(() => req(P, { method: 'POST', path: '/api/bench/run', headers: { host: LAN, origin: EVIL } })));
  ok(runs.every((r) => r.status === 403), `3 × POST /api/bench/run depuis une page tierce : ${runs.map((r) => r.status).join(', ')} (403 attendu)`);
  const lanJson = { host: LAN, 'content-type': 'application/json' };
  ok((await req(P, { method: 'POST', path: '/api/notify', headers: lanJson, body: '{"digest":""}' })).status === 403, 'réglages modifiés depuis le réseau local sans jeton : 403');
  ok((await req(P, { method: 'POST', path: '/api/refresh', headers: { host: LAN } })).status === 403, 'POST /api/refresh sans jeton : 403');
  ok((await req(P, { method: 'POST', path: '/api/bench/run', headers: { host: LAN } })).status === 403, 'POST /api/bench/run sans jeton : 403');
  ok((await req(P, { method: 'POST', path: '/sync/zz/upload/vn_prof.bin', headers: { host: LAN, 'content-type': 'application/octet-stream' }, body: Buffer.alloc(64) })).status === 403,
    'envoi de profil par une carte sans jeton : 403');
  ok((await req(P, { method: 'POST', path: '/api/notify/test', headers: { host: LAN } })).status === 403, 'POST /api/notify/test sans jeton : 403');
  // requête locale (bouclage + Host local) : permise, sauf rebinding DNS, proxy, origine tierce ou type « simple »
  const local = await req(P, { method: 'POST', path: '/api/notify', headers: { 'content-type': 'application/json' }, body: '{"digest":""}' });
  ok(local.status === 200 && local.json?.config?.digest === '', `réglages modifiés en local (127.0.0.1) : ${local.status}`);
  ok((await req(P, { method: 'POST', path: '/api/notify', headers: { origin: EVIL, 'content-type': 'application/json' }, body: '{}' })).status === 403,
    'navigateur du serveur sur une page tierce (Origin étrangère) : 403');
  ok((await req(P, { method: 'POST', path: '/api/notify', headers: { host: 'rebind.evil.example:8790', 'content-type': 'application/json' }, body: '{}' })).status === 403,
    'rebinding DNS (Host étranger sur 127.0.0.1) : 403');
  ok((await req(P, { method: 'POST', path: '/api/refresh', headers: { 'x-forwarded-for': '203.0.113.9' } })).status === 403, 'requête relayée par un proxy du même hôte : 403');
  ok((await req(P, { method: 'POST', path: '/api/notify', headers: { 'content-type': 'text/plain' }, body: '{}' })).status === 415, 'réglages en text/plain, même en local : 415');
  ok((await req(P, { method: 'POST', path: '/sync/zz/upload/vn_prof.bin', headers: { 'content-type': 'text/plain' }, body: 'x' })).status === 415, 'envoi de carte en text/plain : 415');
  const st = await req(P, { path: '/api/notify' });
  ok(st.status === 200 && st.json?.config?.webhook === '', 'le webhook de la page tierce n\'a pas été enregistré');
  // lectures : publiques ; synchro des cartes en lecture : ouverte sans jeton (réseau local), CSRF refusé
  ok((await req(P, { path: '/api/history', headers: { host: LAN } })).status === 200, 'lecture publique depuis le réseau local');
  ok(![401, 403].includes((await req(P, { path: '/sync/manifest', headers: { host: LAN } })).status), 'manifeste lisible par une carte sans jeton');
  ok((await req(P, { path: '/sync/zz/report?took=profils&result=1', headers: { host: LAN } })).status === 200, 'compte rendu d\'une carte sans jeton');
  ok((await req(P, { path: '/sync/zz/report?took=x&result=1', headers: { host: LAN, 'sec-fetch-site': 'cross-site' } })).status === 403,
    'compte rendu forgé par une image d\'une page tierce : 403');

  console.log('B1. VN_OPEN=1 : écritures ouvertes au réseau local, CSRF toujours refusé');
  const s1 = await start('ouvert', { VN_OPEN: '1' });
  ok((await req(s1.port, { method: 'POST', path: '/api/notify', headers: lanJson, body: '{"digest":""}' })).status === 200, 'réglages depuis le réseau local avec VN_OPEN=1');
  ok((await req(s1.port, { method: 'POST', path: '/api/notify', headers: { ...lanJson, origin: `http://${LAN}`, 'sec-fetch-site': 'same-origin' }, body: '{}' })).status === 200,
    'la page elle-même (même origine)');
  ok((await req(s1.port, { method: 'POST', path: '/api/notify', headers: { host: LAN, origin: EVIL, 'content-type': 'text/plain' }, body: '{}' })).status === 403,
    'page tierce, même avec VN_OPEN=1 : 403');
  ok((await req(s1.port, { method: 'POST', path: '/api/refresh', headers: { host: LAN, 'sec-fetch-site': 'cross-site' } })).status === 403, 'formulaire d\'un autre site : 403');

  console.log('B1. avec VN_TOKEN : jeton exigé, faux jeton refusé, jamais dans l\'URL');
  const s2 = await start('jeton', { VN_TOKEN: 'jeton-test', VN_BENCH_BIN: fakeBench });
  const Q = s2.port;
  ok((await req(Q, { path: '/api/notify', headers: { authorization: 'Bearer jeton-test' } })).status === 200, 'bon jeton (Bearer) : 200');
  ok((await req(Q, { path: '/api/notify', headers: { 'x-vn-token': 'jeton-test' } })).status === 200, 'bon jeton (X-VN-Token) : 200');
  ok((await req(Q, { path: '/api/notify', headers: { authorization: 'Bearer jeton-faux' } })).status === 401, 'faux jeton (Bearer) : 401');
  ok((await req(Q, { path: '/api/notify', headers: { authorization: 'Bearer jeton-tes' } })).status === 401, 'jeton tronqué : 401');
  ok((await req(Q, { path: '/api/notify', headers: { 'x-vn-token': 'jeton-faux' } })).status === 401, 'faux jeton (X-VN-Token) : 401');
  ok((await req(Q, { path: '/api/notify?token=jeton-test' })).status === 401, 'jeton dans l\'URL : 401');
  ok((await req(Q, { method: 'POST', path: '/api/bench/run' })).status === 401, 'POST /api/bench/run sans jeton : 401');
  ok((await req(Q, { method: 'POST', path: '/api/bench/run', headers: { authorization: 'Bearer jeton-faux' } })).status === 401, 'POST /api/bench/run, faux jeton : 401');
  ok((await req(Q, { method: 'POST', path: '/api/refresh' })).status === 401, 'POST /api/refresh sans jeton : 401');
  ok((await req(Q, { method: 'POST', path: '/api/refresh', headers: { 'x-vn-token': 'jeton-faux' } })).status === 401, 'POST /api/refresh, faux jeton : 401');
  ok((await req(Q, { path: '/sync/manifest', headers: { authorization: 'Bearer jeton-faux' } })).status === 401, 'synchro, faux jeton : 401');
  ok((await req(Q, { method: 'POST', path: '/api/bench/run', headers: { authorization: 'Bearer jeton-test' } })).status === 200, 'POST /api/bench/run avec le jeton : 200');
  ok((await req(Q, { method: 'POST', path: '/sync/zz/upload/vn_prof.bin', headers: { authorization: 'Bearer jeton-test', 'content-type': 'text/plain' }, body: 'x' })).status === 415,
    'envoi de carte en text/plain, même avec le jeton : 415');
  ok((await req(Q, { path: '/api/history' })).status === 200, 'lecture publique sans jeton');
} finally {
  for (const s of servers) await s.stop();
}
if (fails) for (const s of servers) console.log(s.log().split('\n').slice(-8).join('\n'));
rmSync(tmp, { recursive: true, force: true });
console.log(`\n${checks} vérifications, ${fails} échec(s)`);
process.exit(fails ? 1 : 0);
