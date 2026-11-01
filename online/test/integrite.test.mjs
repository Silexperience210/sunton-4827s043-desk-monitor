// ── Intégrité des données sur disque : historique long, état appris, écritures ─
// Chaque section rejoue un constat de la revue du 01/11/2026 sur les vraies classes (lib/history.mjs,
// lib/store.mjs) ou sur le vrai serveur, dans l'ordre exact où le serveur les appelle.
//  M1. redémarrage de nuit : le jour fini est compressé à 5 s, puis le premier relevé (tardif) écrit sa
//      dernière tranche — la journée ne doit pas être réduite à cette seule tranche.
// node online/test/integrite.test.mjs
import { mkdtempSync, rmSync, readdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { History } from '../lib/history.mjs';

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
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`\n${checks} vérifications, ${fails} échec(s)`);
process.exit(fails ? 1 : 0);
