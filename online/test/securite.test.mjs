// ── Sécurité du serveur : accès, CSRF, envois des cartes, notifications ─
// Vrais processus serveur (online/server.mjs) sur 127.0.0.1, flux et météo vers un port fermé, faux
// services locaux : rien ne sort de la machine. Chaque section rejoue un constat de la revue du 01/11/2026.
//  B1. écritures ouvertes sans jeton (défaut) et CSRF : le POST « simple » d'une page tierce doit être refusé ;
//      lectures publiques, synchro des cartes en lecture, jeton faux ou dans l'URL, banc et relevé sans jeton.
//  B2. profils forgés par un porteur du jeton (partagé par toutes les cartes) : refusés, le serveur garde les
//      siens ; un profil légitime plus appris est adopté, l'ancien gardé à côté, l'adresse de l'envoi journalisée ;
//      un envoi dont le CRC32 annoncé (X-VN-CRC32) est faux est refusé (revue §5.3 : la mutation passait).
//  M6. SSRF par /api/notify/test : le corps de la réponse n'est jamais renvoyé ; bouclage, lien local et adresses
//      non routables refusés (adresse littérale, nom résolu, redirection) ; faux services sur 127.0.0.1 seulement.
//  m9. registre des écrans : 50 écrans au plus (le plus ancien laisse sa place), champs tronqués, devices.json
//      réécrit au plus toutes les 5 s (et à l'arrêt) au lieu d'à chaque requête.
//  m10. adresse de base du manifeste (où la carte lit les fichiers et envoie les siens, avec son jeton) :
//      X-Forwarded-Host / -Proto ignorés, sauf s'ils désignent l'adresse publique déclarée (VN_PUBLIC_URL).
//  c1. la colonne d'accès du tableau des routes (online/README.md, « ## API ») dit ce que fait le serveur : chaque
//      route listée est rejouée sans jeton sur un serveur qui en exige un (protégée ⇔ 401).
//  c2. aucune réponse publique ne donne le chemin du dossier de données (/api/summary le donnait : dataDir).
//  c5. la page porte une politique de sécurité du contenu (CSP : scripts du serveur seulement, aucun cadre) et
//      X-Frame-Options : une page tierce ne peut ni l'encadrer (détournement de clic) ni y injecter de script.
//  m12. la page ne lit jamais le jeton dans l'URL (?token= reste dans l'historique et les journaux du proxy) ;
//      un ancien lien qui en porte un est effacé de la barre d'adresse, sans être lu.
// node online/test/securite.test.mjs
import fs, { mkdtempSync, rmSync, writeFileSync, chmodSync, mkdirSync, readFileSync, existsSync, statSync, readdirSync } from 'node:fs';
import { syncBuiltinESMExports } from 'node:module';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { req, startServer, simulateCard, fakeService } from './aides.mjs';
import { profHeader, daysHeader, SyncHub, profImplausible } from '../lib/sync.mjs';
import { crc32 } from '../lib/store.mjs';
import { site } from './harnais.mjs';

let fails = 0, checks = 0;
// VN_TEST_DETAIL=1 : chaque vérification réussie est aussi affichée (preuves du rapport de correctifs)
const ok = (c, msg) => { checks++; site(); if (!c) { fails++; console.log(`  ÉCHEC : ${msg}`); } else if (process.env.VN_TEST_DETAIL) console.log(`  ok : ${msg}`); };
const tmp = mkdtempSync(join(tmpdir(), 'vn-securite-'));
const servers = [];
const start = async (name, env) => { const s = await startServer({ data: join(tmp, name), env }); servers.push(s); return s; };

// Banc factice : présent (le serveur le croit disponible) ; l'archive vide le fait répondre sans le lancer
const fakeBench = join(tmp, 'banc.sh');
writeFileSync(fakeBench, '#!/bin/sh\nexit 0\n');
chmodSync(fakeBench, 0o755);

