# Plancher de retrait : lire le pic du matin sur DEMAIN, et l'appliquer à tous les retraits

Branche `fix/plancher-retrait`, partie de `main` (1.7.1). **Aucune migration de fichier** : les
formats (`/vn_prof.bin`, `/vn_hist.bin`, `/vn_days.bin`, `/settings.bin`) sont inchangés.

## Ce qui était faux

1. **Le plancher lisait 8 h au lieu du pic du matin.** Sur une station qui se vide le matin, 8 h
   arrive *après* la première heure de départs. Mesuré sur l'historique réel du serveur, 6 jours
   ouvrés (3 → 10/11/2026, station ouverte, relevés de 5 min) :

   | Station | médiane 6 h → 9 h | niveau à 6 h | niveau à 8 h |
   |---|---|---|---|
   | Leon XIII | −3 | 4 … 14 | 4 … 13 |
   | Gare Centrale 2 | **−12** | 13 … 20 | **1 … 15** |
   | Bei der Auer | −5 | 5 … 10 | 2 … 6 |
   | Schluechthaus | −2 | 2 … 6 | 0 … 12 |

   Les quatre perdent **16 à 24 vélos** entre 6 h et 9 h (somme, fourchette sur les 6 jours), et
   Gare Centrale 2 en porte 55 %. Un plancher posé à 8 h vaut donc, pour cette station, ≈ 10 sur 21
   alors que le départ du matin se joue à 15-20 : il laisse repartir des vélos qui manqueront
   avant 9 h.

2. **Le plancher était contourné quand le besoin était un retrait.** Dans le planificateur de
   transferts, `give = s.need < 0 ? -s.need : … - keep` : dès qu'une station était étiquetée
   « surplus » par la cible 50 %, `keep` n'était **pas** appliqué. Le retrait n'était borné que par
   la moitié (garde de l'aller-retour), sans aucune référence au lendemain.

3. **Le plancher ne valait que pour `sens > 0`, c'est-à-dire jamais le week-end.** Un dimanche soir
   n'a pas de motif pendulaire (`sensJour` rend 0) alors que le lundi matin est un pic de départs :
   aucun plancher ne s'appliquait.

4. **Deux chiffres pour la même chose.** La fiche station affichait `need` (cible 50 %, seulement
   divisée par deux) tandis que le planificateur, lui, respectait `keep`. Le conseil affiché
   pouvait donc dépasser ce que la tournée prenait.

## Ce qui change

- `Model::plancherDemain(i, now)` (`model.cpp`, `model.h`) : `max(60 % de la capacité utile, niveau
  du profil demain au PIC DU MATIN — 6 h, l'ancre de `sensJour`)`, lu sur **demain** (donc valable
  un dimanche soir) et seulement si demain est un jour ouvré.
- Le retrait **affiché** (`need < 0`) est borné par ce plancher.
- Le planificateur de transferts utilise la même fonction et **applique la borne à tous les
  prélèvements**, y compris ceux décidés par `need < 0`.
- La garde de l'aller-retour (moitié à partir de midi) est conservée : le plancher est une borne
  basse, pas un remplacement.

## Effet mesuré

- **Banc** (`sim/test_core`, nouveau `test_plancher_retrait.inc`, 14 vérifications) : station de
  30 bornes à 28 vélos à 20 h 30, formule nue −13. Ancienne règle : **−7** (il resterait 21 vélos,
  sous le pic de 24 du lendemain). Nouvelle règle : **−4**, et le transfert prélève 4. Dimanche
  soir, même station : ancienne **−13**, nouvelle **−4**.
- **Rejeu sur l'historique réel** (5 soirées ouvrées, 3 → 10/11/2026, 145 stations) : 69
  station-soirées en surplus (au-dessus de 50 % de capacité à 20 h 30). L'ancienne règle conseillait
  de retirer **132 vélos**, la nouvelle **81** — 51 vélos retenus, sur 39 station-soirées (57 %).
  Dans **21 cas**, l'ancien retrait laissait la station sous son niveau de 6 h du lendemain.
  *Réserve de méthode* : ce rejeu remplace la prévision par la valeur courante et le profil appris
  par le niveau **réellement constaté** à 6 h le lendemain. C'est un proxy mesuré, pas le profil —
  il donne l'ordre de grandeur et le sens, pas le chiffre exact que produira la carte.
- `./sim ../docs/donnees ops` : audit tactile 386 OK / 0 KO. `pio run` env **c** et **r** :
  SUCCESS. `test_core` : **1 301 vérifications, 0 échec**.

## Ce qui n'est pas fait — la chaîne de publication

Cette branche **n'est pas publiée, ni flashée**. Pour livrer, il faut, dans cet ordre :
bump `VN_VERSION` (1.7.2) dans `src/config.h`, **reconstruire les deux moteurs wasm**
(`online/public/vn-core.wasm`, `vn-twin.wasm`) — sinon `test/binaires.mjs` refuse la suite et le
serveur continue de calculer avec l'ancienne règle —, pousser et laisser la CI construire, puis
flasher après sauvegarde vérifiée de la flash.

## Reste ouvert (non traité ici)

Trois « planchers » coexistent encore pour la même question : `need` sur 50 % de capacité (carte),
`plancherDemain` (planificateur carte), et `keep = max(2, 30 % de capacité)` dans
`online/lib/planner.mjs`, qui **ignore complètement le motif pendulaire**. La cible unique — celle
que le banc de nuit utilise déjà (`sim/bench_prevision.cpp`, 70 % vers le profil) — reste à
extraire en une fonction `cible(s, t)` partagée par les trois.
