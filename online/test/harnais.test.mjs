// ── Le dispositif de test lui-même ───────────────────────────────────
// La revue (RAPPORT-REVUE.md §5) a trouvé des tests vacants : features.test.mjs sautait 30 vérifications sans
// sim/bench_prevision en affichant « 59/59 vérifications réussies », npm test ne lançait pas ce fichier, et la CI
// ne lançait que trois fichiers sur dix. Vérifié ici, sur des copies de online/ :
//  - un test qui dépend d'un binaire natif échoue (code 2, message) quand il manque, sans rien déclarer réussi ;
//  - un fichier qui compte moins de vérifications que son plancher (test/harnais.mjs) fait échouer npm test ;
//  - un ok( écrit qui n'a pas tourné fait échouer npm test, même quand un autre bloc a grossi d'autant et que le
//    total atteint le plancher (series.test.mjs muté, lancé par le vrai test/tous.mjs) ; l'analyse du source ;
//  - npm test (test/tous.mjs) refuse de commencer sans les binaires ;
//  - la CI construit les deux binaires puis lance npm test, après la reconstruction des moteurs WebAssembly.
// node online/test/harnais.test.mjs
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { spawnSync } from 'node:child_process';
import { ROOT } from './binaires.mjs';
import { copieOnline, decompte, sousPlancher, PLANCHER, site, sitesSource, sitesNonExecutes } from './harnais.mjs';

