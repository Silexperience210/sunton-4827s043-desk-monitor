// ── Banc de nuit (lib/bench.mjs) : ce que le serveur garde des sorties du banc ─
// Banc factice (script shell) : aucun binaire natif, rien ne sort de la machine. Rejoue un constat de la revue
// du 01/11/2026 :
//  m13. la sortie d'erreur du banc était accumulée sans limite (err += d) : un banc bavard ou en boucle
//       remplissait la mémoire du serveur, et le rapport Markdown (qui la reçoit quand la sortie standard est
//       vide) en gardait tout. Bornée : le début et la fin (la cause d'un échec est à la fin), coupure signalée.
// Et la lecture du rapport (parseBenchReport) sur un extrait du rapport de production du 01/11/2026, valeur par
// valeur : features.test.mjs ne compare le banc au journal du serveur qu'à 0,08 près, et modèle et profil seul y
// sont trop proches pour qu'un échange des colonnes se voie (revue §5.1, mutation du rapport du banc).
// node online/test/banc.test.mjs
import { mkdtempSync, rmSync, writeFileSync, chmodSync, mkdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { NightBench, parseBenchReport } from '../lib/bench.mjs';
import { site } from './harnais.mjs';

let fails = 0, checks = 0;
// VN_TEST_DETAIL=1 : chaque vérification réussie est aussi affichée (preuves du rapport de correctifs)
const ok = (c, msg) => { checks++; site(); if (!c) { fails++; console.log(`  ÉCHEC : ${msg}`); } else if (process.env.VN_TEST_DETAIL) console.log(`  ok : ${msg}`); };
const tmp = mkdtempSync(join(tmpdir(), 'vn-banc-'));

try {
  console.log('m13. sortie d\'erreur du banc : bornée en mémoire et dans le rapport');
  {
    // archive minimale : deux instantanés (le banc factice ne les lit pas)
    const A = join(tmp, 'archive');
    for (const d of ['20261101-060000', '20261101-180000']) {
      mkdirSync(join(A, d), { recursive: true });
      for (const f of ['vn_prof.bin', 'vn_hist.bin']) writeFileSync(join(A, d, f), 'x');
    }
    // 8 Mo sur la sortie d'erreur, rien sur la sortie standard, puis la cause de l'échec, code 3
    const bin = join(tmp, 'banc.sh');
    writeFileSync(bin, '#!/bin/sh\nyes "bench_prevision : avertissement répété" | head -c 8000000 >&2\necho "fin : erreur fatale (cause)" >&2\nexit 3\n');
    chmodSync(bin, 0o755);
    const nb = new NightBench({ dataDir: join(tmp, 'data'), archiveDir: A, bin });
    const j = await nb.run('2026-11-02');
    const md = readFileSync(join(tmp, 'data', 'banc', '2026-11-02.md'), 'utf8');
    ok(j.code === 3 && j.ok === false, `banc lancé, échec rapporté (code ${j.code}, ok ${j.ok})`);
    ok(md.length <= 70000, `rapport écrit : ${md.length} caractères pour 8 000 000 émis (64 Kio au plus, plus la marque de coupure)`);
    ok(/fin : erreur fatale \(cause\)/.test(md), 'la fin de la sortie d\'erreur (la cause de l\'échec) est gardée');
    ok(/caractères de la sortie d'erreur omis/.test(md), 'la coupure est signalée dans le rapport');
    ok(md.startsWith('bench_prevision : avertissement répété'), 'le début de la sortie d\'erreur est gardé');
    ok(typeof j.stderr === 'string' && j.stderr.length <= 2000, `extrait JSON : ${j.stderr?.length} caractères (2000 au plus)`);
  }

  console.log('§5.1. rapport du banc : chaque valeur lue à sa place (extrait du rapport de production du 01/11/2026)');
  {
    const md = [
      '# Banc de prévision sur DONNÉES RÉELLES', '', '## 0. Ce qui a été lu', '',
      '- 11 historique(s) lu(s), 0 refusé(s) ; 1557 tranches de 5 min, du 26/10/2026 08:20 au 31/10/2026 18:00 (5.4 jours, 100 % des tranches présentes)',
      '', '## 1. Prévision : MAE (vélos) du chiffre affiché — modèle / persistance / profil seul (sans tendance)', '',
      'Jours évaluables (≥ 12 h relevées) : 5 — dont 4 ouvrés, 1 samedis/dimanches, 0 fériés', '',
      '| échantillon | +15 min | +30 min | +60 min | +120 min | +240 min | n (+60) |',
      '|---|---|---|---|---|---|---|',
      '| tous les jours | 0.654 / 0.545 / 0.578 | 1.001 / 0.864 / 0.881 | 1.415 / 1.318 / 1.274 | 1.890 / 1.897 / 1.755 | 2.380 / 2.529 / 2.252 | 43115 |',
      '| jours ouvrés | 0.676 / 0.564 / 0.602 | 1.024 / 0.893 / 0.914 | 1.439 / 1.359 / 1.307 | 1.921 / 1.957 / 1.792 | 2.379 / 2.562 / 2.249 | 36680 |',
      '| samedis, dimanches | 0.536 / 0.446 / 0.446 | 0.877 / 0.704 / 0.704 | 1.279 / 1.084 / 1.084 | 1.698 / 1.523 / 1.523 | 2.389 / 2.275 / 2.275 | 6435 |',
      '', 'Par jour (+60 min, modèle / persistance) : lun 26/10 1.41/1.16 ; mar 27/10 1.49/1.34 ; mer 28/10 1.43/1.36 ; jeu 29/10 1.41/1.37 ; ven 30/10 1.43/1.36 ; sam 31/10 1.28/1.08 ;',
      '', '## 2. Comparaisons appariées (mêmes données, écart par jour, erreur-type entre jours)', '',
      '1. **Le terme de tendance aide-t-il ?** (simulation : non — profil seul 1,060 contre 1,087 à +60 min)  ',
      '   MAE +60 modèle − profil seul, jours non fériés : **trop court pour conclure** (5 jours ; il en faut au moins 14)',
      '2. **La prévision bat-elle la persistance ?** MAE +60 modèle − persistance : **trop court pour conclure** (5 jours ; il en faut au moins 14)',
      '3. **Les jours fériés sont-ils le pire cas ?** (simulation : MAE +60 1,31 contre 0,97 pour la persistance)  ',
      '   **non mesurable : aucun jour férié évaluable dans l\'historique.**', '', '## 3. Alertes « inhabituel » contre la vérité construite', '',
    ].join('\n');
    const j = parseBenchReport(md);
    const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
    ok(same(j.rows.map((r) => [r.name, r.n60]), [['tous les jours', 43115], ['jours ouvrés', 36680], ['samedis, dimanches', 6435]]), `lignes et effectifs : ${JSON.stringify(j.rows.map((r) => [r.name, r.n60]))}`);
    ok(same(j.rows[0].h, [[0.654, 0.545, 0.578], [1.001, 0.864, 0.881], [1.415, 1.318, 1.274], [1.89, 1.897, 1.755], [2.38, 2.529, 2.252]]),
      `tous les jours, cinq horizons, modèle / persistance / profil seul : ${JSON.stringify(j.rows[0].h)}`);
    ok(same(j.rows[2].h[2], [1.279, 1.084, 1.084]), `samedis et dimanches à +60 min : ${JSON.stringify(j.rows[2].h[2])}`);
    ok(j.evalDays === 5, `jours évaluables : ${j.evalDays}`);
    ok(same(j.perDay.map((d) => [d.day, d.holiday, d.model, d.per]), [['lun 26/10', false, 1.41, 1.16], ['mar 27/10', false, 1.49, 1.34], ['mer 28/10', false, 1.43, 1.36],
      ['jeu 29/10', false, 1.41, 1.37], ['ven 30/10', false, 1.43, 1.36], ['sam 31/10', false, 1.28, 1.08]]), `par jour, modèle / persistance : ${JSON.stringify(j.perDay)}`);
    ok(j.verdicts.length === 3 && j.verdicts[1].q === 'La prévision bat-elle la persistance ?' && /trop court pour conclure/.test(j.verdicts[1].text), `verdicts : ${JSON.stringify(j.verdicts.map((v) => v.q))}`);
    ok(same(j.span, { slots: 1557, from: '26/10/2026 08:20', to: '31/10/2026 18:00', days: 5.4, coverPct: 100 }), `période lue : ${JSON.stringify(j.span)}`);
    ok(!j.leak && !j.empty, 'ni fuite ni rapport vide');
  }
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`\n${checks} vérifications, ${fails} échec(s)`);
process.exit(fails ? 1 : 0);
