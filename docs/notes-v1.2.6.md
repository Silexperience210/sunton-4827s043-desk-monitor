## Ce qui change

Cette version rassemble deux chantiers : **l'onglet Service** (dont les chiffres venaient d'être
audités) et **le parc estimé** du KPI « vélos en trajet ».

### Onglet Service

Sept défauts de chiffres corrigés après une revue qui a confronté `docs/vue-service.md` au code :

- le volet « La carte » **vieillit tout seul** — hors ligne, il restait figé sur « reçu à l'instant ·
  FRAIS · OK », c'est-à-dire sur le panneau chargé de prouver la fraîcheur ;
- le badge ne dit plus **COMPLET** quand aucun relevé n'a eu lieu dans la journée ;
- les comparaisons de ruptures **donnent le nombre de jours de chaque côté** (« 237 (7 j) contre
  212 (6 j) ») et écrivent « non relevé » au lieu d'un zéro inventé ;
- la courbe « 30 j » couvre exactement la **même fenêtre** que la tuile (elle en additionnait 29) ;
- un jour relevé seulement hors 7 h–22 h **n'est plus un jour relevé** et devient un trou ;
- « % des tranches » précise qu'il porte sur les jours relevés ; une **définition unique de « jour
  relevé »** (une heure pleinement relevée) est partagée par la Synthèse, la courbe et les récurrents ;
- « 6 week-ends/fériés sur 6 » devient « 6 j de week-end/férié sur 6 » — c'étaient des jours.

Précisions de sens : la série d'un problème récurrent s'écrit « série 12 j » avec la règle qui dit
qu'elle compte des jours **relevés**, et le classement affiche « N classées, M écartées » (inactives
comprises). Le tableau des états ne promet plus qu'on peut rendre quand il n'y a ni vélo louable ni
borne libre — c'est le texte qui est corrigé, la classification n'a pas changé (la changer modifiait
les sommes des KPI).

### Parc estimé

- **la profondeur d'historique est réellement observée** (tranches renseignées et comparables, trous
  et coupures exclus) : une carte éteinte 12 h n'affiche plus 12 h d'historique sur trois relevés ;
- **la couverture est comparée dans les deux sens** : trois stations retirées du référentiel ne
  peuvent plus faire annoncer « 94 vélos en trajet, validés » pour 49 réels ;
- **le parc est un maximum filtré, prudent** : la troisième plus grande valeur comparable écarte un
  relevé isolé ou double anormalement haut, et le parc ne peut jamais être inférieur aux vélos garés ;
- la valeur est présentée comme une **estimation**, pas comme un plancher : deux artefacts
  indépendants peuvent encore la surestimer, et une valeur qui se prétend être une borne basse tout en
  pouvant être fausse serait pire que l'estimation assumée ;
- la tranche de référence est **datée de son âge** (« Réf. 00:30 (−15 h) »).

### Lisibilité

Les fiches n'affichent plus de texte corrompu (« Rß. » au lieu de « Réf. », « RÞLLEMENT » au lieu de
« RÉELLEMENT ») : la séquence d'échappement `\xA9` avalait la lettre suivante dans le binaire livré.

## Ce qui est vérifié

Banc du simulateur : **987 vérifications, 0 échec**, dont les bornes exactes des règles (3 tranches
vides pour un état touché, 6 tranches pour un créneau relevé, 6 h 55 → 7 h), le jour relevé seulement
la nuit, la station écartée du classement, la station fantôme, la couverture réduite, la coupure de
12 h et le recul d'horloge. Les deux firmwares compilent ; scénario `ops` du simulateur au vert.

Revues indépendantes successives (quatre passes, en lecture seule) : elles ont trouvé, entre autres,
le plancher faux, la ligne de récurrent tronquée, l'axe « auj. » sous la barre d'hier, le parc
inférieur aux vélos garés, et trois tests qui ne mordaient pas. Ce qui n'est **pas** couvert : rien
n'a été mesuré sur la carte pour cette version avant publication, et 9 mutations du détecteur de
récurrents restent sans test.

## Mise à jour

**1.2.2 → 1.2.6 : aucune migration.** Aucun format de fichier ne change (`/vn_hist.bin` v2,
`/vn_prof.bin` v4, `/vn_days.bin` v1) : flash par-dessus, sans effacement, historique et profils
conservés. Les versions 1.2.3 à 1.2.5 n'ont jamais été publiées.
