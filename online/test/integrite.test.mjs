// ── Intégrité des données sur disque : historique long, état appris, écritures ─
// Chaque section rejoue un constat de la revue du 01/11/2026 sur les vraies classes (lib/history.mjs,
// lib/store.mjs) ou sur le vrai serveur, dans l'ordre exact où le serveur les appelle.
//  M1. redémarrage de nuit : le jour fini est compressé à 5 s, puis le premier relevé (tardif) écrit sa
//      dernière tranche — la journée ne doit pas être réduite à cette seule tranche.
//  M2. vn_live.bin abîmé mais de bonne taille (rHead, rCount, nLog, logHead hors bornes) : refusé au
//      démarrage — pas d'erreur WebAssembly à chaque relevé, pas de corruption resauvegardée à l'arrêt ;
//      une station du journal hors bornes est ramenée à -1.
//  M3. fichier d'état illisible au démarrage (profil vide, historique tronqué) : nommé au journal, mis de côté
//      (<nom>.rejete-…) au lieu d'être écrasé par la première sauvegarde ; la génération précédente (<nom>.prev,
//      gardée à chaque sauvegarde) est reprise si elle est lisible.
//  M4. atomicWrite : fsync du fichier temporaire avant le renommage, puis du dossier ; nom temporaire unique
//      (le PID vaut 1 sous Docker) ; coupure avant le renommage : l'ancienne version reste ; orphelins
//      (.tmp-*, archive/.instantane-*) supprimés au démarrage.
//  m1. coupure pendant un ajout : la ligne tronquée en fin de fichier ne doit pas avaler la tranche suivante
//      (historique et journal des prévisions).
//  m2. jour compressé illisible (.gz abîmé) puis fusion ou compression : le fichier n'est pas écrasé par la
//      seule tranche nouvelle — mis de côté (<jour>.jsonl.gz.rejete-…), ce qui se relit encore est gardé.
//  m3. fusion d'un vidage plus récent (merge) : les tranches locales intermédiaires sont encore écrites, à leur
//      place ; au redémarrage, l'anneau comble les trous de l'historique.
//  m4. vn_hist.bin dont une tranche est datée dans le futur : refusé à la relecture (sinon l'anneau reste figé
//      sur cette tranche : Model::applyStatus n'en crée pas d'antérieure).
//  m5. fichier abîmé à taille égale (un octet de l'anneau à 250 vélos : Model::load l'accepte) : instantané
//      d'archive refusé s'il ne correspond pas au CRC32 de son manifeste ; état du serveur refusé au démarrage
//      s'il ne correspond pas au CRC32 noté à sa sauvegarde (etat.crc32.json), génération précédente reprise.
//  §5.3. import d'une archive : la tranche de tête d'un instantané pris moins de 5 min après son ouverture était en
//      cours — elle n'est pas versée ; close, elle l'est (la revue importait la tranche en cours sans qu'aucun test
//      ne bouge : « added >= 3*287+60 » est trop lâche).
// node online/test/integrite.test.mjs
import fs, { mkdtempSync, rmSync, readdirSync, writeFileSync, readFileSync, mkdirSync, utimesSync, existsSync, appendFileSync } from 'node:fs';
import { syncBuiltinESMExports } from 'node:module';
import { join, dirname } from 'node:path';
import { tmpdir } from 'node:os';
import { History, dayOf, importArchive } from '../lib/history.mjs';
import { loadEngine } from '../public/vn-engine.js';
import * as store from '../lib/store.mjs';     // espace de noms : le test tourne (et échoue) sur un code qui n'a pas encore ces fonctions
import { req, startServer, simulateCard, fakeGbfs, sleep, CORE } from './aides.mjs';

let fails = 0, checks = 0;
// VN_TEST_DETAIL=1 : chaque vérification réussie est aussi affichée (preuves du rapport de correctifs)
const ok = (c, msg) => { checks++; if (!c) { fails++; console.log(`  ÉCHEC : ${msg}`); } else if (process.env.VN_TEST_DETAIL) console.log(`  ok : ${msg}`); };
const tmp = mkdtempSync(join(tmpdir(), 'vn-integrite-'));
let dirN = 0;
const fresh = () => join(tmp, `d${++dirN}`);

// Tranches synthétiques d'un jour de Luxembourg (heure d'hiver : minuit local = 23 h UTC la veille)
const ST = [['1', 'Gare', 20, 49.6, 6.13], ['2', 'Hamilius', 15, 49.61, 6.12], ['3', 'Glacis', 30, 49.62, 6.12]];
const DAY = '2026-10-31', NEXT = '2026-11-01';
const T0 = Date.UTC(2026, 9, 30, 23, 0, 0) / 1000;
const slot = (k) => ({ t: T0 + k * 300, n: [k, 40, 1, k + 1, 2, 1, 0, 0], w: [9, 0, 10, 12, 3], b: [k % 20, 7, (k * 3) % 30], st: ST });

