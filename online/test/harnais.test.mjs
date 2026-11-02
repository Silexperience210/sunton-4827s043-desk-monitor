// ── Le dispositif de test lui-même ───────────────────────────────────
// La revue (RAPPORT-REVUE.md §5) a trouvé des tests vacants : features.test.mjs sautait 30 vérifications sans
// sim/bench_prevision en affichant « 59/59 vérifications réussies », npm test ne lançait pas ce fichier, et la CI
// ne lançait que trois fichiers sur dix. Vérifié ici, sur des copies de online/ :
//  - un test qui dépend d'un binaire natif échoue (code 2, message) quand il manque, sans rien déclarer réussi ;
//  - npm test (test/tous.mjs) refuse de commencer sans les binaires ;
//  - la CI construit les deux binaires puis lance npm test, après la reconstruction des moteurs WebAssembly.
// node online/test/harnais.test.mjs
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { spawnSync } from 'node:child_process';
import { ROOT } from './binaires.mjs';
import { copieOnline } from './harnais.mjs';

let fails = 0, checks = 0;
const ok = (c, msg) => { checks++; console.log(`  ${c ? 'ok   ' : 'ÉCHEC'} ${msg}`); if (!c) fails++; };
const work = mkdtempSync(join(tmpdir(), 'vn-harnais-'));
let n = 0;
const run = (T, file) => {
  const r = spawnSync(process.execPath, [join(T, 'online', 'test', file)], { cwd: T, encoding: 'utf8', timeout: 60000 });
  return { code: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
};

try {
  console.log('binaire natif absent : échec explicite, rien de déclaré réussi');
  for (const [file, sans] of [['features.test.mjs', ['bench_prevision']], ['features.test.mjs', ['sync_sim']], ['sync.e2e.mjs', ['sync_sim']], ['tous.mjs', ['bench_prevision', 'sync_sim']]]) {
    const r = run(copieOnline(join(work, `c${++n}`), { sans }), file);
    const named = sans.every((b) => r.out.includes(`sim/${b}`));
    ok(r.code === 2 && /ÉCHEC/.test(r.out) && named && !/vérifications réussies|vérifications, 0 échec/.test(r.out),
      `${file} sans ${sans.map((b) => `sim/${b}`).join(' ni ')} : code ${r.code}, « ${r.out.split('\n')[0].trim()} »`);
  }

  console.log('npm test lance toute la suite');
  const pkg = JSON.parse(readFileSync(join(ROOT, 'online', 'package.json'), 'utf8'));
  ok(pkg.scripts?.test === 'node test/tous.mjs', `package.json : "test": "${pkg.scripts?.test}"`);

  console.log('la CI construit les binaires et lance npm test');
  const yml = readFileSync(join(ROOT, '.github', 'workflows', 'build.yml'), 'utf8').split('\n');
  const from = yml.findIndex((l) => /^ {2}en-ligne:/.test(l));
  const end = yml.findIndex((l, i) => i > from && /^ {2}\S/.test(l));
  const job = yml.slice(from, end < 0 ? undefined : end);
  const at = (re) => job.findIndex((l) => re.test(l));
  const wasm = at(/run: bash online\/engine\/build\.sh/), make = at(/make -C sim .*\bsync_sim\b.*\bbench_prevision\b|make -C sim .*\bbench_prevision\b.*\bsync_sim\b/);
  const npm = at(/run: npm test\s*$/), wd = at(/working-directory: online\s*$/);
  ok(from >= 0 && wasm >= 0 && make > wasm, `job en-ligne : moteurs WebAssembly reconstruits (ligne ${wasm}), puis sync_sim et bench_prevision construits (ligne ${make})`);
  ok(npm > make && wd > make && wd < npm && npm - wd <= 2, `puis npm test dans online/ (ligne ${npm}, working-directory ligne ${wd})`);
  const single = job.filter((l) => /node online\/test\/\S+\.mjs/.test(l)).map((l) => l.trim());
  ok(!single.length, `aucun fichier de test lancé à part, hors de npm test${single.length ? ` : ${single.join(' ; ')}` : ''}`);
} finally {
  rmSync(work, { recursive: true, force: true });
}
console.log(`\n${checks} vérifications, ${fails} échec(s)`);
process.exit(fails ? 1 : 0);
