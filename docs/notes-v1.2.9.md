## Ce qui change

La 1.2.8 a été relue après publication : **deux défauts bloquants** et deux défauts de présentation y
étaient, tous introduits par les corrections du jour. Cette version les corrige.

### Le tableau de bord lisait hors du tableau

En donnant à l'alerte réseau le risque 100, elle est entrée pour de bon dans les cinq « priorités » du
tableau de bord — un chemin qui, lui, n'avait pas de garde : `st[-1]`, soit 408 octets avant le bloc de
stations, dans l'en-tête du tas PSRAM. AddressSanitizer le voit à chaque rafraîchissement ; sur la
carte, la ligne affichait un nom illisible et un badge pris au hasard. Garde posée : « Réseau », sans
besoin de livraison.

### La fiche station s'écrivait dans son propre tampon

`snprintf(b, sizeof(b), "Dernière : %s", b)` : source et destination confondues, comportement indéfini
(gcc le disait : `-Wrestrict`). Résultat à l'écran : « Dernière : Dernière : → 15 h 40 », sans le type
ni l'heure de début. Et le bilan du jour, écrit dans le même libellé, était écrasé juste après — il ne
s'affichait jamais. Deux libellés, deux tampons.

### Le besoin de livraison passe avant le badge « +N »

Une station vide qui cumulait quatre vélos hors service affichait « +1 » au lieu de « +9 vélos » : le
chiffre utile à l'exploitant. Le besoin est prioritaire, « +N » seulement à défaut.

### Une station suspendue n'ouvre plus d'épisode

`classify()` la donne toujours « vide » (zéro vélo louable), donc elle déclenchait le toast « vient de
se vider », ouvrait un épisode au journal et réclamait des vélos dans le plan de régulation — pour une
station où l'on ne peut pas louer. Le journal et le toast l'ignorent désormais.

### Et deux justesses de plus

Le journal passe à **1 024 épisodes** (200 couvraient 4 à 6 h, pas une journée : mesuré 262 épisodes par
jour) ; le bilan du jour compte les épisodes **à cheval sur minuit** ; et l'alerte « flux figé » dit les
minutes depuis le dernier mouvement plutôt que depuis notre dernier téléchargement.

## Ce qui est vérifié

Banc : **1 080 vérifications, 0 échec**, dont la raison et la gravité des arrêts, le journal d'une
station suspendue, les épisodes à cheval sur minuit, et le « +N » qui ne masque plus le besoin.
ASan et UBSan propres sur le scénario « deux ruptures + parc hors service » qui déclenchait le
débordement du tableau de bord.

**Non couvert** : rien n'a été mesuré sur la carte pour cette version avant publication ; le journal
reste en PSRAM (perdu au redémarrage) ; et le plafond de 96 alertes affichées reste un choix — les
compteurs, eux, sont vrais.

## Mise à jour

Aucune migration de fichier. Flash par-dessus, sans effacement.