try {
  console.log('M1. redémarrage de nuit : journée compressée puis dernière tranche écrite en retard');
  {
    const D = fresh();
    const h1 = new History(D);
    h1.append(Array.from({ length: 287 }, (_, k) => slot(k)));        // 0 h 00 → 23 h 50 : 23 h 55 est encore en cours
    // arrêt, redémarrage après minuit : tâche des 5 s (compression des jours finis), flux lent au démarrage
    const h2 = new History(D);
    ok(h2.lastT === slot(286).t, 'au redémarrage, la dernière tranche écrite est retrouvée');
    h2.compressDone(NEXT);
    ok(readdirSync(join(D, 'historique')).join() === `${DAY}.jsonl.gz`, 'jour fini compressé');
    h2.append([slot(287)]);                                           // premier relevé : la tranche de 23 h 55 se ferme
    ok(h2.readDay(DAY).length === 288, `après l'écriture tardive : ${h2.readDay(DAY).length} tranches lues (288 attendues)`);
    h2.compressDone(NEXT);                                            // tâche de nuit suivante
    const h3 = new History(D);
    const recs = h3.readDay(DAY);
    ok(recs.length === 288 && recs.at(-1).t === slot(287).t && recs.every((r, k) => r.t === slot(k).t),
      `après la compression suivante : ${recs.length} tranches (288 attendues)`);
    ok(readdirSync(join(D, 'historique')).join() === `${DAY}.jsonl.gz`, `un seul fichier pour le jour : ${readdirSync(join(D, 'historique')).join(', ')}`);
  }
  console.log('M1. un clair plus court ne remplace jamais un .gz plus complet');
  {
    const D = fresh();
    const h = new History(D);
    h.append(Array.from({ length: 288 }, (_, k) => slot(k)));
    h.compressDone(NEXT);
    // un clair qui ne contient qu'une tranche (coupure pendant la compression, ou ancienne version)
    writeFileSync(join(D, 'historique', `${DAY}.jsonl`), `${JSON.stringify({ st: ST })}\n${JSON.stringify({ t: slot(5).t, n: slot(5).n, w: slot(5).w, b: slot(5).b })}\n`);
    const h2 = new History(D);
    ok(h2.readDay(DAY).length === 288, `clair d'une tranche à côté du .gz : ${h2.readDay(DAY).length} tranches lues (288 attendues)`);
    h2.compressDone(NEXT);
    ok(new History(D).readDay(DAY).length === 288, 'compression : le .gz garde ses 288 tranches');
  }

  console.log('M2. vn_live.bin abîmé mais de bonne taille : refusé au démarrage');
  {
    const now = Math.floor(Date.now() / 1000);
    const card = await simulateCard({ from: now - 2 * 3600, to: now - 60, stepS: 60, seed: 21 });
    const gbfs = await fakeGbfs();
    const hdr = (b) => { const v = new DataView(b.buffer, b.byteOffset, b.byteLength); return { rHead: v.getInt32(68, true), rCount: v.getInt32(72, true), nLog: v.getInt32(76, true), logHead: v.getInt32(80, true) }; };
    const cases = [
      ['rHead = 1 000 000 000', (b) => b.writeInt32LE(1e9, 68)],
      ['rHead = −2 000 000 000', (b) => b.writeInt32LE(-2e9, 68)],
      ['rCount = 61', (b) => b.writeInt32LE(61, 72)],
      ['nLog = 5000', (b) => b.writeInt32LE(5000, 76)],
      ['logHead = −5000', (b) => b.writeInt32LE(-5000, 80)],
    ];
    try {
      for (const [what, mutate] of cases) {
        const D = fresh();
        mkdirSync(D, { recursive: true });
        for (const f of ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin']) writeFileSync(join(D, f), card[f]);
        const live = Buffer.from(card['vn_live.bin']); mutate(live);
        writeFileSync(join(D, 'vn_live.bin'), live);
        const n0 = gbfs.status;
        const s = await startServer({ data: D, env: { VN_GBFS_BASE: gbfs.base } });
        // premier cycle de relevé : station_status servi puis appliqué
        let sum = null;
        for (let k = 0; k < 60; k++) { sum = (await req(s.port, { path: '/api/summary' })).json; if (gbfs.status > n0 && sum.lastUpdate >= now) break; await sleep(100); }
        await sleep(300);
        sum = (await req(s.port, { path: '/api/summary' })).json;
        const health = (await req(s.port, { path: '/healthz' })).status;
        await s.stop();
        const log = s.log();
        const saved = hdr(readFileSync(join(D, 'vn_live.bin')));
        ok(!/RuntimeError|out of bounds/.test(log), `${what} : aucune erreur WebAssembly au relevé${/RuntimeError/.test(log) ? ` (${(log.match(/RuntimeError[^\n]*/) || [''])[0]})` : ''}`);
        ok(sum.lastUpdate >= now && health === 200, `${what} : relevé appliqué, /healthz ${health}`);
        ok(/vn_live\.bin refusé/.test(log), `${what} : refus journalisé`);
        ok(saved.rHead >= -1 && saved.rHead < 60 && saved.rCount >= 0 && saved.rCount <= 60 && saved.nLog >= 0 && saved.nLog <= 1024 && saved.logHead >= -1 && saved.logHead < 1024,
          `${what} : fichier resauvegardé à l'arrêt dans les bornes (${JSON.stringify(saved)})`);
      }
      // un état vivant sain est toujours repris
      const D = fresh();
      mkdirSync(D, { recursive: true });
      for (const f of ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin', 'vn_live.bin']) writeFileSync(join(D, f), card[f]);
      const s = await startServer({ data: D });
      await s.stop();
      ok(/état vivant oui/.test(s.log()) && !/refusé/.test(s.log()), 'état vivant sain : repris');
      // journal des ruptures : une station hors [0, n) est ramenée à -1, le reste de l'état est repris
      const L = Buffer.from(card['vn_live.bin']);
      const lv = new DataView(L.buffer, L.byteOffset, L.byteLength);
      const logAt = 96 + lv.getUint16(8, true) * lv.getUint16(6, true) + 8 * 60 + 60 * lv.getUint16(6, true);
      L.writeInt16LE(5000, logAt + 12);
      const D2 = fresh();
      mkdirSync(D2, { recursive: true });
      for (const f of ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin']) writeFileSync(join(D2, f), card[f]);
      writeFileSync(join(D2, 'vn_live.bin'), L);
      const s2 = await startServer({ data: D2 });
      await s2.stop();
      const st0 = readFileSync(join(D2, 'vn_live.bin')).readInt16LE(logAt + 12);
      ok(/état vivant oui/.test(s2.log()) && st0 === -1, `journal : station 5000 (hors bornes) ramenée à ${st0}, état vivant repris`);
    } finally { await gbfs.close(); }
  }
  console.log('M2. checkLive : bornes de l\'en-tête, fichier sain inchangé');
  if (typeof store.checkLive !== 'function') ok(false, 'lib/store.mjs n\'exporte pas checkLive');
  else {
    const { checkLive } = store;
    const now = Math.floor(Date.now() / 1000);
    const card = await simulateCard({ from: now - 3600, to: now - 60, stepS: 60, seed: 5 });
    const good = card['vn_live.bin'];
    const c0 = checkLive(good);
    ok(c0.why === null && c0.buf === good, 'fichier sain : accepté, non recopié');
    const at = (o, v) => { const b = Buffer.from(good); b.writeInt32LE(v, o); return checkLive(b).why; };
    ok(/rHead/.test(at(68, 60)) && /rHead/.test(at(68, -2)) && /rCount/.test(at(72, -1)) && /rCount/.test(at(72, 0)), 'rHead 60 ou −2, rCount −1, ou rCount 0 avec rHead ≥ 0 : refusés');
    ok(/logHead/.test(at(80, 1024)) && /nLog/.test(at(76, 1025)) && /nLog/.test(at(76, 0)), 'logHead 1024, nLog 1025, ou nLog 0 avec logHead ≥ 0 : refusés');
    ok(checkLive(good.subarray(0, 50)).why === 'en-tête tronqué', 'en-tête tronqué : refusé');
  }

  console.log('M3. fichier d\'état illisible : mis de côté et nommé, génération précédente reprise');
  {
    const now = Math.floor(Date.now() / 1000);
    const A = await simulateCard({ from: now - 86400, to: now - 120, seed: 31 });
    const B = await simulateCard({ from: now - 2 * 86400, to: now - 150, seed: 32 });
    const put = (D, files) => { mkdirSync(D, { recursive: true }); for (const [f, b] of Object.entries(files)) writeFileSync(join(D, f), b); };
    const rejected = (D, f) => readdirSync(D).filter((x) => x.startsWith(`${f}.rejete-`));
    const rd = (p) => { try { return readFileSync(p); } catch { return null; } };
    // le scénario de la revue : vn_prof.bin vide, vn_hist.bin tronqué
    {
      const D = fresh();
      const cut = A['vn_hist.bin'].subarray(0, 5000);
      put(D, { 'vn_prof.bin': Buffer.alloc(0), 'vn_hist.bin': cut, 'vn_days.bin': A['vn_days.bin'], 'vn_live.bin': A['vn_live.bin'] });
      const s = await startServer({ data: D });
      await s.stop();
      const L = s.log(), kept = rejected(D, 'vn_hist.bin');
      ok(!/profils oui/.test(L) && /profils non/.test(L), `journal : profils non (« ${(L.match(/\[fs\] état relu[^\n]*/) || ['?'])[0]} »)`);
      ok(/vn_prof\.bin vide/.test(L) && /vn_hist\.bin refusé/.test(L), 'refus nommés fichier par fichier');
      ok(kept.length === 1 && readFileSync(join(D, kept[0])).equals(cut), `vn_hist.bin tronqué gardé tel quel après la sauvegarde de l'arrêt (${kept.join(', ') || 'aucune copie'})`);
    }
    // profil abîmé, génération précédente lisible : elle est reprise
    {
      const D = fresh();
      const cut = B['vn_prof.bin'].subarray(0, 100000);
      put(D, { 'vn_prof.bin': cut, 'vn_prof.bin.prev': B['vn_prof.bin'], 'vn_hist.bin': B['vn_hist.bin'], 'vn_days.bin': B['vn_days.bin'] });
      const s = await startServer({ data: D });
      const sum = (await req(s.port, { path: '/api/summary' })).json;
      await s.stop();
      const kept = rejected(D, 'vn_prof.bin');
      ok(/vn_prof\.bin repris de vn_prof\.bin\.prev/.test(s.log()) && sum.learnSamples === B.summary.learnSamples,
        `profil abîmé : génération précédente reprise (${sum.learnSamples} relevés appris, ${B.summary.learnSamples} attendus)`);
      ok(kept.length === 1 && readFileSync(join(D, kept[0])).equals(cut), 'profil abîmé gardé de côté');
      ok(rd(join(D, 'vn_prof.bin.prev'))?.equals(B['vn_prof.bin']), 'la génération précédente n\'est pas écrasée par la sauvegarde suivante');
    }
    // chaque sauvegarde garde la version remplacée
    {
      const D = fresh();
      put(D, { 'vn_prof.bin': A['vn_prof.bin'], 'vn_hist.bin': A['vn_hist.bin'], 'vn_days.bin': A['vn_days.bin'], 'vn_live.bin': A['vn_live.bin'] });
      const s = await startServer({ data: D });
      await s.stop();
      ok(/état sauvegardé \(arrêt/.test(s.log()) && ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin', 'vn_live.bin'].every((f) => rd(join(D, `${f}.prev`))?.equals(A[f])),
        'sauvegarde : la version remplacée de chaque fichier est gardée (<nom>.prev)');
    }
    // historique 24 h de plus de 20 h : règle de la carte, pas une erreur
    {
      const D = fresh();
      const old = await simulateCard({ from: now - 2 * 86400, to: now - 30 * 3600, seed: 33 });
      put(D, { 'vn_prof.bin': old['vn_prof.bin'], 'vn_hist.bin': old['vn_hist.bin'] });
      const s = await startServer({ data: D });
      await s.stop();
      ok(/vn_hist\.bin non repris : sauvegardé il y a 30 h/.test(s.log()) && rejected(D, 'vn_hist.bin').length === 0 && /profils oui/.test(s.log()),
        'historique 24 h de 30 h : non repris, nommé, pas mis de côté ; profils repris');
    }
  }

  console.log('M4. atomicWrite : fsync avant le renommage, puis du dossier ; orphelins supprimés');
  {
    // Espion sur node:fs : les liaisons ESM de lib/store.mjs suivent (syncBuiltinESMExports), rien d'autre ne change
    const names = ['openSync', 'writeSync', 'writeFileSync', 'fsyncSync', 'fdatasyncSync', 'closeSync', 'renameSync'];
    const orig = Object.fromEntries(names.map((k) => [k, fs[k]]));
    let calls = [], failRename = false;
    const fdPath = new Map();
    for (const k of names) {
      fs[k] = function (...a) {
        if (k === 'renameSync' && failRename) { calls.push({ k, a }); throw Object.assign(new Error('coupure simulée'), { code: 'EIO' }); }
        const r = orig[k].apply(this, a);
        if (k === 'openSync') fdPath.set(r, String(a[0]));
        calls.push({ k, a, path: typeof a[0] === 'number' ? fdPath.get(a[0]) : String(a[0]) });
        return r;
      };
    }
    syncBuiltinESMExports();
    try {
      const D = fresh();
      mkdirSync(D, { recursive: true });
      const P = join(D, 'vn_prof.bin');
      writeFileSync(P, 'ancienne version');
      calls = [];
      store.atomicWrite(P, Buffer.from('nouvelle version'));
      const mine = calls.filter((c) => c.path && (c.path.startsWith(P) || c.path === D));
      const tmpOpen = mine.find((c) => c.k === 'openSync' && c.path.startsWith(`${P}.tmp-`));
      const ren = mine.findIndex((c) => c.k === 'renameSync');
      const fsyncTmp = mine.findIndex((c) => (c.k === 'fsyncSync' || c.k === 'fdatasyncSync') && c.path === tmpOpen?.path);
      const fsyncDir = mine.findIndex((c, i) => i > ren && c.k === 'fsyncSync' && c.path === D);
      const trace = mine.map((c) => `${c.k}(${c.path === D ? 'dossier' : c.path.slice(P.length) || 'cible'})`).join(' → ');
      ok(tmpOpen && fsyncTmp >= 0 && ren > fsyncTmp, `fsync du fichier temporaire avant le renommage : ${trace}`);
      ok(fsyncDir > ren, 'fsync du dossier après le renommage');
      ok(readFileSync(P, 'utf8') === 'nouvelle version' && !readdirSync(D).some((f) => f.includes('.tmp-')), 'contenu écrit, aucun temporaire laissé');
      calls = [];
      store.atomicWrite(P, Buffer.from('v3'));
      const tmp2 = calls.find((c) => c.k === 'openSync' && c.path.startsWith(`${P}.tmp-`))?.path;
      ok(tmp2 && tmpOpen && tmp2 !== tmpOpen.path, `nom temporaire unique d'une écriture à l'autre (${tmpOpen?.path.slice(P.length)} puis ${tmp2?.slice(P.length)})`);
      // coupure entre l'écriture et le renommage
      failRename = true;
      let threw = false;
      try { store.atomicWrite(P, Buffer.from('jamais visible')); } catch { threw = true; }
      failRename = false;
      ok(threw && readFileSync(P, 'utf8') === 'v3', `coupure avant le renommage : l'ancienne version reste (« ${readFileSync(P, 'utf8')} »)`);
      ok(!readdirSync(D).some((f) => f.includes('.tmp-')), 'échec : le temporaire est supprimé');
    } finally {
      for (const k of names) fs[k] = orig[k];
      syncBuiltinESMExports();
    }
    // orphelins d'un arrêt brutal : supprimés au démarrage s'ils ont plus de 10 min (un autre processus peut écrire)
    const D = fresh();
    mkdirSync(join(D, 'historique'), { recursive: true });
    mkdirSync(join(D, 'archive', '.instantane-1-1793000000000'), { recursive: true });
    mkdirSync(join(D, 'archive', '20261101-060052'), { recursive: true });
    const old = [join(D, 'vn_prof.bin.tmp-1'), join(D, 'historique', '2026-10-31.jsonl.gz.tmp-1-0a1b2c3d'), join(D, 'archive', '.instantane-1-1793000000000')];
    const recent = join(D, 'vn_hist.bin.tmp-7-deadbeef');
    for (const f of [old[0], old[1], recent]) writeFileSync(f, 'x');
    writeFileSync(join(D, 'archive', '20261101-060052', 'vn_hist.bin'), 'instantané');
    const past = new Date(Date.now() - 3600e3);
    for (const f of old) utimesSync(f, past, past);
    const s = await startServer({ data: D });
    await s.stop();
    ok(old.every((f) => !existsSync(f)) && existsSync(recent) && existsSync(join(D, 'archive', '20261101-060052', 'vn_hist.bin')),
      `orphelins de plus de 10 min supprimés au démarrage, le reste gardé (${(s.log().match(/\[fs\][^\n]*orphelin[^\n]*/) || ['aucun message'])[0]})`);
  }

  console.log('m1. ligne tronquée en fin de fichier : la tranche suivante n\'y est pas collée');
  {
    const D = fresh();
    const h = new History(D);
    h.append(Array.from({ length: 10 }, (_, k) => slot(k)));
    // coupure pendant l'ajout de la tranche 10 : une ligne sans fin
    const line = JSON.stringify({ t: slot(10).t, n: slot(10).n, w: slot(10).w, b: slot(10).b });
    appendFileSync(join(D, 'historique', `${DAY}.jsonl`), line.slice(0, 25));
    const h2 = new History(D);
    h2.append([slot(11), slot(12)]);
    const recs = new History(D).readDay(DAY);
    ok(recs.some((r) => r.t === slot(11).t) && recs.some((r) => r.t === slot(12).t), `tranches écrites après la coupure : ${recs.length} lues (12 attendues : 0 à 9, 11, 12)`);
    // journal des prévisions : même règle
    const { ForecastLog } = await import('../lib/forecast.mjs');
    const F = fresh();
    const fl = new ForecastLog(F, new History(F));
    const pday = '2026-10-30', pslot = Date.UTC(2026, 9, 30, 7, 0, 0) / 1000;      // 8 h à Luxembourg
    mkdirSync(join(F, 'prevision'), { recursive: true });
    writeFileSync(join(F, 'prevision', `${pday}.jsonl`), `${JSON.stringify({ sid: ['1'] })}\n${JSON.stringify({ slot: pslot, t: pslot, st: [[1]] })}\n{"slot":${pslot + 900},"t":`);
    const fake = { ringHead: () => pslot + 1800, forecast: () => ({ slot: pslot + 1800, t: pslot + 1800, net60: 0, bikes: 1, st: [[2]] }), stations: () => [{ sid: '1' }] };
    const fl2 = new ForecastLog(F, new History(F));
    fl2.capture(fake);
    ok(fl2.readDay(pday).map((x) => x.slot).join() === `${pslot},${pslot + 1800}`, `journal des prévisions : quart d'heure suivant lisible après la coupure (${fl2.readDay(pday).map((x) => x.slot - pslot).join(', ')})`);
  }

  console.log('m2. jour compressé illisible : mis de côté, jamais écrasé par la seule tranche nouvelle');
  {
    const day288 = (D) => { const h = new History(D); h.append(Array.from({ length: 288 }, (_, k) => slot(k))); h.compressDone(NEXT); return join(D, 'historique', `${DAY}.jsonl.gz`); };
    const asideOf = (D) => readdirSync(join(D, 'historique')).filter((f) => f.startsWith(`${DAY}.jsonl.gz.rejete-`));
    // a. octets qui ne sont pas du gzip : rien à relire, mais le fichier est gardé
    {
      const D = fresh();
      const gz = day288(D);
      const junk = Buffer.from('PAS DU GZIP '.repeat(100));
      writeFileSync(gz, junk);
      const h = new History(D);
      h.merge([slot(150)]);
      const aside = asideOf(D);
      ok(aside.length === 1 && readFileSync(join(D, 'historique', aside[0])).equals(junk), `fusion sur un .gz illisible : fichier gardé tel quel (${aside.join(', ') || 'aucune copie'})`);
    }
    // b. .gz tronqué (coupure) : ce qui précède la coupure est encore relu et gardé
    {
      const D = fresh();
      const gz = day288(D);
      const full = readFileSync(gz);
      writeFileSync(gz, full.subarray(0, Math.floor(full.length / 2)));
      const h = new History(D);
      h.merge([slot(276)]);
      const recs = new History(D).readDay(DAY);
      ok(recs.length > 100 && recs.some((r) => r.t === slot(276).t), `fusion sur un .gz tronqué : ${recs.length} tranches gardées (le fichier en avait 288 ; avant : la seule tranche nouvelle)`);
      ok(asideOf(D).length === 1, 'le .gz tronqué est mis de côté');
    }
    // c. compression d'un clair à côté d'un .gz tronqué
    {
      const D = fresh();
      const gz = day288(D);
      const full = readFileSync(gz);
      writeFileSync(gz, full.subarray(0, Math.floor(full.length / 2)));
      writeFileSync(join(D, 'historique', `${DAY}.jsonl`), `${JSON.stringify({ st: ST })}\n${JSON.stringify({ t: slot(280).t, n: slot(280).n, w: slot(280).w, b: slot(280).b })}\n`);
      const h = new History(D);
      h.compressDone(NEXT);
      const recs = new History(D).readDay(DAY);
      ok(recs.length > 100 && asideOf(D).length === 1, `compression à côté d'un .gz tronqué : ${recs.length} tranches, ${asideOf(D).length} copie(s) du .gz`);
    }
  }

  console.log('m3. fusion d\'un vidage plus récent : les tranches locales intermédiaires sont gardées');
  {
    const range = (a, b, mk = slot) => Array.from({ length: b - a + 1 }, (_, k) => mk(a + k));
    const sortedUnique = (recs) => recs.every((r, k) => k === 0 || r.t > recs[k - 1].t);
    // a. jour passé (chemin .gz) puis b. jour en cours (chemin clair)
    const now = Math.floor(Date.now() / 1000);
    let t0 = now - (now % 300);
    while (dayOf(t0 - 300) === dayOf(now)) t0 -= 300;
    const todaySlot = (k) => ({ ...slot(k), t: t0 + k * 300 });
    for (const [what, mk] of [['jour passé', slot], ['jour en cours', todaySlot]]) {
      const D = fresh();
      const h = new History(D);
      h.append(range(0, 9, mk));
      h.merge(range(100, 110, mk));                      // vidage d'une carte, plus récent que le relevé local
      ok(h.lastT === mk(9).t, `${what} : la fusion ne déplace pas la dernière tranche relevée (${(h.lastT - mk(0).t) / 300}, 9 attendu)`);
      h.append(range(10, 99, mk));                       // le relevé local continue
      const recs = new History(D).readDay(dayOf(mk(0).t));
      ok(recs.length === 111 && sortedUnique(recs), `${what} : ${recs.length} tranches après le relevé local (111 attendues, triées, sans double)`);
    }
    // c. redémarrage : l'historique a 0..9 et 100..110 ; l'anneau relu (0..120) comble les trous
    {
      const D = fresh();
      const h = new History(D);
      h.append(range(0, 9));
      h.merge(range(100, 110));
      const ring = range(0, 120);
      const engine = { ringHead: () => ring.at(-1).t + 300, slot: (back) => (back >= 1 && back <= ring.length ? { ...ring[ring.length - back], st: undefined } : null),
        stations: () => ST.map(([sid, name, cap, lat, lon]) => ({ sid, name, cap, lat, lon })) };
      const h2 = new History(D);
      const n = h2.capture(engine);
      const recs = new History(D).readDay(DAY);
      ok(recs.length === 121 && sortedUnique(recs), `redémarrage : l'anneau comble les trous (${n} tranches écrites, ${recs.length} au total, 121 attendues)`);
      ok(h2.capture({ ...engine, ringHead: () => ring.at(-1).t + 600 }) === 0, 'l\'anneau relu ensuite ne réécrit rien');
    }
  }

  console.log('m4. tranche de tête datée dans le futur : vn_hist.bin refusé, l\'anneau avance');
  {
    const now = Math.floor(Date.now() / 1000);
    const card = await simulateCard({ from: now - 6 * 3600, to: now - 120, seed: 41 });
    const forge = (dt) => {
      const h = Buffer.from(card['vn_hist.bin']);
      const nSt = h.readUInt16LE(6), head = h.readInt32LE(8), saved = Number(h.readBigInt64LE(32));
      const ringAt = 40 + nSt * 8, nsz = (h.length - ringAt) / 288 - 8 - nSt;
      h.writeBigInt64LE(BigInt(saved + dt), ringAt + head * 8);
      h.writeBigInt64LE(BigInt(saved + dt), ringAt + 288 * 8 + head * nsz);
      return h;
    };
    const gbfs = await fakeGbfs();
    try {
      for (const [what, hist, refused] of [['tête datée de +3 jours', forge(3 * 86400), true], ['historique sain', card['vn_hist.bin'], false]]) {
        const D = fresh();
        mkdirSync(D, { recursive: true });
        writeFileSync(join(D, 'vn_prof.bin'), card['vn_prof.bin']);
        writeFileSync(join(D, 'vn_hist.bin'), hist);
        const n0 = gbfs.status;
        const s = await startServer({ data: D, env: { VN_GBFS_BASE: gbfs.base } });
        for (let k = 0; k < 60 && gbfs.status <= n0; k++) await sleep(100);
        await sleep(300);
        const ring = (await req(s.port, { path: '/api/ring' })).json || [];
        await s.stop();
        const headT = ring.at(-1)?.[0] ?? 0;
        if (refused) {
          ok(/vn_hist\.bin refusé \(tranche datée/.test(s.log()) && readdirSync(D).some((f) => f.startsWith('vn_hist.bin.rejete-')), `${what} : refus journalisé, fichier gardé de côté`);
          ok(headT > 0 && headT <= now + 300, `${what} : tête de l'anneau après le premier relevé à ${headT - now} s de maintenant (≤ 300 attendu)`);
        } else ok(/historique 24 h oui/.test(s.log()) && headT <= now + 300 && ring.length > 60, `${what} : repris (${ring.length} tranches)`);
      }
      // --import d'un vidage de carte dont l'anneau a une tête datée dans le futur : historique 24 h ignoré
      const dump = fresh(), D = fresh();
      mkdirSync(dump, { recursive: true });
      writeFileSync(join(dump, 'vn_prof.bin'), card['vn_prof.bin']);
      writeFileSync(join(dump, 'vn_hist.bin'), forge(3 * 86400));
      const n0 = gbfs.status;
      const s = await startServer({ data: D, args: ['--import', dump, '--serve'], env: { VN_GBFS_BASE: gbfs.base } });
      for (let k = 0; k < 60 && gbfs.status <= n0; k++) await sleep(100);
      await sleep(300);
      const ring = (await req(s.port, { path: '/api/ring' })).json || [];
      await s.stop();
      const headT = ring.at(-1)?.[0] ?? 0;
      ok(/--import : vn_hist\.bin ignoré \(tranche datée/.test(s.log()) && headT > 0 && headT <= now + 300,
        `--import, tête datée de +3 jours : historique 24 h ignoré, tête de l'anneau à ${headT - now} s de maintenant (≤ 300 attendu)`);
    } finally { await gbfs.close(); }
    console.log('m4. checkHist : tranche après la sauvegarde ou après l\'heure du serveur ; import de l\'archive');
    if (typeof store.checkHist !== 'function') ok(false, 'lib/store.mjs n\'exporte pas checkHist');
    else {
      const { checkHist } = store;
      const h = card['vn_hist.bin'], saved = Number(h.readBigInt64LE(32));
      const nSt = h.readUInt16LE(6), ringAt = 40 + nSt * 8;
      ok(checkHist(h, now) === null, 'historique sain : accepté');
      ok(/^tranche datée .* après la sauvegarde/.test(checkHist(forge(3 * 86400), now) || ''), 'tête datée de +3 jours : refusée (après la sauvegarde)');
      ok(checkHist(forge(240), now) === null, 'tête datée de +4 min : acceptée (petit recul d\'horloge, marge de 5 min)');
      const old = Buffer.from(h); old.writeBigInt64LE(BigInt(saved + 86400), ringAt + ((h.readInt32LE(8) + 5) % 288) * 8);
      ok(/^tranche datée/.test(checkHist(old, now) || ''), 'tranche ancienne de l\'anneau datée dans le futur : refusée aussi');
      const ahead = forge(2 * 86400); ahead.writeBigInt64LE(BigInt(saved + 2 * 86400), 32);
      ok(/après l'heure du serveur/.test(checkHist(ahead, now) || ''), 'sauvegarde et tête datées de +2 jours : refusées (après l\'heure du serveur)');
    }
    {
      // instantané d'archive dont une tranche (10 avant la tête) est datée de +3 jours : il n'entre pas dans
      // l'historique long (sinon un jour à venir y est écrit)
      const D = fresh(), snap = join(D, 'archive', '20261101-060000');
      mkdirSync(snap, { recursive: true });
      writeFileSync(join(snap, 'vn_prof.bin'), card['vn_prof.bin']);
      const h = Buffer.from(card['vn_hist.bin']);
      const nSt = h.readUInt16LE(6), ringAt = 40 + nSt * 8, nsz = (h.length - ringAt) / 288 - 8 - nSt, r10 = (h.readInt32LE(8) - 10 + 288) % 288;
      const saved = Number(h.readBigInt64LE(32));
      h.writeBigInt64LE(BigInt(saved + 3 * 86400), ringAt + r10 * 8);
      h.writeBigInt64LE(BigInt(saved + 3 * 86400), ringAt + 288 * 8 + r10 * nsz);
      writeFileSync(join(snap, 'vn_hist.bin'), h);
      writeFileSync(join(snap, 'manifeste.json'), JSON.stringify({ t_carte: saved, fichiers: {} }));
      const hist = new History(D);
      const r = await importArchive({ dirs: [snap], history: hist, makeEngine: () => loadEngine(CORE, { onLog: () => {} }) });
      const future = hist.files().filter((f) => f.day > dayOf(now + 3600));
      ok(r.refused === 1 && future.length === 0, `import de l'archive : instantané refusé (${r.refused}), aucun jour à venir écrit (${future.map((f) => f.day).join(', ') || 'aucun'})`);
    }
  }

  console.log('m5. CRC32 : fichier abîmé à taille égale (anneau à 250 vélos) — instantané d\'archive, état du serveur');
  {
    const now = Math.floor(Date.now() / 1000);
    const card = await simulateCard({ from: now - 4 * 3600, to: now - 120, seed: 51 });
    const eng = await loadEngine(CORE, { onLog: () => {} });
    const hex = (b) => eng.crc32(b).toString(16).toUpperCase().padStart(8, '0');
    const mkEngine = () => loadEngine(CORE, { onLog: () => {} });
    // vélos de la première station dans la tranche de tête : 250 (même taille ; Model::load relit le fichier)
    const at250 = (src) => {
      const h = Buffer.from(src);
      const nSt = h.readUInt16LE(6), ringAt = 40 + nSt * 8, nsz = (h.length - ringAt) / 288 - 8 - nSt;
      h[ringAt + 288 * (8 + nsz) + h.readInt32LE(8) * nSt] = 250;
      return h;
    };
    const snap = (D, name, hist, decl = {}) => {
      const d = join(D, 'archive', name);
      mkdirSync(d, { recursive: true });
      const files = { 'vn_prof.bin': card['vn_prof.bin'], 'vn_hist.bin': hist, 'vn_days.bin': card['vn_days.bin'] };
      for (const [f, b] of Object.entries(files)) writeFileSync(join(d, f), b);
      // le manifeste décrit les fichiers d'origine (store.archive, tools/carte_dump.py)
      const fichiers = {};
      for (const f of Object.keys(files)) fichiers[f] = { taille: card[f].length, crc32: hex(card[f]), ...(decl[f] || {}) };
      writeFileSync(join(d, 'manifeste.json'), JSON.stringify({ t_carte: Number(card['vn_hist.bin'].readBigInt64LE(32)), fichiers }));
      return d;
    };
    {
      const D = fresh(), logs = [];
      const r = await importArchive({ dirs: [snap(D, '20261101-060000', at250(card['vn_hist.bin']))], history: new History(D), makeEngine: mkEngine, log: (m) => logs.push(m) });
      ok(r.refused === 1 && r.added === 0, `archive : vn_hist.bin différent de son manifeste (même taille) → instantané refusé (${r.refused} refusé, ${r.added} tranches ajoutées)`);
      ok(logs.some((m) => /ignoré \(vn_hist\.bin : CRC32/.test(m)), `refus journalisé et nommé (${logs.find((m) => /ignoré/.test(m)) || logs.at(-1)})`);
      const D2 = fresh();
      const r2 = await importArchive({ dirs: [snap(D2, '20261101-060000', card['vn_hist.bin'], { 'vn_days.bin': { taille: card['vn_days.bin'].length + 1 } })], history: new History(D2), makeEngine: mkEngine });
      ok(r2.refused === 1 && r2.added === 0, `archive : taille déclarée fausse → instantané refusé (${r2.refused})`);
      const D3 = fresh();
      const r3 = await importArchive({ dirs: [snap(D3, '20261101-060000', card['vn_hist.bin'])], history: new History(D3), makeEngine: mkEngine });
      ok(r3.snaps === 1 && r3.added >= 45, `archive conforme à son manifeste : importée (${r3.added} tranches)`);
    }
    {
      const D = fresh();
      mkdirSync(D, { recursive: true });
      for (const f of ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin', 'vn_live.bin']) writeFileSync(join(D, f), card[f]);
      const rejected = (f) => readdirSync(D).filter((x) => x.startsWith(`${f}.rejete-`));
      const relu = (L) => (L.match(/\[fs\] état relu[^\n]*/) || ['?'])[0];
      let s = await startServer({ data: D });
      await s.stop();
      s = await startServer({ data: D });
      await s.stop();
      ok(/profils oui, historique 24 h oui, résumés par jour oui, état vivant oui/.test(s.log()) && !/refusé/.test(s.log()), `redémarrage sans altération : tout est repris (${relu(s.log())})`);
      writeFileSync(join(D, 'vn_hist.bin'), at250(readFileSync(join(D, 'vn_hist.bin'))));
      s = await startServer({ data: D });
      await s.stop();
      ok(/vn_hist\.bin refusé \(CRC32/.test(s.log()) && rejected('vn_hist.bin').length === 1, `vn_hist.bin abîmé sur le disque (même taille) : refusé, gardé de côté (${(s.log().match(/vn_hist\.bin (refusé|repris)[^\n]*/) || [relu(s.log())])[0]})`);
      ok(/vn_hist\.bin repris de vn_hist\.bin\.prev/.test(s.log()) && /historique 24 h oui \(génération précédente\)/.test(s.log()), 'génération précédente reprise');
      // coupure entre le manifeste et l'écriture des fichiers : la génération précédente, encore en place, est reconnue
      writeFileSync(join(D, 'vn_prof.bin'), readFileSync(join(D, 'vn_prof.bin.prev')));
      s = await startServer({ data: D });
      await s.stop();
      ok(/profils oui,/.test(s.log()) && !/vn_prof\.bin refusé/.test(s.log()), `génération précédente encore en place (coupure avant l'écriture) : reprise (${relu(s.log())})`);
    }
  }

  console.log('§5.3. import d\'une archive : tranche de tête en cours non versée, close versée');
  {
    const now = Math.floor(Date.now() / 1000);
    const card = await simulateCard({ from: now - 3 * 3600, to: now - 900, seed: 53 });
    const head = card.engine.ringHead();
    // même vidage, deux instants d'instantané : 2 min puis 5 min après l'ouverture de la tranche de tête
    const imp = async (t) => {
      const D = fresh(), d = join(D, 'archive', '20261101-060000');
      mkdirSync(d, { recursive: true });
      for (const f of ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin']) writeFileSync(join(d, f), card[f]);
      writeFileSync(join(d, 'manifeste.json'), JSON.stringify({ t_carte: t, fichiers: {} }));
      const h = new History(D);
      const r = await importArchive({ dirs: [d], history: h, makeEngine: () => loadEngine(CORE, { onLog: () => {} }) });
      return { r, ts: h.records(dayOf(head - 86400), dayOf(head)).map((x) => x.t) };
    };
    const a = await imp(head + 120), b = await imp(head + 300);
    ok(head > 0 && a.r.snaps === 1 && a.ts.length > 30 && !a.ts.includes(head) && a.ts.at(-1) === head - 300,
      `instantané à +2 min : tranche de tête en cours non versée (${a.r.added} tranches, dernière à tête − ${(head - a.ts.at(-1)) / 60} min)`);
    ok(b.r.snaps === 1 && b.ts.includes(head) && b.r.added === a.r.added + 1, `instantané à +5 min : tranche de tête close versée (${b.r.added} tranches)`);
  }
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`\n${checks} vérifications, ${fails} échec(s)`);
process.exit(fails ? 1 : 0);
