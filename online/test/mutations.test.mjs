// ── Mutations de la revue : chacune doit faire échouer un test ─────────
// La revue (RAPPORT-REVUE.md §5) a cassé le code de plusieurs façons sans qu'aucun test ne bouge : n'importe
// quel jeton accepté, écritures ouvertes, atomicWrite direct, notation des prévisions faussée, heure d'été
// supprimée, CRC32 des envois ignoré, tranche en cours importée… Chacune est rejouée ici sur une copie de
// online/ : le fichier de test désigné doit échouer sur au moins une vérification (un plantage ne compte pas),
// et passer sur la copie intacte (témoin). Si le texte à muter n'est plus dans le code, c'est un échec aussi :
// la mutation est à réécrire sur le nouveau code, pas à oublier.
// node online/test/mutations.test.mjs [identifiant…]
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir, availableParallelism } from 'node:os';
import { spawn } from 'node:child_process';
import { exigerBinaires } from './binaires.mjs';
import { decompte, copieOnline } from './harnais.mjs';

exigerBinaires();

// from : texte exact du code actuel (n occurrences, 1 par défaut), remplacé partout par to
const MUTATIONS = [
  // §5.3, tableau : vacantes même avec tous les binaires construits
  { id: 'bearer', what: 'n\'importe quel Bearer accepté', file: 'lib/acces.mjs',
    from: 'if (bearer && sameSecret(bearer, token)) return true;', to: 'if (bearer) return true;', tests: ['securite.test.mjs'] },
  { id: 'x-vn-token', what: 'n\'importe quel X-VN-Token accepté', file: 'lib/acces.mjs',
    from: 'return !!(alt && sameSecret(alt, token));', to: 'return !!alt;', tests: ['securite.test.mjs'] },
  { id: 'ecritures', what: '/api/bench/run et toutes les écritures ouvertes sans jeton', file: 'lib/acces.mjs',
    from: 'if (write && !open && !isLocal(req)) return', to: 'if (false) return', tests: ['securite.test.mjs'] },
  { id: 'atomic', what: 'atomicWrite = writeFileSync direct', file: 'lib/store.mjs',
    from: 'export function atomicWrite(path, data) {\n', to: 'export function atomicWrite(path, data) {\n  return writeFileSync(path, data);\n', tests: ['integrite.test.mjs'] },
  { id: 'note-15', what: 'prévision +15 notée à tous les horizons (v[1])', file: 'lib/forecast.mjs',
    from: 'model[h].add(v[1 + h] - tv);', to: 'model[h].add(v[1] - tv);', tests: ['prevision.test.mjs'] },
  { id: 'note-heure-station', what: 'MAE par heure et par station faussées (v[0])', file: 'lib/forecast.mjs',
    from: '[0].add(v[1 + h] - tv)', n: 2, to: '[0].add(v[0] - tv)', tests: ['prevision.test.mjs'] },
  { id: 'heure-ete', what: 'heure d\'été supprimée (Etc/GMT-1)', file: 'public/vn-series.js',
    from: 'timeZone: \'Europe/Luxembourg\', year:', to: 'timeZone: \'Etc/GMT-1\', year:', tests: ['series.test.mjs'] },
  { id: 'crc-envoi', what: 'CRC32 des envois des cartes ignoré', file: 'server.mjs',
    from: 'if (crcHdr && parseInt(String(crcHdr), 16) >>> 0 !== crc) return', to: 'if (false) return', tests: ['securite.test.mjs'] },
  { id: 'tranche-en-cours', what: 'tranche en cours d\'un instantané importée (back = 0)', file: 'lib/history.mjs',
    from: 'for (let back = t - head >= SLOT_S ? 0 : 1; back < 400; back++)', to: 'for (let back = 0; back < 400; back++)', tests: ['integrite.test.mjs'] },
  // §5.1 et §5.2 : notation cassée, verte sans le binaire du banc ou hors de npm test
  { id: 'note-modele', what: 'prévision notée comme la persistance (v[0])', file: 'lib/forecast.mjs',
    from: 'model[h].add(v[1 + h] - tv);', to: 'model[h].add(v[0] - tv);', tests: ['prevision.test.mjs'] },
  { id: 'note-profil', what: 'profil seul noté comme la persistance (v[0])', file: 'lib/forecast.mjs',
    from: 'prof[h].add(v[6 + h] - tv);', to: 'prof[h].add(v[0] - tv);', tests: ['prevision.test.mjs'] },
  { id: 'jeton-ignore', what: 'authorized() toujours vrai : jeton jamais exigé', file: 'lib/acces.mjs',
    from: 'if (!tokenOk(req, token)) return', to: 'if (false) return', tests: ['securite.test.mjs'] },
  { id: 'banc-lance', what: '/api/bench/run répond sans lancer le banc', file: 'server.mjs',
    from: 'bench.run(dayOf(nowS())).catch(', to: 'Promise.resolve().catch(', tests: ['features.test.mjs'] },
  { id: 'banc-rapport', what: 'rapport du banc mal lu (modèle et profil seul échangés)', file: 'lib/bench.mjs',
    from: '.map((c) => c.split(\'/\').map((x) => num(x.trim())));', to: '.map((c) => c.split(\'/\').reverse().map((x) => num(x.trim())));', tests: ['banc.test.mjs'] },
  // les deux bloquants de la revue : leur garde ne doit pas pouvoir disparaître sans bruit
  { id: 'csrf', what: 'B1 : requête d\'une autre origine acceptée sans jeton', file: 'lib/acces.mjs',
    from: 'if (crossOrigin(req)) return', to: 'if (false) return', tests: ['securite.test.mjs'] },
  { id: 'plausible', what: 'B2 : profil ou résumés forgés adoptés (aucun contrôle de vraisemblance)', file: 'server.mjs',
    from: 'const implausible = name === \'vn_prof.bin\' ? profImplausible(body, nowS()) : daysImplausible(body, nowS());', to: 'const implausible = null;', tests: ['securite.test.mjs'] },
];

