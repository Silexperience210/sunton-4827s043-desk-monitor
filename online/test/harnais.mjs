// ── Outils communs du dispositif de test : décompte publié, plancher, sites exécutés, copie de online/ ─
import { cpSync, symlinkSync, mkdirSync, writeFileSync } from 'node:fs';
import { join, dirname, relative, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { BINAIRES, ROOT } from './binaires.mjs';

const ONLINE = join(dirname(fileURLToPath(import.meta.url)), '..');

// Décompte publié par chaque fichier de test, lu par test/tous.mjs (npm test) et les tests du dispositif :
// « 89/89 vérifications réussies » (features) ou « 75 vérifications, 0 échec(s) » (les autres) ; null si rien
// n'est publié : un fichier qui ne compte rien ne vérifie rien.
export function decompte(out) {
  let m = out.match(/(\d+)\/(\d+) vérifications réussies/);
  if (m) return { checks: +m[2], fails: +m[2] - +m[1] };
  m = out.match(/(\d+) vérifications, (\d+) échec/);
  return m ? { checks: +m[1], fails: +m[2] } : null;
}

// Plancher de vérifications par fichier : un bloc sauté sans bruit (condition jamais vraie, branche oubliée)
// fait baisser le décompte sans rien faire échouer — npm test échoue alors. À relever quand on ajoute des
// vérifications ; à baisser seulement en le disant (vérification retirée exprès). Un fichier sans plancher
// échoue aussi : un nouveau test s'inscrit ici.
export const PLANCHER = {
  'banc.test.mjs': 14, 'features.test.mjs': 93, 'flux.test.mjs': 9, 'harnais.test.mjs': 16, 'integrite.test.mjs': 80,
  'mutations.test.mjs': 30, 'prevision.test.mjs': 65, 'replication.test.mjs': 84, 'securite.test.mjs': 102,
  'series.test.mjs': 18, 'sync.e2e.mjs': 10, 'regulation.test.mjs': 67, 'reg.e2e.mjs': 26,
};
// Raison de l'échec si le fichier a compté moins que son plancher (ou n'en a pas) ; null sinon
export function sousPlancher(file, d, plancher = PLANCHER) {
  if (!(file in plancher)) return `aucun plancher pour ${file} (test/harnais.mjs, PLANCHER)`;
  if (!d || d.checks < plancher[file]) return `${d ? d.checks : 0} vérifications pour un plancher de ${plancher[file]} : un bloc n'a pas tourné`;
  return null;
}

// ── Sites de vérification : chaque ok( écrit doit avoir tourné ─
// Le plancher compte un total : un bloc sauté pendant qu'un autre grossit le laisse intact. Ici, la liste vient
// du source du test lui-même (rien à recopier) : chaque appel ok( écrit, hors commentaires et chaînes, doit avoir
// tourné au moins une fois, quoi que fassent les autres. Exception : ok(false, …), qui ne sert qu'à signaler un
// échec et ne tourne donc pas quand tout va bien. Une boucle qui tourne moins de fois reste l'affaire du plancher.
// Le ok( de chaque fichier appelle site() ; npm test (test/tous.mjs) donne VN_SITES, un dossier où chaque
// processus écrit, à sa sortie, <pid>.json : les sites exécutés (« ligne:colonne » → nombre de passages).
const vus = new Map();
export function site() {
  const dir = process.env.VN_SITES;
  if (!dir) return;
  // [0] Error, [1] site, [2] ok, [3] l'appel de ok dans le fichier de test
  const m = (new Error().stack.split('\n')[3] || '').match(/\(?(file:\/\/[^\s()]+):(\d+):(\d+)\)?$/);
  const k = m && m[1] === pathToFileURL(process.argv[1]).href ? `${m[2]}:${m[3]}` : `hors du fichier : ${m ? m[0] : '?'}`;
  if (!vus.size) process.on('exit', () => writeFileSync(join(dir, `${process.pid}.json`), JSON.stringify({ file: process.argv[1], sites: Object.fromEntries(vus) })));
  vus.set(k, (vus.get(k) || 0) + 1);
}

// Le source réduit au code : commentaires, chaînes, gabarits (hors ${…}) et expressions régulières remplacés par
// des espaces, retours à la ligne gardés — les positions ne bougent pas.
function codeSeul(src) {
  const o = src.split('');
  const blank = (a, b) => { for (let k = a; k < b; k++) if (o[k] !== '\n') o[k] = ' '; };
  const tpl = [];            // profondeur d'accolades à laquelle chaque ${ d'un gabarit se referme
  let depth = 0, prev = '', word = '';
  const KW = /^(return|typeof|case|do|else|in|of|new|delete|void|throw|instanceof|yield|await)$/;
  const gabarit = (i) => {   // i : juste après ` ou après la } d'un ${…} ; rend la position de reprise du code
    let j = i;
    while (j < src.length && src[j] !== '`' && !(src[j] === '$' && src[j + 1] === '{')) j += src[j] === '\\' ? 2 : 1;
    blank(i, j);
    if (src[j] === '`') { o[j] = ' '; return j + 1; }
    tpl.push(++depth); return j + 2;
  };
  for (let i = 0; i < src.length;) {
    const c = src[i], d = src[i + 1];
    if (c === '/' && d === '/') { const j = src.indexOf('\n', i); blank(i, j < 0 ? src.length : j); i = j < 0 ? src.length : j; continue; }
    if (c === '/' && d === '*') { const j = src.indexOf('*/', i + 2); const e = j < 0 ? src.length : j + 2; blank(i, e); i = e; continue; }
    if (c === '\'' || c === '"') { let j = i + 1; while (j < src.length && src[j] !== c && src[j] !== '\n') j += src[j] === '\\' ? 2 : 1; blank(i, j + 1); i = j + 1; prev = 'x'; word = ''; continue; }
    if (c === '`') { o[i] = ' '; i = gabarit(i + 1); prev = 'x'; word = ''; continue; }
    if (c === '/' && (!prev || /[(,=:[!&|?{};+\-*%<>~^]/.test(prev) || KW.test(word))) {
      let j = i + 1, cls = false;
      while (j < src.length && src[j] !== '\n' && (cls || src[j] !== '/')) { if (src[j] === '\\') j++; else if (src[j] === '[') cls = true; else if (src[j] === ']') cls = false; j++; }
      blank(i, j + 1); i = j + 1; while (/[a-z]/.test(src[i] || '')) o[i++] = ' ';
      prev = 'x'; word = ''; continue;
    }
    if (c === '{') depth++;
    if (c === '}') { if (tpl.length && tpl.at(-1) === depth) { tpl.pop(); depth--; o[i] = ' '; i = gabarit(i + 1); prev = 'x'; word = ''; continue; } depth--; }
    if (!/\s/.test(c)) { prev = c; word = /[\w$]/.test(c) ? word + c : ''; }
    i++;
  }
  return o.join('');
}

// Sites ok( écrits dans le source : ligne et colonne (comptées comme dans une pile d'appels), ok(false, …) à part,
// et le bloc où ils sont — le dernier titre console.log('…') en tête de ligne qui les précède.
export function sitesSource(src) {
  const code = codeSeul(src), starts = [0];
  for (let i = 0; i < src.length; i++) if (src[i] === '\n') starts.push(i + 1);
  const lineOf = (i) => { let a = 0, b = starts.length - 1; while (a < b) { const m = (a + b + 1) >> 1; if (starts[m] <= i) a = m; else b = m - 1; } return a; };
  const titres = src.split('\n').map((l) => (l.match(/^\s*console\.log\((['`])([^\s'`].*?)\1/) || [])[2]);
  const out = [];
  for (const m of code.matchAll(/(?<![\w$.])ok\s*\(/g)) {
    const l = lineOf(m.index);
    let t = l; while (t >= 0 && !titres[t]) t--;
    out.push({ at: `${l + 1}:${m.index - starts[l] + 1}`, ligne: l + 1, echecSeul: /^ok\s*\(\s*false\s*[,)]/.test(code.slice(m.index)), bloc: t >= 0 ? titres[t].slice(0, 70) : '(avant le premier titre)' });
  }
  return out;
}

// Raison de l'échec si un ok( écrit n'a pas tourné, ou si un ok( a tourné hors des sites repérés ; null sinon.
// rec : { sites } écrit par site(), ou null si le fichier n'a rien enregistré.
export function sitesNonExecutes(src, rec) {
  if (!rec) return 'aucun site de vérification enregistré : son ok( doit appeler site() (test/harnais.mjs)';
  const ecrits = sitesSource(src), connus = new Set(ecrits.map((s) => s.at));
  const inconnus = Object.keys(rec.sites).filter((k) => !connus.has(k));
  if (inconnus.length) return `ok( exécuté hors des sites repérés dans le source (${inconnus.slice(0, 5).join(' ; ')}) : analyse du source à corriger (test/harnais.mjs)`;
  const manquent = ecrits.filter((s) => !s.echecSeul && !rec.sites[s.at]);
  if (!manquent.length) return null;
  const parBloc = new Map();
  for (const s of manquent) parBloc.set(s.bloc, [...(parBloc.get(s.bloc) || []), s.ligne]);
  return `${manquent.length} vérification(s) écrite(s) jamais exécutée(s) — ${[...parBloc].map(([b, ls]) => `« ${b} » : ligne(s) ${ls.join(', ')}`).join(' ; ')}`;
}

// Copie de online/ dans T/online (sans node_modules, données ni chaîne de compilation) ; T/sim/<binaire>,
// T/docs/donnees et T/.github (lu par harnais.test.mjs) en liens vers le dépôt — sauf les binaires de `sans`, pour
// vérifier qu'un test les exige.
export function copieOnline(T, { sans = [] } = {}) {
  cpSync(ONLINE, join(T, 'online'), { recursive: true, filter: (src) => !/^(node_modules|data|engine[\\/](\.toolchain|build))([\\/]|$)/.test(relative(ONLINE, src)) });
  mkdirSync(join(T, 'sim'));
  for (const [n, b] of Object.entries(BINAIRES)) if (!sans.includes(n)) symlinkSync(b, join(T, 'sim', b.split(sep).at(-1)));
  mkdirSync(join(T, 'docs'));
  symlinkSync(join(ROOT, 'docs', 'donnees'), join(T, 'docs', 'donnees'));
  symlinkSync(join(ROOT, '.github'), join(T, '.github'));
  return T;
}