const PUB = join(dirname(fileURLToPath(import.meta.url)), '..', 'public');
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

  console.log('c2. lectures publiques : le chemin du dossier de données n\'apparaît nulle part');
  {
    const dir = join(tmp, 'sans-jeton');
    const leaks = [];
    for (const path of ['/', '/healthz', '/api/summary', '/api/ring', '/api/archive', '/api/history', '/api/history/series', '/api/history/daily',
      '/api/stations', '/api/forecast', '/api/bench', '/api/twin/snapshot']) {
      const r = await req(P, { path });
      if (r.text.includes(dir)) leaks.push(`${path} (${r.text.match(/"(\w+)":"[^"]*sans-jeton[^"]*"/)?.[1] || 'champ ?'})`);
    }
    ok(!leaks.length, `routes publiques qui donnent ${dir} : ${leaks.join(', ') || 'aucune'}`);
  }

  console.log('c5. la page : CSP et X-Frame-Options');
  {
    const r = await req(P, { path: '/' });
    const csp = String(r.headers['content-security-policy'] || '');
    const dir = Object.fromEntries(csp.split(';').map((d) => d.trim().split(/\s+/)).filter((d) => d[0]).map(([k, ...v]) => [k, v]));
    ok(r.status === 200 && /text\/html/.test(r.headers['content-type']), `GET / : ${r.status} ${r.headers['content-type']}`);
    ok(dir['frame-ancestors']?.join(' ') === "'none'" && r.headers['x-frame-options'] === 'DENY',
      `aucun cadre : frame-ancestors ${dir['frame-ancestors']?.join(' ') || 'absent'}, X-Frame-Options ${r.headers['x-frame-options'] || 'absent'}`);
    ok(dir['script-src']?.includes("'self'") && !dir['script-src'].some((v) => ["'unsafe-inline'", "'unsafe-eval'"].includes(v) || /\*|https?:|data:|blob:/.test(v)) && dir['object-src']?.join(' ') === "'none'",
      `scripts du serveur seulement : script-src ${dir['script-src']?.join(' ') || 'absent'}, object-src ${dir['object-src']?.join(' ') || 'absent'}`);
    ok(dir['connect-src']?.join(' ') === "'self'" && dir['base-uri']?.join(' ') === "'none'", `connect-src ${dir['connect-src']?.join(' ') || 'absent'}, base-uri ${dir['base-uri']?.join(' ') || 'absent'}`);
  }

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

  console.log('c1. README : l\'accès de chaque route du tableau « API » est celui que le serveur applique');
  {
    const md = readFileSync(join(PUB, '..', 'README.md'), 'utf8');
    const table = md.split(/^## API$/m)[1]?.split(/^## /m)[0] || '';
    const routes = [];
    for (const line of table.split('\n')) {
      const cells = line.split('|').map((c) => c.trim());
      if (cells.length < 4 || !cells[1].startsWith('`')) continue;
      const doc = /^(oui|protégé)$/.test(cells[2]) ? true : /^(—|-|public)$/.test(cells[2]) ? false : null;
      // « `GET`/`POST /api/notify` » : méthodes cumulées ; « `/api/bench/report.md` » seule : méthode de la précédente
      let pending = [], last = ['GET'];
      for (const [, spec] of cells[1].matchAll(/`([^`]+)`/g)) {
        const m = spec.match(/^(GET|POST|HEAD)?\s*(\/\S*)?$/);
        if (!m || (!m[1] && !m[2])) continue;
        if (m[1] && !m[2]) { pending.push(m[1]); continue; }
        const methods = m[1] ? [...pending, m[1]] : last;
        pending = []; last = methods;
        const path = m[2].replace(/<id>/g, 'zz').replace('vn_*.bin', 'vn_prof.bin').replace(/\[[^\]]*\]/g, '');
        for (const method of methods) routes.push({ method, path, doc, cell: cells[2] });
      }
    }
    ok(routes.length >= 25, `${routes.length} routes lues dans le tableau du README`);
    const wrong = [];
    for (const r of routes) {
      const got = (await req(Q, { method: r.method, path: r.path, headers: r.method === 'POST' ? { 'content-type': 'application/json' } : {}, body: r.method === 'POST' ? '{}' : null })).status;
      if (r.doc === null || r.doc !== (got === 401)) wrong.push(`${r.method} ${r.path} : « ${r.cell} » dans le README, ${got} sans jeton`);
    }
    ok(!wrong.length, `accès documenté = accès appliqué${wrong.length ? ` — ${wrong.length} écart(s) :\n      ${wrong.join('\n      ')}` : ''}`);
  }

  console.log('B2. profils forgés par un porteur du jeton : refusés ; profil légitime adopté, ancien gardé');
  const now = Math.floor(Date.now() / 1000);
  const srvState = await simulateCard({ from: now - 2 * 86400, to: now - 120, seed: 11 });
  mkdirSync(join(tmp, 'b2'), { recursive: true });
  for (const f of ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin']) writeFileSync(join(tmp, 'b2', f), srvState[f]);
  const s3 = await start('b2', { VN_TOKEN: 'jeton-test' });
  const auth = { authorization: 'Bearer jeton-test', 'x-vn-device': 'carte-b2' };
  const N0 = (await req(s3.port, { path: '/api/summary' })).json.learnSamples;
  ok(N0 === srvState.summary.learnSamples && N0 > 500, `état du serveur relu : ${N0} relevés appris`);
  const up = (name, body, extra = {}) => req(s3.port, { method: 'POST', path: `/sync/zz/upload/${name}`, headers: { ...auth, 'content-type': 'application/octet-stream', ...extra }, body });
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
  // /vn_prof.bin version 5 (feat/modele-v2, 1.4.0) = version 4 + FcLearn (13 520 octets) en fin de fichier : ce que le
  // contrôle lit est au même endroit. Refusée comme « inconnue », toute carte 1.4.0 le serait après la fusion.
  const v5 = (b) => { const x = Buffer.concat([b, Buffer.alloc(13520)]); x.writeUInt16LE(5, 4); return x; };
  const w5 = profImplausible(v5(P0), now), f5 = profImplausible(v5(forged[0][1]), now);
  ok(w5 === null, `profil v5 (v4 + 13 520 octets) légitime : ${w5 || 'plausible'}`);
  ok(/tranches/.test(f5 || ''), `profil v5 forgé (4 000 000 000 relevés) : ${f5}`);
  const v6 = Buffer.from(P0); v6.writeUInt16LE(6, 4);
  ok(/version 6 inconnue/.test(profImplausible(v6, now) || ''), `version 6 : ${profImplausible(v6, now)}`);
  const man = await req(s3.port, { path: '/sync/manifest', headers: auth });
  ok(man.json?.prof?.samples === N0, `manifeste après les envois forgés : ${man.json?.prof?.samples} relevés (le serveur garde ses ${N0})`);
  ok(!existsSync(join(tmp, 'b2', 'vn_prof.bin.avant-adoption')), 'aucune adoption : aucune copie');
  // résumés par jour datés dans le futur : une carte les croirait toujours plus récents
  const card = await simulateCard({ from: now - 3 * 86400, to: now - 60, seed: 12 });
  const dFut = Buffer.from(card['vn_days.bin']); dFut.writeBigInt64LE(BigInt(now + 30 * 86400), 32);
  const rd = await up('vn_days.bin', dFut);
  ok(rd.json?.adopted === false && /futur/.test(rd.json?.why || ''), `résumés par jour datés dans un mois : ${rd.status} ${JSON.stringify(rd.json)}`);
  // revue §5.3 : le CRC32 annoncé par la carte (X-VN-CRC32, src/net/net_esp32.cpp) est vérifié — un fichier abîmé
  // en route n'est jamais adopté, même plausible et plus appris
  const crcHex = (b, flip = 0) => ((crc32(b) ^ flip) >>> 0).toString(16).toUpperCase().padStart(8, '0');
  const badCrc = await up('vn_prof.bin', card['vn_prof.bin'], { 'x-vn-crc32': crcHex(card['vn_prof.bin'], 1) });
  ok(badCrc.status === 400 && badCrc.json?.adopted === false && /CRC32/.test(badCrc.json?.why || ''), `profil légitime, CRC32 annoncé faux : ${badCrc.status} ${JSON.stringify(badCrc.json)}`);
  ok((await req(s3.port, { path: '/api/summary' })).json.learnSamples === N0, 'CRC32 faux : l\'état du serveur n\'a pas bougé');
  // une carte légitime plus apprise (3 jours contre 2), CRC32 juste : adoptée, l'ancien profil gardé, l'envoi journalisé avec son adresse
  const legit = await up('vn_prof.bin', card['vn_prof.bin'], { 'x-vn-crc32': crcHex(card['vn_prof.bin']) });
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

  console.log('m9. registre des écrans : 50 au plus, champs tronqués, écritures espacées');
  {
    const s6 = await start('m9', {});
    const long = 'x'.repeat(6000);
    for (let k = 0; k < 300; k += 50) {
      await Promise.all(Array.from({ length: 50 }, (_, j) => req(s6.port, { path: `/sync/zz/report?took=${long}&result=${long}`, headers: { 'x-vn-device': `ecran-${k + j}` } })));
    }
    const list = (await req(s6.port, { path: '/api/devices' })).json || [];
    ok(list.length <= 50, `300 écrans annoncés : ${list.length} gardés (50 au plus)`);
    ok(list.some((d) => d.id === 'ecran-299'), 'le plus récent est gardé');
    const longest = Math.max(0, ...list.map((d) => Math.max(String(d.took || '').length, String(d.result || '').length)));
    ok(longest <= 200, `took et result tronqués : ${longest} caractères au plus (6000 envoyés, 200 au plus attendus)`);
    await s6.stop();
    const f = join(tmp, 'm9', 'devices.json');
    const saved = existsSync(f) ? JSON.parse(readFileSync(f, 'utf8')) : {};
    ok(Object.keys(saved).length >= 1 && Object.keys(saved).length <= 50 && statSync(f).size < 64 << 10,
      `devices.json écrit à l'arrêt : ${Object.keys(saved).length} écrans, ${(statSync(f).size / 1024).toFixed(0)} Ko`);
  }
  {
    // rafale de 1000 requêtes : devices.json n'est pas réécrit à chacune (espion sur renameSync, comme M4)
    const D = join(tmp, 'm9-unite');
    mkdirSync(D, { recursive: true });
    const hub = new SyncHub({ engine: null, store: { path: (n) => join(D, n) }, events: { seq: 0 }, cfg: {}, log: () => {} });
    const orig = fs.renameSync;
    let writes = 0;
    fs.renameSync = function (a, b) { if (String(b).endsWith('devices.json')) writes++; return orig.apply(this, [a, b]); };
    syncBuiltinESMExports();
    try {
      for (let k = 0; k < 1000; k++) hub.seen({ headers: { 'x-vn-device': `e${k % 70}` }, socket: { remoteAddress: '127.0.0.1' } }, { lastReport: k });
    } finally { fs.renameSync = orig; syncBuiltinESMExports(); }
    ok(writes <= 1, `1000 requêtes d'affilée : devices.json réécrit ${writes} fois (au plus 1 attendu)`);
    if (typeof hub.flushDevices === 'function') hub.flushDevices();
    const saved = existsSync(join(D, 'devices.json')) ? JSON.parse(readFileSync(join(D, 'devices.json'), 'utf8')) : {};
    // gardés : les 50 vus en dernier (k = 950…999), même dans la même seconde
    const want = Array.from({ length: 50 }, (_, j) => `e${(950 + j) % 70}`);
    ok(Object.keys(saved).length === 50 && want.every((id) => id in saved) && saved.e19?.lastReport === 999,
      `écriture différée faite (flushDevices) : ${Object.keys(saved).length} écrans, les 50 vus en dernier (${want.filter((id) => !(id in saved)).length} manquant(s))`);
  }

  console.log('m10. manifeste : X-Forwarded-Host et X-Forwarded-Proto ignorés hors adresse publique déclarée');
  {
    const card = await simulateCard({ from: now - 3600, to: now - 60, seed: 13 });
    const withState = (name) => { mkdirSync(join(tmp, name), { recursive: true }); for (const f of ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin']) writeFileSync(join(tmp, name, f), card[f]); };
    withState('m10'); withState('m10-pub');
    const a = await start('m10', {});
    const b = await start('m10-pub', { VN_PUBLIC_URL: 'https://velohnav.example.org' });
    const base = async (srv, headers) => { const r = await req(srv.port, { path: '/sync/manifest', headers }); return r.json?.base || `${r.status} ${r.text.slice(0, 80)}`; };
    const evil = { 'x-forwarded-host': 'evil.example', 'x-forwarded-proto': 'https' };
    const got1 = await base(a, evil);
    ok(got1.startsWith(`http://127.0.0.1:${a.port}/sync/`), `sans adresse publique, X-Forwarded-Host: evil.example → base ${got1}`);
    const got2 = await base(b, evil);
    ok(got2.startsWith(`http://127.0.0.1:${b.port}/sync/`), `VN_PUBLIC_URL réglé, X-Forwarded-Host étranger → base ${got2}`);
    const got3 = await base(b, { 'x-forwarded-host': 'velohnav.example.org', 'x-forwarded-proto': 'https' });
    ok(got3.startsWith('https://velohnav.example.org/sync/'), `par le tunnel (X-Forwarded-Host = l'adresse publique) → base ${got3}`);
    const got5 = await base(b, { host: 'velohnav.example.org', 'x-forwarded-proto': 'https' });
    ok(got5.startsWith('https://velohnav.example.org/sync/'), `par cloudflared (Host = l'adresse publique, sans X-Forwarded-Host) → base ${got5}`);
    const got4 = await base(a, { host: 'evil.example/x?' });
    ok(!/evil/.test(got4), `Host malformé (« evil.example/x? ») : ${got4}`);
  }

  console.log('m12. jeton : jamais lu dans l\'URL par la page ; un ancien lien qui en porte un est effacé de la barre d\'adresse');
  {
    const scripts = readdirSync(PUB).filter((f) => f.endsWith('.js'));
    const readers = scripts.filter((f) => /(URLSearchParams\([^)]*location|searchParams)[^;\n]*\.get\(\s*['"]token['"]/.test(readFileSync(join(PUB, f), 'utf8')));
    ok(scripts.length >= 3 && !readers.length, `scripts de la page qui lisent ?token= : ${readers.join(', ') || 'aucun'} (${scripts.length} lus)`);
    const m = readFileSync(join(PUB, 'board.js'), 'utf8').match(/^function dropUrlToken\([^)]*\) \{[\s\S]*?^\}/m);
    ok(!!m, 'board.js : dropUrlToken() présente (efface ?token= de la barre d\'adresse)');
    if (m) {
      const drop = new Function(`${m[0]}; return dropUrlToken;`)();
      const urls = [];
      const hist = { state: { onglet: 1 }, replaceState: (st, t, u) => urls.push(u) };
      drop({ href: 'http://umbrel.local:8790/?token=s3cret&x=1#plein', search: '?token=s3cret&x=1' }, hist);
      drop({ href: 'http://umbrel.local:8790/?x=1', search: '?x=1' }, hist);
      ok(urls.length === 1 && urls[0] === '/?x=1#plein', `ancien lien « /?token=s3cret&x=1#plein » : barre d'adresse → ${urls.join(' | ') || 'inchangée'} (« /?x=1#plein » attendu, une fois)`);
    }
  }
} finally {
  for (const s of servers) await s.stop();
}
if (fails) for (const s of servers) console.log(s.log().split('\n').slice(-8).join('\n'));
rmSync(tmp, { recursive: true, force: true });
console.log(`\n${checks} vérifications, ${fails} échec(s)`);
process.exit(fails ? 1 : 0);