let fails = 0, checks = 0;
const ok = (c, msg) => { checks++; console.log(`  ${c ? 'ok   ' : 'ÉCHEC'} ${msg}`); if (!c) fails++; };
const only = process.argv.slice(2);
const chosen = only.length ? MUTATIONS.filter((m) => only.includes(m.id)) : MUTATIONS;
const work = mkdtempSync(join(tmpdir(), 'vn-mutations-'));

const copy = (name) => copieOnline(join(work, name));
function run(T, test) {
  return new Promise((resolve) => {
    const p = spawn(process.execPath, [join(T, 'online', 'test', test)], { cwd: T, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    p.stdout.on('data', (d) => { out += d; });
    p.stderr.on('data', (d) => { out += d; });
    const timer = setTimeout(() => p.kill('SIGKILL'), 180000);
    p.on('close', (code, signal) => { clearTimeout(timer); resolve({ code, signal, out, d: decompte(out) }); });
  });
}
async function pool(jobs, width) {
  const res = new Array(jobs.length);
  let next = 0;
  await Promise.all(Array.from({ length: Math.min(width, jobs.length) }, async () => {
    while (next < jobs.length) { const i = next++; res[i] = await jobs[i](); }
  }));
  return res;
}
const firstFail = (out) => (out.match(/ÉCHEC[^\n]*/) || [''])[0].slice(0, 160);

console.log(`Mutations de la revue (${chosen.length}), chacune sur sa copie de online/`);
const temoins = [...new Set(chosen.flatMap((m) => m.tests))];
const jobs = [
  // témoin : la copie intacte passe chaque fichier désigné
  ...temoins.map((test) => async () => ({ temoin: test, r: await run(copy(`temoin-${test}`), test) })),
  ...chosen.map((m) => async () => {
    const T = copy(m.id);
    const f = join(T, 'online', m.file);
    const src = readFileSync(f, 'utf8');
    const found = src.split(m.from).length - 1;
    if (found !== (m.n || 1)) return { m, found };
    writeFileSync(f, src.split(m.from).join(m.to));
    const rs = [];
    for (const test of m.tests) rs.push({ test, ...(await run(T, test)) });
    return { m, found, rs };
  }),
];
const width = Math.max(2, Math.min(6, Math.floor(availableParallelism() / 2)));
const t0 = Date.now();
for (const x of await pool(jobs, width)) {
  if (x.temoin) {
    const { r } = x;
    ok(r.code === 0 && r.d && r.d.checks > 0 && r.d.fails === 0, `témoin : ${x.temoin} passe sur la copie intacte (${r.d ? `${r.d.checks - r.d.fails}/${r.d.checks}` : 'aucun décompte'}, code ${r.code})`);
    continue;
  }
  const { m, found, rs } = x;
  if (!rs) { ok(false, `${m.id} — ${m.what} : texte à muter trouvé ${found} fois dans ${m.file} (${m.n || 1} attendu) — mutation à réécrire`); continue; }
  const caught = rs.filter((r) => r.code !== 0 && r.d && r.d.fails > 0);
  const how = rs.map((r) => `${r.test} : ${r.d ? `${r.d.fails} échec(s)` : r.signal ? `tué (${r.signal})` : `plantage, code ${r.code}`}`).join(' · ');
  ok(caught.length > 0, `${m.id} — ${m.what} : ${caught.length ? 'attrapée' : 'NON ATTRAPÉE'} (${how})`);
  if (caught.length) console.log(`         ${firstFail(caught[0].out)}`);
  else for (const r of rs) console.log(r.out.split('\n').slice(-12).map((l) => `         ${l}`).join('\n'));
}
console.log(`   ${((Date.now() - t0) / 1000).toFixed(1)} s, ${width} à la fois`);
rmSync(work, { recursive: true, force: true });
console.log(`\n${checks} vérifications, ${fails} échec(s)`);
process.exit(fails ? 1 : 0);
