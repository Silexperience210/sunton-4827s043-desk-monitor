// ── Banc de nuit (lib/bench.mjs) : ce que le serveur garde des sorties du banc ─
// Banc factice (script shell) : aucun binaire natif, rien ne sort de la machine. Rejoue un constat de la revue
// du 01/11/2026 :
//  m13. la sortie d'erreur du banc était accumulée sans limite (err += d) : un banc bavard ou en boucle
//       remplissait la mémoire du serveur, et le rapport Markdown (qui la reçoit quand la sortie standard est
//       vide) en gardait tout. Bornée : le début et la fin (la cause d'un échec est à la fin), coupure signalée.
// node online/test/banc.test.mjs
import { mkdtempSync, rmSync, writeFileSync, chmodSync, mkdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { NightBench } from '../lib/bench.mjs';

let fails = 0, checks = 0;
// VN_TEST_DETAIL=1 : chaque vérification réussie est aussi affichée (preuves du rapport de correctifs)
const ok = (c, msg) => { checks++; if (!c) { fails++; console.log(`  ÉCHEC : ${msg}`); } else if (process.env.VN_TEST_DETAIL) console.log(`  ok : ${msg}`); };
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
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`\n${checks} vérifications, ${fails} échec(s)`);
process.exit(fails ? 1 : 0);
