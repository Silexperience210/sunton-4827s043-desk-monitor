// ── Sécurité du serveur : accès, CSRF, envois des cartes, notifications ─
// Vrais processus serveur (online/server.mjs) sur 127.0.0.1, flux et météo vers un port fermé, faux
// services locaux : rien ne sort de la machine. Chaque section rejoue un constat de la revue du 01/11/2026.
//  B1. écritures ouvertes sans jeton (défaut) et CSRF : le POST « simple » d'une page tierce doit être refusé ;
//      lectures publiques, synchro des cartes en lecture, jeton faux ou dans l'URL, banc et relevé sans jeton.
//  B2. profils forgés par un porteur du jeton (partagé par toutes les cartes) : refusés, le serveur garde les
//      siens ; un profil légitime plus appris est adopté, l'ancien gardé à côté, l'adresse de l'envoi journalisée.
//  M6. SSRF par /api/notify/test : le corps de la réponse n'est jamais renvoyé ; bouclage, lien local et adresses
//      non routables refusés (adresse littérale, nom résolu, redirection) ; faux services sur 127.0.0.1 seulement.
// node online/test/securite.test.mjs
import { mkdtempSync, rmSync, writeFileSync, chmodSync, mkdirSync, readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { req, startServer, simulateCard, fakeService } from './aides.mjs';
import { profHeader, daysHeader } from '../lib/sync.mjs';

let fails = 0, checks = 0;
// VN_TEST_DETAIL=1 : chaque vérification réussie est aussi affichée (preuves du rapport de correctifs)
const ok = (c, msg) => { checks++; if (!c) { fails++; console.log(`  ÉCHEC : ${msg}`); } else if (process.env.VN_TEST_DETAIL) console.log(`  ok : ${msg}`); };
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

  console.log('B2. profils forgés par un porteur du jeton : refusés ; profil légitime adopté, ancien gardé');
  const now = Math.floor(Date.now() / 1000);
  const srvState = await simulateCard({ from: now - 2 * 86400, to: now - 120, seed: 11 });
  mkdirSync(join(tmp, 'b2'), { recursive: true });
  for (const f of ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin']) writeFileSync(join(tmp, 'b2', f), srvState[f]);
  const s3 = await start('b2', { VN_TOKEN: 'jeton-test' });
  const auth = { authorization: 'Bearer jeton-test', 'x-vn-device': 'carte-b2' };
  const N0 = (await req(s3.port, { path: '/api/summary' })).json.learnSamples;
  ok(N0 === srvState.summary.learnSamples && N0 > 500, `état du serveur relu : ${N0} relevés appris`);
  const up = (name, body) => req(s3.port, { method: 'POST', path: `/sync/zz/upload/${name}`, headers: { ...auth, 'content-type': 'application/octet-stream' }, body });
  const P0 = srvState['vn_prof.bin'];
  const nSt = P0.readUInt16LE(6), cellsAt = 40 + nSt * 8, cellNAt = cellsAt + nSt * 672 * 4;
  const forge = (fn) => { const b = Buffer.from(P0); fn(b); return b; };
  // ajoute k relevés aux cellules déjà apprises (Σ cellN suit le compteur, comme un vrai apprentissage)
  const addCells = (b, k) => { for (let q = 0; k > 0; q = (q + 1) % 672) if (b[cellNAt + q] && b[cellNAt + q] < 250) { b[cellNAt + q]++; k--; } };
  const setHdr = (b, { since, samples, saved }) => {
    if (since !== undefined) b.writeBigInt64LE(BigInt(since), 16);
    if (samples !== undefined) b.writeUInt32LE(samples, 24);
    if (saved !== undefined) b.writeBigInt64LE(BigInt(saved), 32);
  };
  const h0 = profHeader(P0);
  const forged = [
    ['revue : 4 000 000 000 relevés, charge mise à zéro', forge((b) => { setHdr(b, { samples: 4000000000 }); b.fill(0, cellsAt, cellNAt + 672); }), /tranches/],
    ['0xFFFFFFFF relevés', forge((b) => setHdr(b, { samples: 0xFFFFFFFF })), /tranches/],
    ['début en 1970, compteur et cellules cohérents', forge((b) => { setHdr(b, { since: 1, samples: h0.learnSamples + 1000 }); addCells(b, 1000); }), /début/],
    ['sauvegardé demain, compteur et cellules cohérents', forge((b) => { setHdr(b, { saved: now + 86400, samples: h0.learnSamples + 288 }); addCells(b, 288); }), /futur/],
    ['en-tête seul : un jour de plus, cellules inchangées', forge((b) => setHdr(b, { since: h0.learnSince - 86400, samples: h0.learnSamples + 100 })), /compteur/],
    ['compteurs cohérents, cellules des stations vidées', forge((b) => {
      setHdr(b, { since: h0.learnSince - 86400, samples: h0.learnSamples + 288 }); addCells(b, 288); b.fill(0, cellsAt, cellNAt);
    }), /vide/],
    ['une station apprise plus souvent que le parc', forge((b) => {
      setHdr(b, { since: h0.learnSince - 86400, samples: h0.learnSamples + 288 }); addCells(b, 288);
      const q = [...Array(672).keys()].find((x) => b[cellNAt + x] >= 1); b[cellsAt + q * 4 + 2] = (b[cellsAt + q * 4 + 2] & 0xe0) | 31;
    }), /cellule/],
  ];
  for (const [what, body, why] of forged) {
    const r = await up('vn_prof.bin', body);
    ok(r.json?.adopted === false && why.test(r.json?.why || ''), `profil forgé (${what}) : ${r.status} ${JSON.stringify(r.json)}`);
  }
  const man = await req(s3.port, { path: '/sync/manifest', headers: auth });
  ok(man.json?.prof?.samples === N0, `manifeste après les envois forgés : ${man.json?.prof?.samples} relevés (le serveur garde ses ${N0})`);
  ok(!existsSync(join(tmp, 'b2', 'vn_prof.bin.avant-adoption')), 'aucune adoption : aucune copie');
  // résumés par jour datés dans le futur : une carte les croirait toujours plus récents
  const card = await simulateCard({ from: now - 3 * 86400, to: now - 60, seed: 12 });
  const dFut = Buffer.from(card['vn_days.bin']); dFut.writeBigInt64LE(BigInt(now + 30 * 86400), 32);
  const rd = await up('vn_days.bin', dFut);
  ok(rd.json?.adopted === false && /futur/.test(rd.json?.why || ''), `résumés par jour datés dans un mois : ${rd.status} ${JSON.stringify(rd.json)}`);
  // une carte légitime plus apprise (3 jours contre 2) : adoptée, l'ancien profil gardé, l'envoi journalisé avec son adresse
  const legit = await up('vn_prof.bin', card['vn_prof.bin']);
  ok(legit.status === 200 && legit.json?.adopted === true, `profil légitime plus appris : ${legit.status} ${JSON.stringify(legit.json)}`);
  const bak = join(tmp, 'b2', 'vn_prof.bin.avant-adoption');
  ok(existsSync(bak) && profHeader(readFileSync(bak))?.learnSamples === N0, 'profil du serveur gardé avant l\'adoption (vn_prof.bin.avant-adoption)');
  const legitD = await up('vn_days.bin', card['vn_days.bin']);
  ok(legitD.json?.adopted === true && existsSync(join(tmp, 'b2', 'vn_days.bin.avant-adoption')) && daysHeader(readFileSync(join(tmp, 'b2', 'vn_days.bin.avant-adoption')))?.nDays >= 1,
    `résumés légitimes adoptés, anciens gardés : ${JSON.stringify(legitD.json)}`);
  ok(/carte-b2 envoie vn_prof\.bin .*depuis 127\.0\.0\.1/.test(s3.log()), 'adresse de l\'envoi journalisée');

  console.log('M6. notification de test : aucune lecture de la réponse, adresses internes refusées');
  // faux service interne (127.0.0.1) : répond 418 avec un secret ; un second, cible d'une redirection
  const secret = 'REPONSE-SERVICE-INTERNE: admin_password=hunter2';
  const inner = await fakeService(() => ({ status: 418, type: 'text/plain', body: secret }));
  const hop = await fakeService(() => ({ status: 200, body: secret }));
  try {
    const H = { authorization: 'Bearer jeton-test', 'content-type': 'application/json' };
    const s4 = await start('m6', { VN_TOKEN: 'jeton-test' });
    const set = (port, body) => req(port, { method: 'POST', path: '/api/notify', headers: H, body: JSON.stringify(body) });
    for (const u of [`http://127.0.0.1:${inner.port}/interne/admin`, `http://2130706433:${inner.port}/`, `http://[::1]:${inner.port}/`, `http://[::ffff:127.0.0.1]:${inner.port}/`,
      `http://0.0.0.0:${inner.port}/`, 'http://169.254.169.254/latest/meta-data/', 'http://[fe80::1]/']) {
      const r = await set(s4.port, { webhook: u });
      ok(r.status === 400 && /refusée/.test(r.json?.error || ''), `webhook ${u} : ${r.status} ${r.json?.error || ''}`);
    }
    // nom qui se résout en 127.0.0.1 : refusé à la connexion (DNS), rien n'atteint le service
    const n0 = inner.got.length;
    ok((await set(s4.port, { webhook: `http://localhost:${inner.port}/interne/admin` })).status === 200, 'nom d\'hôte : accepté au réglage (la résolution a lieu à l\'envoi)');
    const t1 = await req(s4.port, { method: 'POST', path: '/api/notify/test', headers: H });
    ok(t1.json?.ok === false && /refusée/.test(t1.json?.why || '') && inner.got.length === n0, `localhost → 127.0.0.1 à l'envoi : ${JSON.stringify(t1.json)}, ${inner.got.length - n0} requête(s) reçue(s)`);
    await set(s4.port, { webhook: '' });
    // VN_NOTIFY_LOCAL=1 (ntfy sur la même machine) : bouclage permis, mais la réponse n'est jamais lue ni renvoyée
    const s5 = await start('m6-local', { VN_TOKEN: 'jeton-test', VN_NOTIFY_LOCAL: '1' });
    ok((await set(s5.port, { webhook: `http://127.0.0.1:${inner.port}/interne/admin` })).status === 200, 'VN_NOTIFY_LOCAL=1 : webhook en 127.0.0.1 accepté');
    const t2 = await req(s5.port, { method: 'POST', path: '/api/notify/test', headers: H });
    const st2 = await req(s5.port, { path: '/api/notify', headers: H });
    ok(inner.got.length === n0 + 1 && t2.json?.why === 'HTTP 418', `réponse du service interne : seul le code est rendu (${JSON.stringify(t2.json)})`);
    ok(!/hunter2|REPONSE/.test(t2.text + st2.text), 'le corps n\'apparaît ni dans la réponse ni dans le journal des messages');
    ok((await set(s5.port, { webhook: 'http://169.254.169.254/latest/meta-data/' })).status === 400, 'VN_NOTIFY_LOCAL=1 : lien local toujours refusé');
    // redirection vers un autre service : jamais suivie
    const r3 = await fakeService(() => ({ status: 302, body: '', headers: { location: `http://127.0.0.1:${hop.port}/secret` } }));
    await set(s5.port, { webhook: `http://127.0.0.1:${r3.port}/` });
    const t3 = await req(s5.port, { method: 'POST', path: '/api/notify/test', headers: H });
    ok(hop.got.length === 0 && t3.json?.why === 'HTTP 302', `redirection non suivie (${JSON.stringify(t3.json)}, ${hop.got.length} requête(s) au service visé)`);
    await r3.close();
  } finally { await inner.close(); await hop.close(); }
} finally {
  for (const s of servers) await s.stop();
}
if (fails) for (const s of servers) console.log(s.log().split('\n').slice(-8).join('\n'));
rmSync(tmp, { recursive: true, force: true });
console.log(`\n${checks} vérifications, ${fails} échec(s)`);
process.exit(fails ? 1 : 0);
