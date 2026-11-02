// ── npm test : toute la suite de la version en ligne, en une commande ─
// Lance chaque fichier *.test.mjs et *.e2e.mjs de ce dossier dans son propre processus, l'un après l'autre
// (chacun démarre ses serveurs sur des ports au hasard), puis récapitule. Échoue si un fichier échoue, s'il
// ne publie pas son décompte de vérifications (un test qui ne compte rien ne vérifie rien), ou si un binaire
// natif manque : c'est vérifié avant de commencer, rien n'est sauté en silence.
// node online/test/tous.mjs
import { readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { exigerBinaires } from './binaires.mjs';
import { decompte } from './harnais.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
exigerBinaires();

const files = readdirSync(HERE).filter((f) => /\.(test|e2e)\.mjs$/.test(f)).sort();
const rows = [];
for (const f of files) {
  const t0 = Date.now();
  const r = spawnSync(process.execPath, [join(HERE, f)], { encoding: 'utf8', maxBuffer: 256 << 20 });
  const out = `${r.stdout || ''}${r.stderr || ''}`;
  const d = decompte(out);
  const ok = r.status === 0 && d && d.fails === 0 && d.checks > 0;
  process.stdout.write(`\n── ${f} ${'─'.repeat(Math.max(0, 60 - f.length))}\n${out}`);
  if (r.status === 0 && !d) process.stdout.write('  ÉCHEC : aucun décompte de vérifications publié\n');
  rows.push({ f, ok, d, code: r.status, s: ((Date.now() - t0) / 1000).toFixed(1) });
}
let checks = 0, fails = 0;
console.log('\n── Récapitulatif ──────────────────────────────────────────────');
for (const x of rows) {
  checks += x.d?.checks || 0;
  if (!x.ok) fails++;
  console.log(`  ${x.ok ? 'ok   ' : 'ÉCHEC'} ${x.f.padEnd(26)} ${x.d ? `${x.d.checks - x.d.fails}/${x.d.checks}` : '—'} vérifications · code ${x.code} · ${x.s} s`);
}
console.log(`\n${files.length} fichiers, ${checks} vérifications, ${fails} fichier(s) en échec`);
process.exit(fails ? 1 : 0);
