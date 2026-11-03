// ── Binaires natifs exigés par les tests (construits depuis sim/, non versionnés) ─
// Un test qui saute une étape faute de binaire et affiche « réussi » ne vérifie rien : il est pire que pas
// de test. Chaque test qui en dépend appelle exigerBinaires() en tête et échoue (code 2) en donnant la
// commande à lancer ; test/tous.mjs (npm test) fait la même vérification avant de commencer.
import { existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

export const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
export const BINAIRES = {
  bench_prevision: join(ROOT, 'sim', 'bench_prevision'),
  sync_sim: join(ROOT, 'sim', 'sync_sim'),
};

export function exigerBinaires(noms = Object.keys(BINAIRES)) {
  const manque = noms.filter((n) => !existsSync(BINAIRES[n]));
  if (!manque.length) return;
  console.log(`ÉCHEC : ${manque.map((n) => `sim/${n}`).join(', ')} absent(s) — aucune vérification lancée.`);
  console.log(`  make -C sim ${manque.join(' ')}   (LVGL_DIR=… AJSON_DIR=… si .deps/ manque : voir sim/Makefile)`);
  process.exit(2);
}
