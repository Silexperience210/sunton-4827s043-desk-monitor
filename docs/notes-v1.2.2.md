## Ce qui change

**« Vélos en trajet » : une profondeur d'historique réellement observée, et une référence datée.**

Signalé par le propriétaire : « le device était hors tension 12 h ». La validité du chiffre et la
ligne `hist. Xh YY` venaient de l'**écart d'horloge** entre la tranche la plus ancienne et la plus
récente de l'anneau : une carte éteinte douze heures affichait donc douze heures d'historique avec
trois relevés, et pouvait se déclarer validée dessus. Une coupure n'est pas une observation.

- la profondeur est celle des tranches **réellement renseignées et comparables** à la population
  courante (× 5 min ; trous, coupures et changements de couverture exclus) ;
- validé à **6 h observées** *et* un creux ; **le seuil du provisoire ne bouge pas** — cette version
  ne change que la mesure, aucune décision produit n'est prise au passage ;
- la tranche de référence est **datée** quand elle vient d'un autre jour : `Réf. 04:55 (veille)`.

Trois autres endroits mentaient de la même façon, corrigés après une revue indépendante :

- **fiche « Variation sur 1 h »** : « Historique disponible : 1440 min » après une coupure, pour
  145 tranches réellement observées — elle affiche désormais la profondeur observée ;
- **tuile d'activité du tableau de bord** : « 1 h : 0 retraits · 0 retours » dès le premier relevé
  après une coupure, le critère comptant des lignes d'anneau au lieu de tranches renseignées — il
  en faut maintenant douze pleines ;
- **quart « calme »** : mesuré sur les mêmes tranches que le parc (couverture égale), pour qu'un
  moment calme pour trois stations ne valide pas un parc calculé sur six.

Un **recul d'horloge** (resynchronisation NTP vers l'arrière) n'inscrit plus deux fois la même
tranche, ce qui gonflait la profondeur.

## Ce qui est vérifié

Banc du simulateur : **965 vérifications, 0 échec**, dont cinq nouvelles qui figent les cas — coupure
(trois relevés espacés de 6 h → ni validé ni provisoire, 15 min d'historique), couverture inégale,
recul d'horloge, seuil du provisoire inchangé, référence datée « (veille) ». Les deux firmwares
compilent ; scénario `ops` du simulateur au vert (captures des écrans comprises).

La revue indépendante (modèle d'une autre famille, en lecture seule) a produit le constat ; ses
points sont repris un par un ci-dessus. Ce qu'elle n'a **pas** pu vérifier : le comportement sur la
carte (toute la démonstration vient du code et du simulateur), la fréquence réelle des trous de
tranche quand un cycle HTTP traîne, et le rendu à l'écran de la fiche.

## Mise à jour

**1.2.1 → 1.2.2 : aucune migration.** Aucun format de fichier ne change (`/vn_hist.bin` reste en v2,
`/vn_prof.bin` en v4) : flasher par-dessus, sans effacement, retrouve l'historique, les profils et
les résumés tels quels.

## Point laissé au propriétaire

Après une longue coupure, la référence peut avoir douze heures ou plus : elle est maintenant datée,
mais la validation repose encore sur des données d'avant la coupure. Exiger une part récente (par
exemple une heure de relevés dans les deux dernières heures) retiendrait le chiffre plus longtemps —
c'est un arbitrage d'exploitation, pas une correction, et il n'est pas tranché ici.