let fails = 0, checks = 0;
const ok = (c, msg) => { checks++; site(); console.log(`  ${c ? 'ok   ' : 'ÉCHEC'} ${msg}`); if (!c) fails++; };
const work = mkdtempSync(join(tmpdir(), 'vn-harnais-'));
let n = 0;
const run = (T, file, args = []) => {
  const r = spawnSync(process.execPath, [join(T, 'online', 'test', file), ...args], { cwd: T, encoding: 'utf8', timeout: 60000 });
  return { code: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
};

try {
  console.log('binaire natif absent : échec explicite, rien de déclaré réussi');
  for (const [file, sans] of [['features.test.mjs', ['bench_prevision']], ['features.test.mjs', ['sync_sim']], ['sync.e2e.mjs', ['sync_sim']], ['reg.e2e.mjs', ['reg_sim']],
    ['tous.mjs', ['bench_prevision', 'sync_sim', 'reg_sim']]]) {
    const r = run(copieOnline(join(work, `c${++n}`), { sans }), file);
    const named = sans.every((b) => r.out.includes(`sim/${b}`));
    ok(r.code === 2 && /ÉCHEC/.test(r.out) && named && !/vérifications réussies|vérifications, 0 échec/.test(r.out),
      `${file} sans ${sans.map((b) => `sim/${b}`).join(' ni ')} : code ${r.code}, « ${r.out.split('\n')[0].trim()} »`);
  }

  console.log('un bloc sauté sans bruit fait échouer npm test (plancher par fichier)');
  // revue §5.1 : sans le binaire du banc, features.test.mjs affichait « 59/59 vérifications réussies » pour 89
  const low = sousPlancher('features.test.mjs', decompte('59/59 vérifications réussies'));
  ok(new RegExp(`59 vérifications pour un plancher de ${PLANCHER['features.test.mjs']}`).test(low || '') && PLANCHER['features.test.mjs'] > 59, `features à 59/59 : ${low}`);
  ok(sousPlancher('nouveau.test.mjs', decompte('3 vérifications, 0 échec(s)')) !== null && Object.keys(PLANCHER).every((f) => sousPlancher(f, { checks: PLANCHER[f], fails: 0 }) === null),
    'fichier sans plancher refusé ; chaque plancher atteint exactement passe');

  console.log('un bloc sauté pendant qu\'un autre grossit d\'autant fait échouer npm test (chaque ok( écrit doit tourner)');
  // series.test.mjs : 18 vérifications, 4 dans le bloc « heure de Luxembourg », 7 par tour dans la boucle m8 (2 tours).
  // Copie mutée lancée par le vrai test/tous.mjs de la copie ; le texte à muter doit y être, sinon échec.
  const muter = (T, edits) => {
    const f = join(T, 'online', 'test', 'series.test.mjs');
    let src = readFileSync(f, 'utf8');
    for (const [from, to] of edits) { if (src.split(from).length !== 2) return false; src = src.replace(from, to); }
    writeFileSync(f, src);
    return true;
  };
  const LUX = 'console.log(\'heure de Luxembourg : hiver, été, et les deux nuits de changement\');\n', M8 = '\nfor (const [what, day, from, to, len] of [';
  const GROSSIT = ['  const ds = dailySummary(', '  ok(true, \'ajoutée 1\'); ok(true, \'ajoutée 2\');\n  const ds = dailySummary('];
  const scenarios = [
    ['copie intacte (témoin)', [], null],
    ['bloc « heure de Luxembourg » sauté (−4), bloc m8 grossi de 2 par tour (+4)', [[LUX, `${LUX}if (false) {\n`], [M8, `\n}${M8}`], GROSSIT],
      /4 vérification\(s\) écrite\(s\) jamais exécutée\(s\) — « heure de Luxembourg : hiver, été, et les deux nuits de changement » : ligne\(s\) 25, 26, 27, 28/],
    ['une vérification de m8 sautée (−1 par tour), une autre ajoutée au même bloc (+1 par tour)',
      [['  ok(d.cover.every((c) => c === 1),', '  if (false) ok(d.cover.every((c) => c === 1),'], [GROSSIT[0], `  ok(true, 'ajoutée');\n${GROSSIT[0]}`]],
      /1 vérification\(s\) écrite\(s\) jamais exécutée\(s\) — « m8\. \$\{what\} : pas de 1440 et 180 min » : ligne\(s\) 40$/],
    ['ok( qui n\'appelle plus site()', [['checks++; site(); ', 'checks++; ']], /aucun site de vérification enregistré/],
  ];
  for (const [what, edits, attendu] of scenarios) {
    const T = copieOnline(join(work, `c${++n}`));
    if (!muter(T, edits)) { ok(false, `${what} : texte à muter introuvable dans series.test.mjs — scénario à réécrire`); continue; }
    const r = run(T, 'tous.mjs', ['series.test.mjs']);
    const d = decompte(r.out.split('── Récapitulatif')[0]), raison = (r.out.match(/ÉCHEC : ([^\n]*)/) || [])[1];
    if (!attendu) ok(r.code === 0 && d?.checks === PLANCHER['series.test.mjs'] && !raison, `${what} : code ${r.code}, ${d?.checks} vérifications`);
    else ok(r.code === 1 && d?.checks === PLANCHER['series.test.mjs'] && d.fails === 0 && sousPlancher('series.test.mjs', d) === null && attendu.test(raison || ''),
      `${what} : ${d?.checks}/${PLANCHER['series.test.mjs']} vérifications, plancher atteint, et pourtant code ${r.code} : « ${raison} »`);
  }
  // L'analyse du source : commentaires, chaînes, gabarits et expressions régulières ignorés ; ok(false, …) à part
  const src = [
    'console.log(\'bloc A\');',
    'ok(1, \'a\'); // ok(commentaire)',
    '/* ok(bloc de commentaire) */ x.ok(2); took(3); const s = "ok(chaîne)", r = /\'ok\\(/;',
    'console.log(\'bloc B\');',
    'ok(`gabarit ${ok(4, \'imbriqué\')} ok(texte)`, `${{ a: 1 }.a}`); if (r) ok(false, \'échec seul\');',
    '  const q = a / b; ok(5, \'après une division\');',
  ].join('\n');
  const vus = sitesSource(src).map((s) => `${s.at}${s.echecSeul ? ' (échec seul)' : ''} ${s.bloc}`);
  ok(JSON.stringify(vus) === JSON.stringify(['2:1 bloc A', '5:1 bloc B', '5:15 bloc B', '5:71 (échec seul) bloc B', '6:20 bloc B']), `sites repérés : ${vus.join(' · ')}`);
  ok(sitesNonExecutes(src, { sites: { '2:1': 1, '5:1': 1, '5:15': 1, '6:20': 3 } }) === null
    && /« bloc B » : ligne\(s\) 6$/.test(sitesNonExecutes(src, { sites: { '2:1': 1, '5:1': 1, '5:15': 1 } }) || '')
    && /hors des sites repérés dans le source \(9:1\)/.test(sitesNonExecutes(src, { sites: { '2:1': 1, '5:1': 1, '5:15': 1, '6:20': 1, '9:1': 1 } }) || ''),
  'tous les sites exécutés : accepté ; un site manquant : nommé avec son bloc ; un site exécuté inconnu de l\'analyse : refusé');

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
