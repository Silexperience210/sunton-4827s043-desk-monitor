// ── Outils communs du dispositif de test : décompte publié, copie de online/ pour rejouer un test à part ─
import { cpSync, symlinkSync, mkdirSync } from 'node:fs';
import { join, dirname, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
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
  'banc.test.mjs': 14, 'features.test.mjs': 91, 'flux.test.mjs': 9, 'harnais.test.mjs': 10, 'integrite.test.mjs': 80,
  'mutations.test.mjs': 22, 'prevision.test.mjs': 13, 'replication.test.mjs': 75, 'securite.test.mjs': 91,
  'series.test.mjs': 18, 'sync.e2e.mjs': 10,
};
// Raison de l'échec si le fichier a compté moins que son plancher (ou n'en a pas) ; null sinon
export function sousPlancher(file, d, plancher = PLANCHER) {
  if (!(file in plancher)) return `aucun plancher pour ${file} (test/harnais.mjs, PLANCHER)`;
  if (!d || d.checks < plancher[file]) return `${d ? d.checks : 0} vérifications pour un plancher de ${plancher[file]} : un bloc n'a pas tourné`;
  return null;
}

// Copie de online/ dans T/online (sans node_modules, données ni chaîne de compilation) ; T/sim/<binaire> et
// T/docs/donnees en liens vers le dépôt — sauf les binaires de `sans`, pour vérifier qu'un test les exige.
export function copieOnline(T, { sans = [] } = {}) {
  cpSync(ONLINE, join(T, 'online'), { recursive: true, filter: (src) => !/^(node_modules|data|engine[\\/](\.toolchain|build))([\\/]|$)/.test(relative(ONLINE, src)) });
  mkdirSync(join(T, 'sim'));
  for (const [n, b] of Object.entries(BINAIRES)) if (!sans.includes(n)) symlinkSync(b, join(T, 'sim', b.split(sep).at(-1)));
  mkdirSync(join(T, 'docs'));
  symlinkSync(join(ROOT, 'docs', 'donnees'), join(T, 'docs', 'donnees'));
  return T;
}
