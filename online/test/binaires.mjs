// ── Binaires natifs exigés par les tests (construits depuis sim/, non versionnés) ─
// Un test qui saute une étape faute de binaire et affiche « réussi » ne vérifie rien : il est pire que pas
// de test. Chaque test qui en dépend appelle exigerBinaires() en tête et échoue (code 2) en donnant la
// commande à lancer ; test/tous.mjs (npm test) fait la même vérification avant de commencer.
import { existsSync, readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

export const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
export const BINAIRES = {
  bench_prevision: join(ROOT, 'sim', 'bench_prevision'),
  sync_sim: join(ROOT, 'sim', 'sync_sim'),
  reg_sim: join(ROOT, 'sim', 'reg_sim'),          // le Sunton du camion (rôle régulateur) : test/reg.e2e.mjs
};

export function exigerBinaires(noms = Object.keys(BINAIRES)) {
  const manque = noms.filter((n) => !existsSync(BINAIRES[n]));
  if (!manque.length) return;
  console.log(`ÉCHEC : ${manque.map((n) => `sim/${n}`).join(', ')} absent(s) — aucune vérification lancée.`);
  console.log(`  make -C sim ${manque.join(' ')}   (LVGL_DIR=… AJSON_DIR=… si .deps/ manque : voir sim/Makefile)`);
  process.exit(2);
}

// ── Les moteurs wasm doivent être ceux des sources ──────────────────────────────
// public/*.wasm est livré PRÉCOMPILÉ et le Dockerfile ne le reconstruit pas : une suite verte peut donc valider
// un moteur périmé. Constaté le 03/11/2026 — les 441 vérifications passaient sur le cœur 1.3.0 alors que la
// carte et le serveur étaient en 1.4.0, et les défauts réels du modèle 2 n'apparaissaient qu'après
// reconstruction. On refuse un moteur dont la version gravée n'est pas celle de src/config.h.
export function exigerMoteurs() {
  const cfgPath = join(ROOT, 'src', 'config.h');
  if (!existsSync(cfgPath)) {
    // Arbre sans les sources du firmware (copie de online/ seule, image de déploiement) : la version des moteurs
    // ne peut pas être confrontée à la source. On le dit au lieu de le taire — un contrôle muet ne prouve rien.
    console.log('moteurs wasm : src/config.h absent de cet arbre — version non confrontée aux sources');
    return;
  }
  const cfg = readFileSync(cfgPath, 'utf8');
  const m = cfg.match(/VN_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"/);
  if (!m) { console.log('ÉCHEC : VN_VERSION introuvable dans src/config.h — contrôle du moteur impossible.'); process.exit(2); }
  const attendu = m[1], mauvais = [];
  for (const w of ['vn-core.wasm', 'vn-twin.wasm']) {
    const p = join(ROOT, 'online', 'public', w);
    if (!existsSync(p)) { mauvais.push(`${w} absent`); continue; }
    if (!readFileSync(p).includes(Buffer.from(attendu))) mauvais.push(`${w} ne porte pas ${attendu}`);
  }
  if (!mauvais.length) return;
  console.log(`ÉCHEC : moteur(s) wasm pas à jour des sources ${attendu} : ${mauvais.join(', ')} — la suite testerait un autre code.`);
  console.log('  CLANG=…/bin/clang CLANGXX=…/bin/clang++ bash online/engine/build.sh   (wasi-sdk 22)');
  process.exit(2);
}
